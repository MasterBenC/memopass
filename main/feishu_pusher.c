#include "feishu_pusher.h"

#include "app_net.h"
#include "feishu_formatter.h"
#include "volc_asr_client.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "memopass_push";
#define PUSH_SLOTS 3

/* A second TLS session (HTTPS beside the live WSS) needs roughly 30-40 KB at
 * handshake peak with CONFIG_MBEDTLS_DYNAMIC_BUFFER. Below these marks we back
 * off instead of starting a handshake that is likely to fail with ENOMEM. */
#define PUSH_HEAP_MIN_FREE (40 * 1024)
#define PUSH_HEAP_MIN_BLOCK (16 * 1024)
#define PUSH_HEAP_BACKOFF_MS 500
#define PUSH_HEAP_BACKOFF_MAX_MS 4000
#define PUSH_HEAP_WAIT_MAX_MS 12000
/* Never overlap our handshake with the ASR WSS handshake for longer than this. */
#define PUSH_ASR_HANDSHAKE_WAIT_MS 4000
/* Signed bots need a real clock; Feishu rejects timestamps off by > 1 hour. */
#define PUSH_CLOCK_WAIT_MS 30000
#define PUSH_CLOCK_WAIT_ONCE_MS 6000
#define PUSH_CLOCK_POLL_MS 500

static feishu_seg_t s_slots[PUSH_SLOTS];
static QueueHandle_t s_free;
static QueueHandle_t s_used;
static TaskHandle_t s_task;
static const app_config_t *s_cfg;
static volatile bool s_run;
static volatile int s_sent;
static volatile int s_failed;
static char s_body[4096];
/* Short Feishu JSON only (code/msg). Never log this buffer. */
static char s_resp[256];
static size_t s_resp_len;

