#include "app_config.h"

#include "feishu_formatter.h"

#include <string.h>

bool app_config_id_ok(const char *id) {
    size_t n;
    if (!id || !id[0]) return false;
    n = strlen(id);
    if (n < 4 || n > APP_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (c <= 0x20 || c >= 0x7f) return false;
    }
    return true;
}

bool app_config_token_ok(const char *token) {
    size_t n;
    if (!token || !token[0]) return false;
    n = strlen(token);
    if (n < 8 || n > APP_TOKEN_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)token[i];
        if (c <= 0x20 || c >= 0x7f) return false;
    }
    return true;
}

bool app_config_ssid_ok(const char *ssid) {
    size_t n;
    if (!ssid) return false;
    n = strlen(ssid);
    return n > 0 && n <= APP_SSID_MAX;
}

int app_config_segment_min(int requested) {
    if (requested == 1 || requested == 5) return requested;
    return 3;
}

const char *app_config_resource(const app_config_t *cfg) {
    if (cfg && cfg->resource[0]) return cfg->resource;
    return APP_RESOURCE_DEFAULT;
}

bool app_config_ready(const app_config_t *cfg) {
    bool asr;
    if (!cfg || !app_config_ssid_ok(cfg->ssid) || !feishu_url_ok(cfg->webhook)) return false;
    if (cfg->api_key_mode) asr = app_config_token_ok(cfg->apikey);
    else asr = app_config_id_ok(cfg->app_id) && app_config_token_ok(cfg->access);
    return asr;
}

#if defined(ESP_PLATFORM)
#include "nvs.h"
#include "nvs_flash.h"

static const char *NS = "memopass";

static esp_err_t open_ns(nvs_handle_t *out, nvs_open_mode_t mode) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        esp_err_t erased = nvs_flash_erase();
        if (erased != ESP_OK) return erased;
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    return nvs_open(NS, mode, out);
}

static void read_str(nvs_handle_t h, const char *key, char *dst, size_t cap) {
    size_t n = cap;
    dst[0] = '\0';
    if (nvs_get_str(h, key, dst, &n) != ESP_OK) dst[0] = '\0';
}

esp_err_t app_config_load(app_config_t *cfg) {
    nvs_handle_t h;
    esp_err_t err;
    uint8_t u8 = 0;
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));
    cfg->segment_min = 3;
    err = open_ns(&h, NVS_READONLY);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    read_str(h, "ssid", cfg->ssid, sizeof(cfg->ssid));
    read_str(h, "pass", cfg->pass, sizeof(cfg->pass));
    read_str(h, "app", cfg->app_id, sizeof(cfg->app_id));
    read_str(h, "access", cfg->access, sizeof(cfg->access));
    read_str(h, "apikey", cfg->apikey, sizeof(cfg->apikey));
    read_str(h, "resource", cfg->resource, sizeof(cfg->resource));
    read_str(h, "hook", cfg->webhook, sizeof(cfg->webhook));
    read_str(h, "secret", cfg->secret, sizeof(cfg->secret));
    if (nvs_get_u8(h, "seg", &u8) == ESP_OK) cfg->segment_min = (uint8_t)app_config_segment_min(u8);
    if (nvs_get_u8(h, "apimode", &u8) == ESP_OK) cfg->api_key_mode = u8 != 0;
    if (nvs_get_u8(h, "beep", &u8) == ESP_OK) cfg->beep = u8 != 0;
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t put_str(nvs_handle_t h, const char *key, const char *value) {
    return nvs_set_str(h, key, value ? value : "");
}

esp_err_t app_config_save(const app_config_t *cfg) {
    nvs_handle_t h;
    esp_err_t err;
    if (!cfg) return ESP_ERR_INVALID_ARG;
    err = open_ns(&h, NVS_READWRITE);
    if (err != ESP_OK) return err;
    err = put_str(h, "ssid", cfg->ssid);
    if (err == ESP_OK) err = put_str(h, "pass", cfg->pass);
    if (err == ESP_OK) err = put_str(h, "app", cfg->app_id);
    if (err == ESP_OK) err = put_str(h, "access", cfg->access);
    if (err == ESP_OK) err = put_str(h, "apikey", cfg->apikey);
    if (err == ESP_OK) err = put_str(h, "resource", cfg->resource);
    if (err == ESP_OK) err = put_str(h, "hook", cfg->webhook);
    if (err == ESP_OK) err = put_str(h, "secret", cfg->secret);
    if (err == ESP_OK) err = nvs_set_u8(h, "seg", (uint8_t)app_config_segment_min(cfg->segment_min));
    if (err == ESP_OK) err = nvs_set_u8(h, "apimode", cfg->api_key_mode ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "beep", cfg->beep ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_clear(void) {
    nvs_handle_t h;
    esp_err_t err = open_ns(&h, NVS_READWRITE);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_hist_load(app_hist_t *items, int *count) {
    nvs_handle_t h;
    esp_err_t err;
    size_t n = sizeof(app_hist_t) * APP_HIST_MAX;
    if (!items || !count) return ESP_ERR_INVALID_ARG;
    memset(items, 0, n);
    *count = 0;
    err = open_ns(&h, NVS_READONLY);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_get_blob(h, "hist", items, &n);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (n % sizeof(app_hist_t) != 0) return ESP_ERR_INVALID_SIZE;
    *count = (int)(n / sizeof(app_hist_t));
    if (*count > APP_HIST_MAX) *count = APP_HIST_MAX;
    return ESP_OK;
}

esp_err_t app_config_hist_push(const app_hist_t *item) {
    app_hist_t items[APP_HIST_MAX];
    int count = 0;
    nvs_handle_t h;
    esp_err_t err;
    if (!item) return ESP_ERR_INVALID_ARG;
    err = app_config_hist_load(items, &count);
    if (err != ESP_OK) count = 0;
    if (count >= APP_HIST_MAX) {
        memmove(items, items + 1, sizeof(app_hist_t) * (APP_HIST_MAX - 1));
        count = APP_HIST_MAX - 1;
    }
    items[count++] = *item;
    err = open_ns(&h, NVS_READWRITE);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "hist", items, sizeof(app_hist_t) * (size_t)count);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
#endif
