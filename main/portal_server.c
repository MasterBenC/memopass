#include "portal_server.h"

#include <ctype.h>
#include <string.h>

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_copy(const char *in, size_t len, char *out, size_t cap) {
    size_t o = 0;
    if (!out || cap == 0) return -1;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '+') c = ' ';
        else if (c == '%') {
            if (i + 2 >= len) return -1;
            int hi = hex_nibble(in[i + 1]);
            int lo = hex_nibble(in[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            c = (char)((hi << 4) | lo);
            i += 2;
        }
        if (o + 1 >= cap) return -1;
        out[o++] = c;
    }
    out[o] = '\0';
    return 0;
}

static void take(portal_form_t *out, const char *key, const char *val) {
    if (strcmp(key, "ssid") == 0) {
        strncpy(out->ssid, val, PORTAL_SSID_MAX);
        out->ssid[PORTAL_SSID_MAX] = '\0';
        out->has_ssid = out->ssid[0] != '\0';
    } else if (strcmp(key, "pass") == 0) {
        strncpy(out->pass, val, PORTAL_PASS_MAX);
        out->pass[PORTAL_PASS_MAX] = '\0';
    } else if (strcmp(key, "app_id") == 0) {
        strncpy(out->app_id, val, sizeof(out->app_id) - 1);
        out->has_app = out->app_id[0] != '\0';
    } else if (strcmp(key, "access") == 0) {
        strncpy(out->access, val, sizeof(out->access) - 1);
        out->has_access = out->access[0] != '\0';
    } else if (strcmp(key, "apikey") == 0) {
        strncpy(out->apikey, val, sizeof(out->apikey) - 1);
        out->has_apikey = out->apikey[0] != '\0';
    } else if (strcmp(key, "webhook") == 0) {
        strncpy(out->webhook, val, PORTAL_FIELD_MAX);
        out->webhook[PORTAL_FIELD_MAX] = '\0';
        out->has_webhook = out->webhook[0] != '\0';
    } else if (strcmp(key, "secret") == 0) {
        strncpy(out->secret, val, sizeof(out->secret) - 1);
    } else if (strcmp(key, "resource") == 0) {
        strncpy(out->resource, val, sizeof(out->resource) - 1);
    } else if (strcmp(key, "segment") == 0) {
        out->segment_min = (strcmp(val, "1") == 0) ? 1 : (strcmp(val, "5") == 0) ? 5 : 3;
    } else if (strcmp(key, "auth") == 0) {
        out->api_key_mode = strcmp(val, "apikey") == 0;
    } else if (strcmp(key, "factory") == 0) {
        out->factory = strcmp(val, "1") == 0;
    }
}

int portal_form_parse(const char *body, size_t len, portal_form_t *out) {
    size_t i = 0;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->segment_min = 3;
    if (!body || len == 0) return 0;
    while (i < len) {
        size_t key_b = i;
        size_t key_e;
        size_t val_b;
        size_t val_e;
        char key[32];
        char val[PORTAL_FIELD_MAX + 1];
        while (i < len && body[i] != '=' && body[i] != '&') i++;
        key_e = i;
        if (i < len && body[i] == '=') i++;
        val_b = i;
        while (i < len && body[i] != '&') i++;
        val_e = i;
        if (i < len && body[i] == '&') i++;
        if (key_e == key_b) continue;
        if (decode_copy(body + key_b, key_e - key_b, key, sizeof(key)) != 0) return -1;
        if (decode_copy(body + val_b, val_e - val_b, val, sizeof(val)) != 0) return -1;
        take(out, key, val);
    }
    return 0;
}
