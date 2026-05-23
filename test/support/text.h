#ifndef PAKKA_TEST_TEXT_H
#define PAKKA_TEST_TEXT_H

#include <stddef.h>

/* Drop '\r' bytes that immediately precede '\n'. In-place; returns the
 * new length. Other '\r' bytes are preserved. */
size_t text_strip_crlf(char *buf, size_t len);

/* Split `input` (length `len`, not necessarily NUL-terminated) on '\n'.
 * Each returned line is its own malloc'd NUL-terminated string with any
 * trailing '\r' stripped. A trailing line without '\n' is included.
 * Returns a NULL-terminated array of *count strings, or NULL on OOM. */
char **text_split_lines(const char *input, size_t len, size_t *out_count);

void text_free_lines(char **lines, size_t count);

/* Non-overlapping occurrences of `needle` in `haystack`. */
size_t text_substring_count(const char *haystack, const char *needle);

#endif
