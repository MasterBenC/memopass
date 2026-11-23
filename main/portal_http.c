#include "portal_server.h"

#include "app_config.h"
#include "app_net.h"
#include "feishu_formatter.h"
#include "feishu_pusher.h"
#include "meeting_fsm.h"
#include "mp_utf8.h"
#include "portal_page.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "memopass_portal";
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns;
static volatile bool s_dns_run;
static char s_ssid[20];
#define PORTAL_BODY_MAX 1400
/* s_test_state, s_test_msg and s_test_form are shared between the HTTPD task
 * (test_post) and test_task; every access goes through s_test_mu. */
static SemaphoreHandle_t s_test_mu;
static volatile int s_test_state;
static char s_test_msg[96];
static portal_form_t s_test_form;

/* Request scratch kept off the 6 KB HTTPD stack. */
typedef struct {
    char raw[PORTAL_BODY_MAX];
    portal_form_t form;
    app_config_t cfg;
} portal_req_buf_t;

static void test_lock(void) {
    if (s_test_mu) xSemaphoreTake(s_test_mu, portMAX_DELAY);
}

static void test_unlock(void) {
    if (s_test_mu) xSemaphoreGive(s_test_mu);
}

static void test_finish(int state, const char *msg) {
    test_lock();
    snprintf(s_test_msg, sizeof(s_test_msg), "%s", msg);
    s_test_state = state;
    test_unlock();
}

static void dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        s_dns = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        s_dns = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint32_t ip = inet_addr("192.168.4.1");
    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n < 12) continue;
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[6] = 0;
        buf[7] = 1;
        int i = 12;
        while (i < n && buf[i] != 0) {
            int jump = buf[i];
            if (jump <= 0 || i + jump >= n) break;
            i += jump + 1;
        }
        if (i >= n || buf[i] != 0) continue;
        i += 5;
        if (i + 16 > (int)sizeof(buf)) continue;
        buf[i++] = 0xC0;
        buf[i++] = 0x0C;
        buf[i++] = 0x00;
        buf[i++] = 0x01;
        buf[i++] = 0x00;
        buf[i++] = 0x01;
        buf[i++] = 0x00;
        buf[i++] = 0x00;
        buf[i++] = 0x00;
        buf[i++] = 30;
        buf[i++] = 0x00;
        buf[i++] = 0x04;
        memcpy(buf + i, &ip, 4);
        i += 4;
        sendto(sock, buf, i, 0, (struct sockaddr *)&from, fromlen);
    }
    close(sock);
    s_dns = NULL;
    vTaskDelete(NULL);
}

static int json_escape(char *out, size_t cap, const char *in) {
    size_t o = 0;
    if (!out || cap == 0) return -1;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = (char)c;
            continue;
        }
        if (c < 0x20) continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return (int)o;
}

static esp_err_t send_json(httpd_req_t *req, bool ok, const char *message) {
    char esc[192];
    char body[256];
    int n = json_escape(esc, sizeof(esc), message ? message : "");
    /* json_escape may stop mid character when it runs out of room. */
    if (n > 0) esc[mp_utf8_fit(esc, (size_t)n, (size_t)n)] = '\0';
    snprintf(body, sizeof(body), "{\"ok\":%s,\"message\":\"%s\"}", ok ? "true" : "false",
             esc);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t redirect_root(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t page_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)PORTAL_PAGE_GZ, PORTAL_PAGE_GZ_LEN);
}

