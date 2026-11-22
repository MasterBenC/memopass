#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define FEISHU_TEXT_MAX 18000u
#define FEISHU_SEGMENT_MAX 2500u

typedef enum {
    FEISHU_KIND_OTHER = 0,
    FEISHU_KIND_BOT,
    FEISHU_KIND_BITABLE,
} feishu_kind_t;

typedef struct {
    const char *meeting_id;
    int segment;
    bool is_final;
    int duration_sec;
    const char *text;
    size_t text_len;
    bool with_sign;
    uint32_t unix_time;
    const char *secret;
} feishu_job_t;

feishu_kind_t feishu_kind_from_url(const char *url);
bool feishu_url_ok(const char *url);

/* Cut on a UTF-8 boundary. Never returns a split character. */
size_t feishu_utf8_cut(const char *text, size_t len, size_t max_bytes);

/* HMAC-SHA256 of an empty message, key = "<unix>\n<secret>", then base64.
 * Matches the Feishu custom-bot sample. out receives a NUL-terminated string. */
int feishu_sign(uint32_t unix_time, const char *secret, char *out, size_t cap);

/* Writes one JSON body. Bot text always contains 会议纪要 and stays under 20 KB.
 * Returns the byte length, or -1 if the buffer is too small or the job is invalid. */
int feishu_format(feishu_kind_t kind, const feishu_job_t *job, char *out, size_t cap);
