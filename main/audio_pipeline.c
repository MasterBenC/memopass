#include "audio_pipeline.h"

#include "bsp_audio.h"
#include "freertos/task.h"

#include <string.h>

static QueueHandle_t s_used;
static QueueHandle_t s_free;
static volatile bool *s_paused;
static volatile bool *s_muted;
static TaskHandle_t s_task;
static volatile bool s_run;
static volatile uint8_t s_level;

static uint8_t rms_level(const int16_t *pcm, size_t samples) {
    uint64_t acc = 0;
    if (!samples) return 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i];
        acc += (uint32_t)(v * v);
    }
    uint32_t mean = (uint32_t)(acc / samples);
    uint32_t root = 0;
    for (uint32_t bit = 1u << 15; bit; bit >>= 1) {
        uint32_t next = root | bit;
        if (next * next <= mean) root = next;
    }
    if (root > 4000) root = 4000;
    return (uint8_t)(root * 255 / 4000);
}

static void audio_task(void *arg) {
    (void)arg;
    static int16_t pcm[AUDIO_PCM_BYTES / 2];
    while (s_run) {
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        bool quiet = (s_paused && *s_paused) || (s_muted && *s_muted);
        s_level = quiet ? 0 : rms_level(pcm, AUDIO_PCM_BYTES / 2);
        uint8_t *slot = NULL;
        if (xQueueReceive(s_free, &slot, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        if (quiet) memset(slot, 0, AUDIO_PCM_BYTES);
        else memcpy(slot, pcm, AUDIO_PCM_BYTES);
        if (xQueueSend(s_used, &slot, 0) != pdTRUE) {
            xQueueSend(s_free, &slot, 0);
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_pipeline_start(QueueHandle_t used, QueueHandle_t freeq,
                               volatile bool *paused, volatile bool *muted) {
    if (s_task || !used || !freeq) return ESP_ERR_INVALID_STATE;
    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) return err;
    err = bsp_audio_set_format(16000, 16, 1);
    if (err != ESP_OK) return err;
    bsp_audio_set_volume(0);
    s_used = used;
    s_free = freeq;
    s_paused = paused;
    s_muted = muted;
    s_run = true;
    s_level = 0;
    if (xTaskCreate(audio_task, "mp_audio", 4096, NULL, 6, &s_task) != pdPASS) {
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_pipeline_stop(void) {
    s_run = false;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    bsp_audio_sleep();
    s_level = 0;
}

uint8_t audio_pipeline_level(void) { return s_level; }
