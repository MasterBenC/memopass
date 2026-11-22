#include "volc_asr_json.h"

#include "mp_utf8.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t append_utf8(char *out, size_t cap, size_t used, uint32_t cp) {
    char tmp[4];
    int n = 0;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (used + (size_t)n >= cap) return used;
    memcpy(out + used, tmp, (size_t)n);
    return used + (size_t)n;
}

static size_t unescape(const char *in, size_t len, char *out, size_t cap) {
    size_t used = 0;
    for (size_t i = 0; i < len && used + 1 < cap; i++) {
        if (in[i] != '\\') {
            out[used++] = in[i];
            continue;
        }
        if (i + 1 >= len) break;
        char esc = in[++i];
        char ch = 0;
        if (esc == '"' || esc == '\\' || esc == '/') ch = esc;
        else if (esc == 'n') ch = '\n';
        else if (esc == 'r') ch = '\r';
        else if (esc == 't') ch = '\t';
        else if (esc == 'u' && i + 4 < len) {
            int h0 = hex_nibble(in[i + 1]);
            int h1 = hex_nibble(in[i + 2]);
            int h2 = hex_nibble(in[i + 3]);
            int h3 = hex_nibble(in[i + 4]);
            if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) break;
            uint32_t cp = ((uint32_t)h0 << 12) | ((uint32_t)h1 << 8) |
                          ((uint32_t)h2 << 4) | (uint32_t)h3;
            i += 4;
            used = append_utf8(out, cap, used, cp);
            continue;
        } else {
            break;
        }
        out[used++] = ch;
    }
    if (cap) out[used] = '\0';
    return used;
}

size_t volc_json_longest_text(const char *json, size_t len,
                                 char *out, size_t out_cap) {
    size_t best = 0;
    if (!json || !out || out_cap == 0) return 0;
    out[0] = '\0';
    for (size_t i = 0; i + 6 < len; i++) {
        if (memcmp(json + i, "\"text\"", 6) != 0) continue;
        size_t j = i + 6;
        while (j < len && (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' ||
                           json[j] == '\r')) {
            j++;
        }
        if (j >= len || json[j] != ':') continue;
        j++;
        while (j < len && (json[j] == ' ' || json[j] == '\t')) j++;
        if (j >= len || json[j] != '"') continue;
        j++;
        size_t start = j;
        bool escaped = false;
        for (; j < len; j++) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (json[j] == '\\') {
                escaped = true;
                continue;
            }
            if (json[j] == '"') break;
        }
        if (j >= len || json[j] != '"') continue;
        if (j - start <= best) continue;
        char *decoded = malloc(out_cap);
        if (!decoded) continue;
        size_t n = unescape(json + start, j - start, decoded, out_cap);
        if (n > best) {
            memcpy(out, decoded, n);
            out[n] = '\0';
            best = n;
        }
        free(decoded);
    }
    return best;
}

size_t volc_text_merge(char *dst, size_t cap, size_t *len,
                          const char *incoming, size_t incoming_len) {
    if (!dst || !len || cap == 0) return 0;
    if (*len >= cap) *len = cap - 1;
    dst[*len] = '\0';
    if (!incoming || incoming_len == 0) return *len;

    size_t fit = mp_utf8_fit(incoming, incoming_len, cap - 1);
    if (*len > 0 && fit >= *len && memcmp(incoming, dst, *len) == 0) {
        memcpy(dst, incoming, fit);
        dst[fit] = '\0';
        *len = fit;
        return *len;
    }
    if (fit > 0 && *len >= fit && memcmp(dst, incoming, fit) == 0) return *len;
    if (*len + 1 >= cap) return *len;
    if (*len > 0) {
        dst[(*len)++] = '\n';
        dst[*len] = '\0';
    }
    size_t room = cap - 1 - *len;
    size_t add = mp_utf8_fit(incoming, incoming_len, room);
    memcpy(dst + *len, incoming, add);
    *len += add;
    dst[*len] = '\0';
    return *len;
}
