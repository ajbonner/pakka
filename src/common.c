#include <stdarg.h>
#include "common.h"

static void emit_and_exit(int saved_errno, const char *format, va_list args) {
    char msg[1000];

    vsnprintf(msg, sizeof(msg), format, args);

    if (saved_errno != 0) {
        fprintf(stderr, "%s: %s\n", msg, strerror(saved_errno));
    } else {
        fprintf(stderr, "%s\n", msg);
    }

    exit(1);
}

void error_exit(const char *format, ...) {
    /* Capture errno before any further library call (vsnprintf/strerror)
     * can perturb it. Callers that need to report errno but have
     * intermediate cleanup (free/closedir/fclose) should use
     * error_exit_e with an explicitly saved errno value. */
    int saved_errno = errno;
    va_list args;
    va_start(args, format);
    emit_and_exit(saved_errno, format, args);
    va_end(args);
}

void error_exit_e(int saved_errno, const char *format, ...) {
    va_list args;
    va_start(args, format);
    emit_and_exit(saved_errno, format, args);
    va_end(args);
}

void fprint_sanitized(FILE *out, const char *s) {
    unsigned char c;
    while ((c = (unsigned char)*s++) != '\0') {
        fputc((c < 0x20 || c == 0x7F) ? '?' : c, out);
    }
}

char *sanitize_name(char *dst, size_t dstsz, const char *src) {
    size_t i = 0;
    unsigned char c;
    if (dstsz == 0) return dst;
    while ((c = (unsigned char)*src++) != '\0' && i + 1 < dstsz) {
        dst[i++] = (c < 0x20 || c == 0x7F) ? '?' : (char)c;
    }
    dst[i] = '\0';
    return dst;
}

/* The on-disk pak format is canonically little-endian (Quake 1/2
 * originated on x86). A raw fread of a uint32_t works only on LE
 * hosts; on big-endian (s390x, sparc, ppc) it interprets the bytes in
 * the wrong order and corrupts diroffset/dirlength/offset/length.
 * Anything that touches an on-disk u32 must go through these. */
int read_u32_le(FILE *fp, uint32_t *out) {
    unsigned char b[4];
    if (fread(b, 1, 4, fp) != 4) {
        return -1;
    }
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return 0;
}

int write_u32_le(FILE *fp, uint32_t value) {
    unsigned char b[4];
    b[0] = (unsigned char)(value & 0xFF);
    b[1] = (unsigned char)((value >> 8) & 0xFF);
    b[2] = (unsigned char)((value >> 16) & 0xFF);
    b[3] = (unsigned char)((value >> 24) & 0xFF);
    if (fwrite(b, 1, 4, fp) != 4) {
        return -1;
    }
    return 0;
}
