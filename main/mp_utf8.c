#include "mp_utf8.h"

#include <stddef.h>
#include <stdint.h>

static int sequence_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0;
}

bool mp_utf8_valid(const char *text, size_t len) {
    size_t i = 0;
    if (!text && len) return false;
    while (i < len) {
        int need = sequence_len((unsigned char)text[i]);
        if (need == 0 || i + (size_t)need > len) return false;
        for (int n = 1; n < need; n++) {
            if (((unsigned char)text[i + (size_t)n] & 0xC0) != 0x80) return false;
        }
        i += (size_t)need;
    }
    return true;
}

size_t mp_utf8_fit(const char *text, size_t len, size_t max_bytes) {
    size_t i = 0;
    size_t last = 0;
    if (!text || max_bytes == 0) return 0;
    if (len > max_bytes) len = max_bytes;
    while (i < len) {
        int need = sequence_len((unsigned char)text[i]);
        if (need == 0 || i + (size_t)need > len) break;
        bool ok = true;
        for (int n = 1; n < need; n++) {
            if (((unsigned char)text[i + (size_t)n] & 0xC0) != 0x80) ok = false;
        }
        if (!ok) break;
        i += (size_t)need;
        last = i;
    }
    return last;
}
