#include "feishu_formatter.h"

#include "mp_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

feishu_kind_t feishu_kind_from_url(const char *url) {
    if (!feishu_url_ok(url)) return FEISHU_KIND_OTHER;
    if (strstr(url, "bot/v2/hook")) return FEISHU_KIND_BOT;
    return FEISHU_KIND_BITABLE;
}

bool feishu_url_ok(const char *url) {
    size_t n;
    if (!url) return false;
    n = strlen(url);
    if (n < 16 || n > 220) return false;
    if (strncmp(url, "https://", 8) != 0) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c >= 0x7f) return false;
    }
    if (!strstr(url, "feishu.cn") && !strstr(url, "larksuite.com") &&
        !strstr(url, "feishu.net")) {
        return false;
    }
    return true;
}

size_t feishu_utf8_cut(const char *text, size_t len, size_t max_bytes) {
    return mp_utf8_fit(text, len, max_bytes);
}

static uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static void sha256(const uint8_t *msg, size_t len, uint8_t out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    size_t blocks = (len + 8 + 64) / 64;
    for (size_t b = 0; b < blocks; b++) {
        uint8_t chunk[64];
        memset(chunk, 0, sizeof(chunk));
        size_t off = b * 64;
        if (off < len) {
            size_t n = len - off;
            if (n > 64) n = 64;
            memcpy(chunk, msg + off, n);
        }
        if (off <= len && len - off < 64) {
            if (len - off < 64) chunk[len - off] = 0x80;
        }
        if (b + 1 == blocks) {
            uint64_t bits = (uint64_t)len * 8u;
            for (int i = 0; i < 8; i++) chunk[63 - i] = (uint8_t)(bits >> (8 * i));
        }
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)chunk[i * 4] << 24) | ((uint32_t)chunk[i * 4 + 1] << 16) |
                   ((uint32_t)chunk[i * 4 + 2] << 8) | (uint32_t)chunk[i * 4 + 3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h0, b0 = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = h + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b0) ^ (a & c) ^ (b0 & c);
            uint32_t t2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b0;
            b0 = a;
            a = t1 + t2;
        }
        h0 += a; h1 += b0; h2 += c; h3 += d;
        h4 += e; h5 += f; h6 += g; h7 += h;
    }
    uint32_t hs[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(hs[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(hs[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(hs[i] >> 8);
        out[i * 4 + 3] = (uint8_t)hs[i];
    }
}

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        sha256(key, key_len, k);
    } else if (key && key_len) {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    uint8_t inner_in[64];
    memcpy(inner_in, ipad, 64);
    uint8_t inner[32];
    if (msg_len == 0) {
        sha256(inner_in, 64, inner);
    } else {
        uint8_t *cat = (uint8_t *)malloc(64 + msg_len);
        if (!cat) return;
        memcpy(cat, ipad, 64);
        memcpy(cat + 64, msg, msg_len);
        sha256(cat, 64 + msg_len, inner);
        free(cat);
    }
    uint8_t outer[96];
    memcpy(outer, opad, 64);
    memcpy(outer + 64, inner, 32);
    sha256(outer, 96, out);
}

static int b64_encode(const uint8_t *in, size_t len, char *out, size_t cap) {
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = 4 * ((len + 2) / 3) + 1;
    if (!out || cap < need) return -1;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)in[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)in[i + 1]) << 8;
        if (i + 2 < len) n |= (uint32_t)in[i + 2];
        out[o++] = tab[(n >> 18) & 63];
        out[o++] = tab[(n >> 12) & 63];
        out[o++] = (i + 1 < len) ? tab[(n >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tab[n & 63] : '=';
    }
    out[o] = '\0';
    return (int)o;
}

int feishu_sign(uint32_t unix_time, const char *secret, char *out, size_t cap) {
    char key[160];
    int n;
    uint8_t mac[32];
    if (!secret || !secret[0] || !out) return -1;
    n = snprintf(key, sizeof(key), "%u\n%s", (unsigned)unix_time, secret);
    if (n <= 0 || (size_t)n >= sizeof(key)) return -1;
    hmac_sha256((const uint8_t *)key, (size_t)n, (const uint8_t *)"", 0, mac);
    return b64_encode(mac, sizeof(mac), out, cap);
}

static int append(char *out, size_t cap, size_t *used, const char *s) {
    size_t n = strlen(s);
    if (*used + n >= cap) return -1;
    memcpy(out + *used, s, n);
    *used += n;
    out[*used] = '\0';
    return 0;
}

static int append_json_str(char *out, size_t cap, size_t *used,
                           const char *s, size_t len) {
    if (append(out, cap, used, "\"") != 0) return -1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char one[2] = {0, 0};
        char hex[8];
        if (c == '"' || c == '\\') {
            one[0] = '\\';
            if (append(out, cap, used, one) != 0) return -1;
            one[0] = (char)c;
            if (append(out, cap, used, one) != 0) return -1;
            continue;
        }
        if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20) {
            snprintf(hex, sizeof(hex), "\\u%04x", c);
            esc = hex;
        }
        if (esc) {
            if (append(out, cap, used, esc) != 0) return -1;
            continue;
        }
        one[0] = (char)c;
        if (append(out, cap, used, one) != 0) return -1;
    }
    return append(out, cap, used, "\"");
}

