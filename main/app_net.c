#include "app_net.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "memopass_net";
static esp_netif_t *s_ap;
static esp_netif_t *s_sta;
static bool s_inited;
static bool s_started;
static volatile bool s_sta_up;
static volatile bool s_portal;
static bool s_sntp;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_up = false;
        if (!s_portal) esp_wifi_connect();
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    if (id != IP_EVENT_STA_GOT_IP) return;
    s_sta_up = true;
    if (!s_sntp) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_init();
        setenv("TZ", "CST-8", 1);
        tzset();
        s_sntp = true;
    }
}

static esp_err_t ensure_init(void) {
    if (s_inited) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL);
    s_inited = true;
    return ESP_OK;
}

static void set_ap_ip(void) {
    esp_netif_ip_info_t info;
    IP4_ADDR(&info.ip, 192, 168, 4, 1);
    IP4_ADDR(&info.gw, 192, 168, 4, 1);
    IP4_ADDR(&info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap);
    esp_netif_set_ip_info(s_ap, &info);
    esp_netif_dhcps_start(s_ap);
}

esp_err_t app_net_start_portal(char *ssid, size_t ssid_cap) {
    if (s_started && !s_portal) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ensure_init();
    if (err != ESP_OK) return err;
    if (!s_ap) s_ap = esp_netif_create_default_wifi_ap();
    if (!s_ap) return ESP_ERR_NO_MEM;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char name[16];
    snprintf(name, sizeof(name), "MemoPass-%02X%02X", mac[4], mac[5]);
    if (s_started) esp_wifi_stop();
    set_ap_ip();
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, name, strlen(name));
    ap.ap.ssid_len = (uint8_t)strlen(name);
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 2;
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_started = true;
    s_portal = true;
    s_sta_up = false;
    if (ssid && ssid_cap) snprintf(ssid, ssid_cap, "%s", name);
    ESP_LOGI(TAG, "portal %s", name);
    return ESP_OK;
}

esp_err_t app_net_start_sta(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0] || s_portal) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ensure_init();
    if (err != ESP_OK) return err;
    if (!s_sta) s_sta = esp_netif_create_default_wifi_sta();
    if (!s_sta) return ESP_ERR_NO_MEM;
    if (s_started) esp_wifi_stop();
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    if (pass) strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) return err;
    s_started = true;
    s_portal = false;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return ESP_OK;
}

void app_net_stop(void) {
    if (s_started) esp_wifi_stop();
    s_started = false;
    s_portal = false;
    s_sta_up = false;
}

bool app_net_sta_up(void) { return s_sta_up; }
bool app_net_portal_up(void) { return s_portal; }

uint32_t app_net_clock(void) {
    time_t now = 0;
    time(&now);
    if (now < 1700000000) return 0;
    return (uint32_t)now;
}

esp_err_t app_net_portal_join(const char *ssid, const char *pass) {
    if (!s_portal || !ssid || !ssid[0]) return ESP_ERR_INVALID_STATE;
    if (!s_sta) s_sta = esp_netif_create_default_wifi_sta();
    if (!s_sta) return ESP_ERR_NO_MEM;
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    if (pass) strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    s_sta_up = false;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err == ESP_OK) err = esp_wifi_connect();
    return err;
}

void app_net_portal_leave(void) {
    if (!s_portal) return;
    esp_wifi_disconnect();
    s_sta_up = false;
    esp_wifi_set_mode(WIFI_MODE_AP);
}

#define SCAN_CACHE 12
static char s_scan_ssid[SCAN_CACHE][33];
static int s_scan_rssi[SCAN_CACHE];
static int s_scan_n;
static uint8_t s_scan_ch = 1;

static void cache_put(const char *ssid, int rssi) {
    if (!ssid || !ssid[0]) return;
    for (int i = 0; i < s_scan_n; i++) {
        if (strcmp(s_scan_ssid[i], ssid) == 0) {
            s_scan_rssi[i] = rssi;
            return;
        }
    }
    if (s_scan_n < SCAN_CACHE) {
        snprintf(s_scan_ssid[s_scan_n], 33, "%s", ssid);
        s_scan_rssi[s_scan_n] = rssi;
        s_scan_n++;
        return;
    }
    int weak = 0;
    for (int i = 1; i < SCAN_CACHE; i++) {
        if (s_scan_rssi[i] < s_scan_rssi[weak]) weak = i;
    }
    if (rssi <= s_scan_rssi[weak]) return;
    snprintf(s_scan_ssid[weak], 33, "%s", ssid);
    s_scan_rssi[weak] = rssi;
}

int app_net_scan_refresh(void) {
    if (!s_started) return 0;
    wifi_scan_config_t scan = {0};
    scan.channel = s_scan_ch;
    scan.scan_time.active.min = 0;
    scan.scan_time.active.max = 90;
    s_scan_ch = (uint8_t)(s_scan_ch >= 13 ? 1 : s_scan_ch + 1);
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK && s_portal) {
        wifi_mode_t mode = WIFI_MODE_AP;
        esp_wifi_get_mode(&mode);
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        err = esp_wifi_scan_start(&scan, true);
        wifi_sta_list_t stas = {0};
        if ((esp_wifi_ap_get_sta_list(&stas) != ESP_OK || stas.num == 0) && mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan ch %u failed: %s", (unsigned)scan.channel, esp_err_to_name(err));
        return s_scan_n;
    }
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 16) count = 16;
    wifi_ap_record_t recs[16];
    uint16_t got = count;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) return s_scan_n;
    for (int i = 0; i < (int)got; i++) cache_put((const char *)recs[i].ssid, recs[i].rssi);
    return s_scan_n;
}

int app_net_scan_cached(char ssids[][33], int *rssi, int max_n) {
    if (!ssids || !rssi || max_n <= 0) return 0;
    int n = s_scan_n < max_n ? s_scan_n : max_n;
    for (int i = 0; i < n; i++) {
        snprintf(ssids[i], 33, "%s", s_scan_ssid[i]);
        rssi[i] = s_scan_rssi[i];
    }
    return n;
}

int app_net_scan(char ssids[][33], int *rssi, int max_n) {
    app_net_scan_refresh();
    return app_net_scan_cached(ssids, rssi, max_n);
}
