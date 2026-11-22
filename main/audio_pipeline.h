#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>

#define AUDIO_PCM_BYTES 6400

esp_err_t audio_pipeline_start(QueueHandle_t used, QueueHandle_t freeq,
                               volatile bool *paused, volatile bool *muted);
void audio_pipeline_stop(void);
uint8_t audio_pipeline_level(void);
