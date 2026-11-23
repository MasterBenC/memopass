#pragma once

#include "app_config.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stddef.h>

esp_err_t volc_asr_open(const app_config_t *cfg, QueueHandle_t used, QueueHandle_t freeq);
void volc_asr_close(void);
bool volc_asr_copy(char *out, size_t cap, size_t *len);
void volc_asr_drop(size_t n);
int volc_asr_fail(void);
bool volc_asr_up(void);
/* True while a WSS/TLS handshake is in progress. */
bool volc_asr_connecting(void);