static int append_escaped(char *out, size_t cap, size_t *used,
                           const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        char one[2] = {0, 0};
        char hex[8];
        const char *esc = NULL;
        if (c == '"' || c == '\\') {
            one[0] = '\\';
            if (append(out, cap, used, one) != 0) return -1;
            one[0] = (char)c;
            if (append(out, cap, used, one) != 0) return -1;
            continue;
        }
        if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20) {
            snprintf(hex, sizeof(hex), "\\u%04x", c);
            esc = hex;
        }
        if (esc) {
            if (append(out, cap, used, esc) != 0) return -1;
            continue;
        }
        one[0] = (char)c;
        if (append(out, cap, used, one) != 0) return -1;
    }
    return 0;
}

/* JSON escape of one byte. Returns the escaped length (1, 2 or 6). */
static size_t escape_byte(unsigned char c, char buf[8]) {
    if (c == '"' || c == '\\') {
        buf[0] = '\\';
        buf[1] = (char)c;
        return 2;
    }
    if (c == '\n' || c == '\r' || c == '\t') {
        buf[0] = '\\';
        buf[1] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
        return 2;
    }
    if (c < 0x20) {
        snprintf(buf, 8, "\\u%04x", c);
        return 6;
    }
    buf[0] = (char)c;
    return 1;
}

/* Byte length of the single well-formed UTF-8 character at s, or 0. */
static size_t utf8_char_len(const char *s, size_t len) {
    unsigned char lead = (unsigned char)s[0];
    size_t need = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 :
                  (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 0;
    if (need == 0 || need > len) return 0;
    return mp_utf8_fit(s, need, need) == need ? need : 0;
}

/* Escapes text one UTF-8 character at a time and stops before the first
 * character whose escaped form would push *used past limit (limit < cap).
 * Never fails and never leaves a split character or a half escape behind.
 * Returns the number of source bytes consumed. */
static size_t append_escaped_fit(char *out, size_t cap, size_t *used, size_t limit,
                                 const char *s, size_t len) {
    size_t i = 0;
    if (limit >= cap) limit = cap - 1;
    while (i < len && *used < limit) {
        size_t clen = utf8_char_len(s + i, len - i);
        char buf[8];
        size_t elen;
        if (clen == 0) break;
        if (clen == 1) {
            elen = escape_byte((unsigned char)s[i], buf);
        } else {
            memcpy(buf, s + i, clen);
            elen = clen;
        }
        if (elen > limit - *used) break;
        memcpy(out + *used, buf, elen);
        *used += elen;
        i += clen;
    }
    out[*used] = '\0';
    return i;
}

int feishu_format(feishu_kind_t kind, const feishu_job_t *job, char *out, size_t cap) {
    size_t used = 0;
    size_t text_len;
    const char *text;
    size_t limit = cap > 20001 ? 20000 : (cap > 0 ? cap - 1 : 0);
    if (!job || !out || cap < 64 || !job->meeting_id || !job->meeting_id[0]) return -1;
    text = job->text ? job->text : "";
    text_len = job->text ? job->text_len : 0;
    text_len = feishu_utf8_cut(text, text_len, FEISHU_TEXT_MAX);
    out[0] = '\0';
    if (kind == FEISHU_KIND_BOT) {
        char sign[48];
        char prefix[80];
        int pn = snprintf(prefix, sizeof(prefix), "会议纪要\n#%s 第%d段\n",
                          job->meeting_id, job->segment);
        if (pn <= 0 || (size_t)pn >= sizeof(prefix)) return -1;
        if (job->with_sign) {
            char head[160];
            int hn;
            if (!job->secret || feishu_sign(job->unix_time, job->secret, sign, sizeof(sign)) < 0) {
                return -1;
            }
            hn = snprintf(head, sizeof(head),
                          "{\"timestamp\":\"%u\",\"sign\":\"%s\",\"msg_type\":\"text\",\"content\":{\"text\":\"",
                          (unsigned)job->unix_time, sign);
            if (hn <= 0 || append(out, cap, &used, head) != 0) return -1;
        } else if (append(out, cap, &used,
                          "{\"msg_type\":\"text\",\"content\":{\"text\":\"") != 0) {
            return -1;
        }
        if (append_escaped(out, cap, &used, prefix, (size_t)pn) != 0) return -1;
        /* Reserve the closing "}} so escape growth can only shorten the text. */
        if (used + 3 > limit) return -1;
        append_escaped_fit(out, cap, &used, limit - 3, text, text_len);
        if (append(out, cap, &used, "\"}}") != 0) return -1;
        if (used > 20000) return -1;
        return (int)used;
    }
    if (append(out, cap, &used, "{\"meeting_id\":") != 0) return -1;
    if (append_json_str(out, cap, &used, job->meeting_id, strlen(job->meeting_id)) != 0) return -1;
    {
        char tail[80];
        snprintf(tail, sizeof(tail), ",\"segment\":%d,\"is_final\":%s,\"duration_sec\":%d,\"text\":",
                 job->segment, job->is_final ? "true" : "false", job->duration_sec);
        if (append(out, cap, &used, tail) != 0) return -1;
    }
    /* Same safe truncation for the bitable body: reserve the closing "} */
    if (append(out, cap, &used, "\"") != 0) return -1;
    if (used + 2 > cap - 1) return -1;
    append_escaped_fit(out, cap, &used, cap - 1 - 2, text, text_len);
    if (append(out, cap, &used, "\"}") != 0) return -1;
    return (int)used;
}
