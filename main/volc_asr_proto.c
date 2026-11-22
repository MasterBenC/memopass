#include "volc_asr_proto.h"

#include <stdio.h>
#include <string.h>

static void put_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint32_t get_u32(const uint8_t *in) {
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

size_t volc_frame_encode(uint8_t *out, size_t out_cap,
                            uint8_t message_type, uint8_t flags,
                            uint8_t serialization, uint8_t compression,
                            bool write_sequence, int32_t sequence,
                            bool write_error, uint32_t error_code,
                            const uint8_t *payload, size_t payload_len) {
    size_t total = 4u + (write_sequence ? 4u : 0u) + (write_error ? 4u : 0u) +
                   4u + payload_len;
    if (!out || total < payload_len || out_cap < total) return 0;
    if (payload_len && !payload) return 0;
    if (message_type > 0x0f || flags > 0x0f || serialization > 0x0f ||
        compression > 0x0f) {
        return 0;
    }

    out[0] = 0x11;
    out[1] = (uint8_t)((message_type << 4) | flags);
    out[2] = (uint8_t)((serialization << 4) | compression);
    out[3] = 0;
    size_t offset = 4;
    if (write_sequence) {
        put_u32(out + offset, (uint32_t)sequence);
        offset += 4;
    }
    if (write_error) {
        put_u32(out + offset, error_code);
        offset += 4;
    }
    put_u32(out + offset, (uint32_t)payload_len);
    offset += 4;
    if (payload_len) memcpy(out + offset, payload, payload_len);
    return total;
}

int volc_frame_decode(const uint8_t *bytes, size_t len, volc_frame_t *out) {
    if (!bytes || !out || len < 4) return -1;
    unsigned header_units = bytes[0] & 0x0fu;
    if (header_units == 0 || len < header_units * 4u) return -1;
    memset(out, 0, sizeof(*out));
    out->message_type = (uint8_t)(bytes[1] >> 4);
    out->flags = (uint8_t)(bytes[1] & 0x0f);
    out->serialization = (uint8_t)(bytes[2] >> 4);
    out->compression = (uint8_t)(bytes[2] & 0x0f);

    size_t offset = header_units * 4u;
    if ((out->flags & VOLC_FLAG_SEQUENCE) != 0) {
        if (len < offset + 4) return -1;
        out->has_sequence = true;
        out->sequence = (int32_t)get_u32(bytes + offset);
        offset += 4;
    }
    if (out->message_type == VOLC_MSG_ERROR_SERVER) {
        if (len < offset + 4) return -1;
        out->has_error = true;
        out->error_code = get_u32(bytes + offset);
        offset += 4;
    }
    if (len < offset + 4) return -1;
    size_t payload_len = get_u32(bytes + offset);
    offset += 4;
    if (payload_len > len - offset) return -1;
    out->payload = payload_len ? bytes + offset : NULL;
    out->payload_len = payload_len;
    return 0;
}

static const char *REQUEST_JSON =
    "{\"user\":{\"uid\":\"memopass\"},\"audio\":{\"format\":\"pcm\",\"codec\":\"raw\","
    "\"rate\":16000,\"bits\":16,\"channel\":1},\"request\":{\"model_name\":\"bigmodel\","
    "\"enable_itn\":true,\"enable_punc\":true,\"enable_ddc\":false,"
    "\"show_utterances\":true,\"result_type\":\"full\"}}";

const char *volc_asr_request_json(void) { return REQUEST_JSON; }

int volc_asr_headers(char *out, size_t cap,
                     const char *app, const char *access, const char *apikey,
                     const char *resource, const char *request_id) {
    const char *res = (resource && resource[0]) ? resource : "volc.bigasr.sauc.duration";
    const char *id = (request_id && request_id[0]) ? request_id : "memopass";
    int n;
    if (!out || cap < 32) return -1;
    if (apikey && apikey[0]) {
        n = snprintf(out, cap,
                     "X-Api-Key: %s\r\n"
                     "X-Api-Resource-Id: %s\r\n"
                     "X-Api-Request-Id: %s\r\n"
                     "X-Api-Connect-Id: %s\r\n"
                     "X-Api-Sequence: -1\r\n",
                     apikey, res, id, id);
    } else {
        if (!app || !app[0] || !access || !access[0]) return -1;
        n = snprintf(out, cap,
                     "X-Api-App-Key: %s\r\n"
                     "X-Api-Access-Key: %s\r\n"
                     "X-Api-Resource-Id: %s\r\n"
                     "X-Api-Request-Id: %s\r\n"
                     "X-Api-Connect-Id: %s\r\n"
                     "X-Api-Sequence: -1\r\n",
                     app, access, res, id, id);
    }
    if (n <= 0 || (size_t)n >= cap) return -1;
    return n;
}

static bool contains(const char *body, const char *needle) {
    return body && needle && strstr(body, needle) != NULL;
}

volc_fail_t volc_asr_classify(uint32_t code, const char *body) {
    if (code == 401 || code == 403 || code == 45000010 || code == 45000011) {
        return VOLC_FAIL_AUTH;
    }
    if (contains(body, "unauthorized") || contains(body, "Invalid auth") ||
        contains(body, "AccessKey") || contains(body, "AppKey") ||
        contains(body, "token") || contains(body, "鉴权")) {
        return VOLC_FAIL_AUTH;
    }
    if (code == 45000030 || contains(body, "resource") || contains(body, "Resource") ||
        contains(body, "未开通") || contains(body, "quota")) {
        return VOLC_FAIL_RESOURCE;
    }
    if (code != 0 || contains(body, "timeout") || contains(body, "Timeout")) {
        return VOLC_FAIL_NET;
    }
    return VOLC_FAIL_NONE;
}
