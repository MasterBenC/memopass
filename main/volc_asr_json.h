#pragma once

#include <stddef.h>

/* Copy the longest JSON string value of the key "text". Handles standard
 * escapes including \uXXXX. Returns the unescaped length, or 0 if absent. */
size_t volc_json_longest_text(const char *json, size_t len,
                                 char *out, size_t out_cap);

/* Merge a newly observed transcript into the live buffer.
 * Cumulative growth replaces; a shorter prefix is ignored; otherwise append. */
size_t volc_text_merge(char *dst, size_t cap, size_t *len,
                          const char *incoming, size_t incoming_len);
