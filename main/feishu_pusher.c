#include "feishu_pusher.h"

#include "app_net.h"
#include "feishu_formatter.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "memopass_push";
#define PUSH_SLOTS 3

static feishu_seg_t s_slots[PUSH_SLOTS];
static QueueHandle_t s_free;
static QueueHandle_t s_used;
static TaskHandle_t s_task;
static const app_config_t *s_cfg;
static volatile bool s_run;
static volatile int s_sent;
static volatile int s_failed;
static char s_body[4096];

static esp_err_t post_body(const char *url, const char *body, int *status) {
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    if (status) *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

static bool format_seg(const feishu_seg_t *seg, char *out, size_t cap) {
    feishu_job_t job = {
        .meeting_id = seg->meeting_id,
        .segment = seg->segment,
        .is_final = seg->is_final,
        .duration_sec = seg->duration_sec,
        .text = seg->text,
        .text_len = seg->text_len,
        .with_sign = s_cfg->secret[0] != '\0',
        .unix_time = app_net_clock(),
        .secret = s_cfg->secret,
    };
    if (!job.unix_time) job.unix_time = 1700000000;
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
        if (format_seg(seg, s_body, sizeof(s_body))) {
            for (int attempt = 0; attempt < 3 && !ok; attempt++) {
                int status = 0;
                esp_err_t err = post_body(s_cfg->webhook, s_body, &status);
                ok = err == ESP_OK && status >= 200 && status < 300;
                if (!ok) vTaskDelay(pdMS_TO_TICKS(400 * (attempt + 1)));
            }
        }
        if (ok) s_sent++;
        else {
            s_failed++;
            ESP_LOGW(TAG, "push failed seg=%d", seg->segment);
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
    for (int i = 0; i < 80 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(25));
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
    feishu_seg_t seg = {0};
    snprintf(seg.meeting_id, sizeof(seg.meeting_id), "test");
    seg.is_final = true;
    seg.text_len = strlen(text);
    if (seg.text_len > FEISHU_PUSH_TEXT) seg.text_len = FEISHU_PUSH_TEXT;
    memcpy(seg.text, text, seg.text_len);
    if (!format_seg(&seg, s_body, sizeof(s_body))) return ESP_FAIL;
    int status = 0;
    esp_err_t err = post_body(cfg->webhook, s_body, &status);
    if (err != ESP_OK || status < 200 || status >= 300) return ESP_FAIL;
    return ESP_OK;
}