static esp_err_t scan_get(httpd_req_t *req) {
    char ssids[8][33];
    int rssi[8];
    int n = app_net_scan_cached(ssids, rssi, 8);
    char body[768];
    size_t used = 0;
    used += (size_t)snprintf(body, sizeof(body), "{\"nets\":[");
    for (int i = 0; i < n && used + 80 < sizeof(body); i++) {
        char esc[48];
        json_escape(esc, sizeof(esc), ssids[i]);
        used += (size_t)snprintf(body + used, sizeof(body) - used,
                                 "%s{\"ssid\":\"%s\",\"rssi\":%d}", i ? "," : "", esc, rssi[i]);
    }
    snprintf(body + used, sizeof(body) - used, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* Reads the whole request body into raw (NUL not required). Returns the
 * length, or -1 if it is empty, too large or the socket fails. */
static int read_body(httpd_req_t *req, char *raw, size_t cap) {
    int left = req->content_len;
    size_t used = 0;
    if (left <= 0 || left >= (int)cap) return -1;
    while (left > 0) {
        int n = httpd_req_recv(req, raw + used, (size_t)left);
        if (n <= 0) return -1;
        used += (size_t)n;
        left -= n;
    }
    return (int)used;
}

static esp_err_t save_post(httpd_req_t *req) {
    portal_req_buf_t *buf = calloc(1, sizeof(*buf));
    if (!buf) return send_json(req, false, meeting_fsm_error_text(MP_ERR_NET));
    int used = read_body(req, buf->raw, sizeof(buf->raw));
    if (used < 0 || portal_form_parse(buf->raw, (size_t)used, &buf->form) != 0) {
        free(buf);
        return send_json(req, false, "请检查必填项");
    }
    if (buf->form.factory) {
        free(buf);
        app_config_clear();
        send_json(req, true, "已恢复，设备正在重启");
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
        return ESP_OK;
    }
    if (app_config_load(&buf->cfg) != ESP_OK) memset(&buf->cfg, 0, sizeof(buf->cfg));
    portal_form_merge(&buf->form, &buf->cfg);
    bool saved = app_config_ready(&buf->cfg) && app_config_save(&buf->cfg) == ESP_OK;
    free(buf);
    if (!saved) return send_json(req, false, "请检查必填项");
    send_json(req, true, "已保存，设备正在联网");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

static void test_task(void *arg) {
    (void)arg;
    portal_req_buf_t *buf = calloc(1, sizeof(*buf));
    if (!buf) {
        test_finish(2, meeting_fsm_error_text(MP_ERR_NET));
        vTaskDelete(NULL);
        return;
    }
    app_config_t *cfg = &buf->cfg;
    if (app_config_load(cfg) != ESP_OK) memset(cfg, 0, sizeof(*cfg));
    test_lock();
    buf->form = s_test_form;
    test_unlock();
    portal_form_merge(&buf->form, cfg);
    if (!feishu_url_ok(cfg->webhook) || !cfg->ssid[0]) {
        free(buf);
        test_finish(2, meeting_fsm_error_text(MP_ERR_FEISHU));
        vTaskDelete(NULL);
        return;
    }
    if (app_net_portal_join(cfg->ssid, cfg->pass) != ESP_OK) {
        free(buf);
        test_finish(2, meeting_fsm_error_text(MP_ERR_NET));
        vTaskDelete(NULL);
        return;
    }
    bool up = false;
    for (int i = 0; i < 50; i++) {
        if (app_net_sta_up()) {
            up = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_err_t err = ESP_FAIL;
    if (up) err = feishu_pusher_once(cfg, "MemoPass 测试消息");
    app_net_portal_leave();
    free(buf);
    test_finish(err == ESP_OK ? 1 : 2,
                err == ESP_OK ? "测试已发送" : meeting_fsm_error_text(MP_ERR_FEISHU));
    vTaskDelete(NULL);
}

static esp_err_t test_post(httpd_req_t *req) {
    portal_req_buf_t *buf = calloc(1, sizeof(*buf));
    if (!buf) return send_json(req, false, meeting_fsm_error_text(MP_ERR_NET));
    int used = read_body(req, buf->raw, sizeof(buf->raw));
    if (used < 0 || portal_form_parse(buf->raw, (size_t)used, &buf->form) != 0) {
        free(buf);
        return send_json(req, false, "请检查必填项");
    }
    test_lock();
    if (s_test_state < 0) {
        test_unlock();
        free(buf);
        return send_json(req, false, "正在发送测试消息");
    }
    s_test_form = buf->form;
    s_test_state = -1;
    s_test_msg[0] = '\0';
    test_unlock();
    free(buf);
    if (xTaskCreate(test_task, "mp_test", 6144, NULL, 4, NULL) != pdPASS) {
        test_finish(2, meeting_fsm_error_text(MP_ERR_NET));
        return send_json(req, false, meeting_fsm_error_text(MP_ERR_NET));
    }
    int state = -1;
    char msg[sizeof(s_test_msg)];
    for (int i = 0; i < 70; i++) {
        test_lock();
        state = s_test_state;
        test_unlock();
        if (state >= 0) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    test_lock();
    state = s_test_state;
    snprintf(msg, sizeof(msg), "%s", s_test_msg[0] ? s_test_msg : "测试失败");
    test_unlock();
    return send_json(req, state == 1, msg);
}

static esp_err_t err_404(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    return redirect_root(req);
}

static void scan_task(void *arg) {
    (void)arg;
    for (int i = 0; i < 13 && s_dns_run; i++) {
        app_net_scan_refresh();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    while (s_dns_run) {
        wifi_sta_list_t stas = {0};
        if (esp_wifi_ap_get_sta_list(&stas) != ESP_OK || stas.num == 0) app_net_scan_refresh();
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    vTaskDelete(NULL);
}

esp_err_t portal_start(const char *ap_ssid) {
    if (s_httpd) return ESP_OK;
    if (!s_test_mu) s_test_mu = xSemaphoreCreateMutex();
    if (!s_test_mu) return ESP_ERR_NO_MEM;
    esp_err_t err = app_net_start_portal(s_ssid, sizeof(s_ssid));
    if (err != ESP_OK) return err;
    if (ap_ssid && ap_ssid[0]) snprintf(s_ssid, sizeof(s_ssid), "%s", ap_ssid);
    s_dns_run = true;
    if (xTaskCreate(dns_task, "mp_dns", 3072, NULL, 4, &s_dns) != pdPASS) return ESP_ERR_NO_MEM;
    xTaskCreate(scan_task, "mp_scan", 4096, NULL, 3, NULL);
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 12;
    cfg.send_wait_timeout = 12;
    err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;
    httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_get};
    httpd_uri_t scan = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    httpd_uri_t test = {.uri = "/test", .method = HTTP_POST, .handler = test_post};
    httpd_register_uri_handler(s_httpd, &page);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &test);
    const char *caps[] = {"/generate_204", "/gen_204", "/hotspot-detect.html",
                          "/ncsi.txt", "/connecttest.txt", "/fwlink", "/success.txt"};
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        httpd_uri_t redir = {.uri = caps[i], .method = HTTP_GET, .handler = redirect_root};
        httpd_register_uri_handler(s_httpd, &redir);
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, err_404);
    ESP_LOGI(TAG, "portal page on 192.168.4.1");
    return ESP_OK;
}

void portal_stop(void) {
    s_dns_run = false;
    for (int i = 0; i < 20 && s_dns; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    app_net_stop();
}

bool portal_running(void) { return s_httpd != NULL; }
const char *portal_ap_ssid(void) { return s_ssid; }
