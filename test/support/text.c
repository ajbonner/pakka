#include "text.h"

#include <stdlib.h>
#include <string.h>

size_t text_strip_crlf(char *buf, size_t len)
{
    size_t w = 0;
    for (size_t r = 0; r < len; r++) {
        if (buf[r] == '\r' && r + 1 < len && buf[r + 1] == '\n') {
            continue;
        }
        buf[w++] = buf[r];
    }
    return w;
}

static char *dup_line(const char *src, size_t len)
{
    if (len > 0 && src[len - 1] == '\r') {
        len--;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    if (len > 0) {
        memcpy(out, src, len);
    }
    out[len] = '\0';
    return out;
}

char **text_split_lines(const char *input, size_t len, size_t *out_count)
{
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        if (input[i] == '\n') {
            count++;
        }
    }
    int has_tail = (len > 0 && input[len - 1] != '\n') ? 1 : 0;
    if (has_tail) {
        count++;
    }

    char **lines = (char **)calloc(count + 1, sizeof(char *));
    if (!lines) {
        *out_count = 0;
        return NULL;
    }

    size_t line_idx = 0;
    size_t start    = 0;
    for (size_t i = 0; i < len; i++) {
        if (input[i] == '\n') {
            lines[line_idx] = dup_line(input + start, i - start);
            if (!lines[line_idx]) {
                text_free_lines(lines, line_idx);
                *out_count = 0;
                return NULL;
            }
            line_idx++;
            start = i + 1;
        }
    }
    if (has_tail) {
        lines[line_idx] = dup_line(input + start, len - start);
        if (!lines[line_idx]) {
            text_free_lines(lines, line_idx);
            *out_count = 0;
            return NULL;
        }
    }

    *out_count = count;
    return lines;
}

void text_free_lines(char **lines, size_t count)
{
    if (!lines) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(lines[i]);
    }
    free(lines);
}

size_t text_substring_count(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return 0;
    }
    size_t      count = 0;
    const char *p     = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += nlen;
    }
    return count;
}