static void log_heap(const char *when) {
    ESP_LOGI(TAG, "heap %s: free=%u min=%u largest=%u", when,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static bool heap_ok(void) {
    return esp_get_free_heap_size() >= PUSH_HEAP_MIN_FREE &&
           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= PUSH_HEAP_MIN_BLOCK;
}

/* Waits (bounded) until the ASR handshake is over and the heap has room for a
 * second TLS session. Proceeds anyway when the wait runs out, so a segment is
 * never dropped only because the heap stayed tight. */
static void wait_tls_room(void) {
    uint32_t waited = 0;
    while (volc_asr_connecting() && waited < PUSH_ASR_HANDSHAKE_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
    if (waited) ESP_LOGI(TAG, "waited %u ms for ASR handshake", (unsigned)waited);
    uint32_t backoff = PUSH_HEAP_BACKOFF_MS;
    waited = 0;
    while (!heap_ok() && waited < PUSH_HEAP_WAIT_MAX_MS) {
        ESP_LOGW(TAG, "heap tight before HTTPS (free=%u largest=%u), back off %u ms",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned)backoff);
        vTaskDelay(pdMS_TO_TICKS(backoff));
        waited += backoff;
        backoff = backoff * 2 > PUSH_HEAP_BACKOFF_MAX_MS ? PUSH_HEAP_BACKOFF_MAX_MS : backoff * 2;
    }
    if (!heap_ok()) ESP_LOGW(TAG, "heap still tight after %u ms, trying anyway", (unsigned)waited);
}

/* Returns a synced unix time, waiting up to max_ms for SNTP. 0 = not synced. */
static uint32_t wait_clock(uint32_t max_ms) {
    uint32_t now = app_net_clock();
    uint32_t waited = 0;
    if (now) return now;
    ESP_LOGW(TAG, "SNTP not synced; signed webhook needs real time, waiting up to %u ms",
             (unsigned)max_ms);
    while (!now && waited < max_ms) {
        vTaskDelay(pdMS_TO_TICKS(PUSH_CLOCK_POLL_MS));
        waited += PUSH_CLOCK_POLL_MS;
        now = app_net_clock();
        if (!now && waited % 5000 == 0) {
            ESP_LOGW(TAG, "still waiting for SNTP (%u ms)", (unsigned)waited);
        }
    }
    if (now) ESP_LOGI(TAG, "SNTP synced after %u ms", (unsigned)waited);
    return now;
}

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    if (!evt || evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    size_t room = sizeof(s_resp) - 1 - s_resp_len;
    size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    if (n) {
        memcpy(s_resp + s_resp_len, evt->data, n);
        s_resp_len += n;
        s_resp[s_resp_len] = '\0';
    }
    return ESP_OK;
}

static bool push_http_ok(esp_err_t err, int status, int *biz_code) {
    int code = 0;
    bool got = feishu_parse_response_code(s_resp, s_resp_len, &code);
    if (biz_code) *biz_code = got ? code : 0;
    if (err != ESP_OK || status < 200 || status >= 300) return false;
    return !got || code == 0;
}

static esp_err_t post_body(const char *url, const char *body, int *status) {
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_event,
    };
    s_resp_len = 0;
    s_resp[0] = '\0';
    wait_tls_room();
    log_heap("before https");
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        log_heap("https init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    if (status) *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    log_heap("after https");
    if (err != ESP_OK) ESP_LOGW(TAG, "https err=%s", esp_err_to_name(err));
    return err;
}

/* clock_wait_ms bounds the SNTP wait when a signing secret is configured. */
static bool format_seg(const feishu_seg_t *seg, char *out, size_t cap, uint32_t clock_wait_ms) {
    bool with_sign = s_cfg->secret[0] != '\0';
    uint32_t now = app_net_clock();
    if (with_sign && !now) {
        now = wait_clock(clock_wait_ms);
        if (!now) {
            /* Never sign with a made-up time: Feishu would reject it anyway. */
            ESP_LOGE(TAG, "skip push seg=%d: SNTP not synced, cannot compute Feishu sign",
                     seg->segment);
            return false;
        }
    }
    feishu_job_t job = {
        .meeting_id = seg->meeting_id,
        .segment = seg->segment,
        .is_final = seg->is_final,
        .duration_sec = seg->duration_sec,
        .text = seg->text,
        .text_len = seg->text_len,
        .with_sign = with_sign,
        .unix_time = now,
        .secret = s_cfg->secret,
    };
    feishu_kind_t kind = feishu_kind_from_url(s_cfg->webhook);
    return feishu_format(kind, &job, out, cap) > 0;
}

static void push_task(void *arg) {
    (void)arg;
    while (s_run || uxQueueMessagesWaiting(s_used) > 0) {
        feishu_seg_t *seg = NULL;
        if (xQueueReceive(s_used, &seg, pdMS_TO_TICKS(200)) != pdTRUE) {
            if (!s_run) break;
            continue;
        }
        bool ok = false;
        if (format_seg(seg, s_body, sizeof(s_body), PUSH_CLOCK_WAIT_MS)) {
            for (int attempt = 0; attempt < 3 && !ok; attempt++) {
                int status = 0;
                int biz = 0;
                esp_err_t err = post_body(s_cfg->webhook, s_body, &status);
                ok = push_http_ok(err, status, &biz);
                if (!ok && attempt < 2) {
                    /* Longer back-off after an allocation failure so the WSS
                     * session can release its buffers before we retry. */
                    uint32_t delay = err == ESP_ERR_NO_MEM || !heap_ok()
                                         ? 1500u * (uint32_t)(attempt + 1)
                                         : 400u * (uint32_t)(attempt + 1);
                    ESP_LOGW(TAG, "push seg=%d attempt=%d status=%d feishu_code=%d, retry in %u ms",
                             seg->segment, attempt + 1, status, biz, (unsigned)delay);
                    vTaskDelay(pdMS_TO_TICKS(delay));
                } else if (!ok) {
                    ESP_LOGW(TAG, "push failed seg=%d status=%d feishu_code=%d err=%s",
                             seg->segment, status, biz, esp_err_to_name(err));
                }
            }
        }
        if (ok) s_sent++;
        else {
            s_failed++;
            if (s_run) ESP_LOGW(TAG, "push failed seg=%d", seg->segment);
        }
        xQueueSend(s_free, &seg, 0);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t feishu_pusher_start(const app_config_t *cfg) {
    if (!cfg || s_task) return ESP_ERR_INVALID_STATE;
    s_cfg = cfg;
    s_sent = 0;
    s_failed = 0;
    s_run = true;
    if (!s_free) s_free = xQueueCreate(PUSH_SLOTS, sizeof(feishu_seg_t *));
    if (!s_used) s_used = xQueueCreate(PUSH_SLOTS, sizeof(feishu_seg_t *));
    if (!s_free || !s_used) return ESP_ERR_NO_MEM;
    xQueueReset(s_free);
    xQueueReset(s_used);
    for (int i = 0; i < PUSH_SLOTS; i++) {
        feishu_seg_t *slot = &s_slots[i];
        xQueueSend(s_free, &slot, 0);
    }
    if (xTaskCreate(push_task, "mp_push", 6144, NULL, 4, &s_task) != pdPASS) {
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void feishu_pusher_stop(void) {
    s_run = false;
    /* push_task drains queued segments first; with the SNTP and heap waits a
     * slot can take tens of seconds, so allow up to 60 s before giving up. */
    for (int i = 0; i < 2400 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(25));
    if (s_task) ESP_LOGW(TAG, "push task still draining after stop");
}

bool feishu_pusher_submit(const feishu_seg_t *seg) {
    feishu_seg_t *slot = NULL;
    if (!seg || !s_free || xQueueReceive(s_free, &slot, 0) != pdTRUE) return false;
    *slot = *seg;
    if (slot->text_len > FEISHU_PUSH_TEXT) slot->text_len = FEISHU_PUSH_TEXT;
    slot->text[slot->text_len] = '\0';
    if (xQueueSend(s_used, &slot, 0) != pdTRUE) {
        xQueueSend(s_free, &slot, 0);
        return false;
    }
    return true;
}

int feishu_pusher_sent(void) { return s_sent; }
int feishu_pusher_failed(void) { return s_failed; }

bool feishu_pusher_idle(void) {
    if (!s_used) return true;
    return uxQueueMessagesWaiting(s_used) == 0 && s_task == NULL;
}

esp_err_t feishu_pusher_once(const app_config_t *cfg, const char *text) {
    if (!cfg || !text) return ESP_ERR_INVALID_ARG;
    s_cfg = cfg;
    /* ~2.5 KB: keep it off the caller's stack, which also has to carry the
     * TLS handshake. Not reentrant, like s_body. */
    static feishu_seg_t seg;
    memset(&seg, 0, sizeof(seg));
    snprintf(seg.meeting_id, sizeof(seg.meeting_id), "test");
    seg.is_final = true;
    seg.text_len = strlen(text);
    if (seg.text_len > FEISHU_PUSH_TEXT) seg.text_len = FEISHU_PUSH_TEXT;
    memcpy(seg.text, text, seg.text_len);
    if (!format_seg(&seg, s_body, sizeof(s_body), PUSH_CLOCK_WAIT_ONCE_MS)) return ESP_FAIL;
    int status = 0;
    int biz = 0;
    esp_err_t err = post_body(cfg->webhook, s_body, &status);
    if (!push_http_ok(err, status, &biz)) {
        ESP_LOGW(TAG, "test/once push failed status=%d feishu_code=%d err=%s",
                 status, biz, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}
