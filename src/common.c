#include <stdarg.h>
#include "common.h"

char *_dirname(char * path) {
#ifdef _WIN32
	char *dir = malloc(sizeof(path));
	_splitpath(path, NULL, dir, NULL, NULL);
	return dir;
#else
	return dirname(path);
#endif
}

void error_exit(const char *format, ...) {
    char msg[1000];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, 1000, format, args);
    va_end(args);

    if (errno) {
        fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    } else {
        fprintf(stderr, "%s\n", msg);
    }

    exit(1);
}
