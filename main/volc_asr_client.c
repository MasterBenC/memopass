#include "volc_asr_client.h"

#include "audio_pipeline.h"
#include "volc_asr_json.h"
#include "volc_asr_proto.h"
#include "mp_utf8.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "memopass_asr";
static const char *ASR_URI = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async";

static char s_headers[512];
static esp_websocket_client_handle_t s_ws;
static QueueHandle_t s_used;
static QueueHandle_t s_free;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_mu;
static volatile bool s_run;
static volatile int s_failed;
static volatile bool s_up;
static char s_text[2048];
static size_t s_text_len;
static int32_t s_seq;
static uint8_t s_rx[3072];
static size_t s_rx_len;
static const app_config_t *s_cfg;

static void note_text(const char *incoming, size_t n) {
    if (!s_mu || xSemaphoreTake(s_mu, pdMS_TO_TICKS(40)) != pdTRUE) return;
    volc_text_merge(s_text, sizeof(s_text), &s_text_len, incoming, n);
    xSemaphoreGive(s_mu);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        s_up = true;
        return;
    }
    if (id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_up = false;
        if (s_run && !s_failed) s_failed = 8;
        return;
    }
    if (id == WEBSOCKET_EVENT_ERROR) {
        s_up = false;
        if (data) {
            esp_websocket_event_data_t *event = data;
            int http = event->error_handle.esp_ws_handshake_status_code;
            if (http == 401 || http == 403) s_failed = 401;
            else if (!s_failed) s_failed = 8;
            ESP_LOGW(TAG, "handshake http=%d", http);
        }
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA || !data) return;
    esp_websocket_event_data_t *event = data;
    if (event->op_code != 0x2 || event->data_len <= 0) return;
    if (event->payload_offset == 0) s_rx_len = 0;
    if (s_rx_len + (size_t)event->data_len > sizeof(s_rx)) {
        s_failed = 8;
        return;
    }
    memcpy(s_rx + s_rx_len, event->data_ptr, (size_t)event->data_len);
    s_rx_len += (size_t)event->data_len;
    if (event->payload_offset + event->data_len < event->payload_len) return;
    volc_frame_t frame;
    if (volc_frame_decode(s_rx, s_rx_len, &frame) != 0) return;
    if (frame.message_type == VOLC_MSG_ERROR_SERVER) {
        volc_fail_t kind = volc_asr_classify(frame.error_code,
                                             (const char *)frame.payload);
        s_failed = kind == VOLC_FAIL_AUTH ? 401 : kind == VOLC_FAIL_RESOURCE ? 45000030 : 8;
        return;
    }
    if (frame.serialization != VOLC_SER_JSON || frame.compression != VOLC_COMP_NONE) return;
    char text[768];
    size_t n = volc_json_longest_text((const char *)frame.payload, frame.payload_len,
                                      text, sizeof(text));
    if (n) note_text(text, n);
}

static bool send_frame(uint8_t type, uint8_t flags, uint8_t ser, int32_t seq,
                       const uint8_t *payload, size_t len) {
    static uint8_t frame[AUDIO_PCM_BYTES + 16];
    size_t n = volc_frame_encode(frame, sizeof(frame), type, flags, ser, VOLC_COMP_NONE,
                                 true, seq, false, 0, payload, len);
    if (!n || !s_ws) return false;
    return esp_websocket_client_send_bin(s_ws, (const char *)frame, (int)n,
                                         pdMS_TO_TICKS(2000)) > 0;
}

static bool connect_once(void) {
    char id[33];
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) snprintf(id + i * 2, 3, "%02x", rnd[i]);
    if (volc_asr_headers(s_headers, sizeof(s_headers), s_cfg->app_id, s_cfg->access,
                         s_cfg->api_key_mode ? s_cfg->apikey : "",
                         app_config_resource(s_cfg), id) < 0) {
        return false;
    }
    esp_websocket_client_config_t cfg = {
        .uri = ASR_URI,
        .buffer_size = 2048,
        .task_stack = 5120,
        .headers = s_headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) return false;
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) return false;
    for (int i = 0; i < 40 && s_run && !s_up && !s_failed; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return s_up;
}

static void asr_task(void *arg) {
    (void)arg;
    const char *request = volc_asr_request_json();
    while (s_run && !s_failed) {
        if (!connect_once()) {
            if (s_failed == 401 || s_failed == 45000030) break;
            s_failed = 0;
            if (!s_run) break;
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }
        s_seq = 1;
        if (!send_frame(VOLC_MSG_FULL_CLIENT, VOLC_FLAG_SEQUENCE, VOLC_SER_JSON,
                        s_seq, (const uint8_t *)request, strlen(request))) {
            s_failed = 8;
            break;
        }
        while (s_run && !s_failed && s_up) {
            uint8_t *slot = NULL;
            if (xQueueReceive(s_used, &slot, pdMS_TO_TICKS(200)) != pdTRUE) continue;
            s_seq++;
            if (!send_frame(VOLC_MSG_AUDIO_CLIENT, VOLC_FLAG_SEQUENCE, VOLC_SER_RAW,
                            s_seq, slot, AUDIO_PCM_BYTES)) {
                s_failed = 8;
            }
            xQueueSend(s_free, &slot, 0);
        }
        if (s_ws && s_up) {
            s_seq++;
            send_frame(VOLC_MSG_AUDIO_CLIENT, VOLC_FLAG_SEQUENCE | VOLC_FLAG_LAST,
                       VOLC_SER_RAW, -s_seq, NULL, 0);
        }
        if (s_ws) {
            esp_websocket_client_stop(s_ws);
            esp_websocket_client_destroy(s_ws);
            s_ws = NULL;
        }
        s_up = false;
        if (s_run && s_failed == 8) {
            s_failed = 0;
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }
        break;
    }
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t volc_asr_open(const app_config_t *cfg, QueueHandle_t used, QueueHandle_t freeq) {
    if (!cfg || s_task) return ESP_ERR_INVALID_STATE;
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    if (!s_mu) return ESP_ERR_NO_MEM;
    s_cfg = cfg;
    s_used = used;
    s_free = freeq;
    s_text_len = 0;
    s_text[0] = '\0';
    s_failed = 0;
    s_up = false;
    s_run = true;
    if (xTaskCreate(asr_task, "mp_asr", 5120, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void volc_asr_close(void) {
    s_run = false;
    for (int i = 0; i < 80 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(25));
}

bool volc_asr_copy(char *out, size_t cap, size_t *len) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (len) *len = 0;
    if (!s_mu || xSemaphoreTake(s_mu, pdMS_TO_TICKS(40)) != pdTRUE) return false;
    size_t n = s_text_len < cap ? s_text_len : cap - 1;
    memcpy(out, s_text, n);
    out[n] = '\0';
    if (len) *len = n;
    xSemaphoreGive(s_mu);
    return true;
}

void volc_asr_drop(size_t n) {
    if (!s_mu || xSemaphoreTake(s_mu, pdMS_TO_TICKS(40)) != pdTRUE) return;
    if (n > s_text_len) n = s_text_len;
    n = mp_utf8_fit(s_text, s_text_len, n);
    memmove(s_text, s_text + n, s_text_len - n);
    s_text_len -= n;
    s_text[s_text_len] = '\0';
    xSemaphoreGive(s_mu);
}

int volc_asr_fail(void) { return s_failed; }
bool volc_asr_up(void) { return s_up; }
