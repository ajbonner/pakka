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
