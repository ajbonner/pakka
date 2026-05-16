#include "filesystem.h"

int file_exists(const char *filename) {
	struct stat sb;
	return stat(filename, &sb) == 0;
}

/* mkdir(2) under a racing-process tolerance: EEXIST is success as long
 * as the existing path is a directory. Without it, two pakka extracts
 * sharing a -C dir spuriously fail when one wins the mkdir race. */
static int mkdir_or_exists_dir(const char *path, int mode) {
    struct stat sb;
    if (compat_mkdir(path, mode) == 0) {
        return 0;
    }
    if (errno != EEXIST) {
        return -1;
    }
    if (stat(path, &sb) != 0) {
        return -1;
    }
    if (! S_ISDIR(sb.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int mkdir_r(char *path) {
    struct stat sb;
    char *curpos = path;
    int mode = 0777;

    if (stat(path, &sb) == 0) {
        if (S_ISDIR(sb.st_mode) == 0) {
            fprintf(stderr, "Could not create directory '%s': file exists but is not a directory\n", path);
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
            fprintf(stderr, "Could not create directory '%s': %s\n", path, strerror(errno));
            return -1;
        }

        *curpos++ = '/';
        while (*curpos == '/') {
            curpos++;
        }
    }

    if (mkdir_or_exists_dir(path, mode) != 0) {
        fprintf(stderr, "Could not create directory '%s': %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

int64_t filesize(FILE *fd) {
    long size;

    if (fseek(fd, 0L, SEEK_END) != 0) {
        return -1;
    }
    size = ftell(fd);
    if (size < 0) {
        return -1;
    }
    rewind(fd);

    return (int64_t)size;
}
