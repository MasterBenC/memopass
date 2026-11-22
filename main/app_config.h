#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_SSID_MAX 32
#define APP_PASS_MAX 64
#define APP_ID_MAX 48
#define APP_TOKEN_MAX 96
#define APP_URL_MAX 220
#define APP_SECRET_MAX 64
#define APP_RESOURCE_MAX 48
#define APP_ID_TEXT_MAX 12
#define APP_HIST_MAX 8

#define APP_RESOURCE_DEFAULT "volc.bigasr.sauc.duration"

typedef struct {
    char ssid[APP_SSID_MAX + 1];
    char pass[APP_PASS_MAX + 1];
    char app_id[APP_ID_MAX + 1];
    char access[APP_TOKEN_MAX + 1];
    char apikey[APP_TOKEN_MAX + 1];
    char resource[APP_RESOURCE_MAX + 1];
    char webhook[APP_URL_MAX + 1];
    char secret[APP_SECRET_MAX + 1];
    uint8_t segment_min;
    bool api_key_mode;
    bool beep;
} app_config_t;

typedef struct {
    uint32_t unix_time;
    uint16_t duration_sec;
    uint8_t segments;
    uint8_t pushed;
    char id[APP_ID_TEXT_MAX];
} app_hist_t;

bool app_config_id_ok(const char *id);
bool app_config_token_ok(const char *token);
bool app_config_ssid_ok(const char *ssid);
int app_config_segment_min(int requested);
bool app_config_ready(const app_config_t *cfg);
const char *app_config_resource(const app_config_t *cfg);

#if defined(ESP_PLATFORM)
#include "esp_err.h"
esp_err_t app_config_load(app_config_t *cfg);
esp_err_t app_config_save(const app_config_t *cfg);
esp_err_t app_config_clear(void);
esp_err_t app_config_hist_load(app_hist_t *items, int *count);
esp_err_t app_config_hist_push(const app_hist_t *item);
#endif
