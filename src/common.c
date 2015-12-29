#include <stdarg.h>
#include "common.h"

void error_exit(const char *format, ...) {
    char msg[1000];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, 1000, format, args);
    va_end(args);
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(1);
}
