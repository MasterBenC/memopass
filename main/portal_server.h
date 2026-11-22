#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORTAL_SSID_MAX 32
#define PORTAL_PASS_MAX 64
#define PORTAL_FIELD_MAX 220

typedef struct {
    char ssid[PORTAL_SSID_MAX + 1];
    char pass[PORTAL_PASS_MAX + 1];
    char app_id[49];
    char access[97];
    char apikey[97];
    char webhook[PORTAL_FIELD_MAX + 1];
    char secret[65];
    char resource[49];
    int segment_min;
    bool api_key_mode;
    bool factory;
    bool has_ssid;
    bool has_app;
    bool has_access;
    bool has_apikey;
    bool has_webhook;
} portal_form_t;

/* application/x-www-form-urlencoded. Unknown keys are ignored.
 * Returns 0 on a well-formed body, -1 on malformed percent-encoding. */
int portal_form_parse(const char *body, size_t len, portal_form_t *out);

#if defined(ESP_PLATFORM)
#include "esp_err.h"
esp_err_t portal_start(const char *ap_ssid);
void portal_stop(void);
bool portal_running(void);
const char *portal_ap_ssid(void);
#endif
