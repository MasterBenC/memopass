#pragma once

#include <stddef.h>
#include <stdbool.h>

/* Largest prefix of len bytes that does not split a UTF-8 sequence and fits
 * in max_bytes. Returns 0 when even the first character does not fit. */
size_t mp_utf8_fit(const char *text, size_t len, size_t max_bytes);

bool mp_utf8_valid(const char *text, size_t len);
