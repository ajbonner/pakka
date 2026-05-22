#include "filesystem.h"

int pakka_file_exists(const char *filename) {
	struct stat sb;
	return pakka_platform_stat(filename, &sb) == 0;
}

/* mkdir(2) under a racing-process tolerance: EEXIST is success as long
 * as the existing path is a directory. Without it, two pakka extracts
 * sharing a -C dir spuriously fail when one wins the mkdir race. */
static int mkdir_or_exists_dir(const char *path, int mode) {
    struct stat sb;
    if (pakka_platform_mkdir(path, mode) == 0) {
        return 0;
    }
    if (errno != EEXIST) {
        return -1;
    }
    if (pakka_platform_stat(path, &sb) != 0) {
        return -1;
    }
    if (! S_ISDIR(sb.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

/* Recursive mkdir. Silent — library code; the caller (CLI) is
 * responsible for translating non-zero return into a user-facing
 * diagnostic. errno is set on failure (either by mkdir(2) or by
 * setting it explicitly when the existing path is not a directory). */
int pakka_mkdir_r(char *path) {
    struct stat sb;
    char *curpos = path;
    int mode = 0777;

    if (pakka_platform_stat(path, &sb) == 0) {
        if (S_ISDIR(sb.st_mode) == 0) {
            errno = ENOTDIR;
            return -1;
        }
        return 0;
    }

    while (*curpos == '/') {
        curpos++;
    }

    while ((curpos = strchr(curpos, '/'))) {
        *curpos = '\0';
        if (mkdir_or_exists_dir(path, mode) != 0) {
            return -1;
        }
        *curpos++ = '/';
        while (*curpos == '/') {
            curpos++;
        }
    }

    if (mkdir_or_exists_dir(path, mode) != 0) {
        return -1;
    }

    return 0;
}

int64_t pakka_filesize(FILE *fd) {
    int64_t size;

    if (pakka_platform_fseek(fd, 0, SEEK_END) != 0) {
        return -1;
    }
    size = pakka_platform_ftell(fd);
    if (size < 0) {
        return -1;
    }
    rewind(fd);

    return size;
}
