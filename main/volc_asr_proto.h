#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Volcengine bigmodel streaming ASR binary frame.
 * Layout: 4-byte header, optional big-endian sequence, optional error code,
 * big-endian payload size, payload. Compression 0 is raw; 1 is gzip. */

#define VOLC_MSG_FULL_CLIENT 0x1u
#define VOLC_MSG_AUDIO_CLIENT 0x2u
#define VOLC_MSG_FULL_SERVER 0x9u
#define VOLC_MSG_ERROR_SERVER 0xFu

#define VOLC_FLAG_SEQUENCE 0x1u
#define VOLC_FLAG_LAST 0x2u

#define VOLC_SER_RAW 0u
#define VOLC_SER_JSON 1u
#define VOLC_COMP_NONE 0u
#define VOLC_COMP_GZIP 1u

typedef struct {
    uint8_t message_type;
    uint8_t flags;
    uint8_t serialization;
    uint8_t compression;
    bool has_sequence;
    int32_t sequence;
    bool has_error;
    uint32_t error_code;
    const uint8_t *payload;
    size_t payload_len;
} volc_frame_t;

size_t volc_frame_encode(uint8_t *out, size_t out_cap,
                            uint8_t message_type, uint8_t flags,
                            uint8_t serialization, uint8_t compression,
                            bool write_sequence, int32_t sequence,
                            bool write_error, uint32_t error_code,
                            const uint8_t *payload, size_t payload_len);

int volc_frame_decode(const uint8_t *bytes, size_t len, volc_frame_t *out);

/* Handshake headers. apikey mode wins when apikey is non-empty.
 * Always emits X-Api-Request-Id, X-Api-Connect-Id and X-Api-Sequence. */
int volc_asr_headers(char *out, size_t cap,
                     const char *app, const char *access, const char *apikey,
                     const char *resource, const char *request_id);

const char *volc_asr_request_json(void);

typedef enum {
    VOLC_FAIL_NONE = 0,
    VOLC_FAIL_AUTH,
    VOLC_FAIL_RESOURCE,
    VOLC_FAIL_NET,
} volc_fail_t;

volc_fail_t volc_asr_classify(uint32_t code, const char *body);
