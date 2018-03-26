#include "filesystem.h"
#include "win/msdirent.h"

int file_exists(const char *filename) {
	struct stat sb;
	return stat(filename, &sb) == 0;
}

int mkdir_r(char *path) {
    struct stat sb;
    char *curpos = path;
    int mode = 0777;

    /* check if something already exists at path */
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
        if (stat(path, &sb) != 0) {
            if (mkdir(path, mode)) {
                fprintf(stderr, "Could not create directory '%s': %s\n", path, strerror(errno));
                return -1;
            }
        } else if (S_ISDIR(sb.st_mode) == 0) {
            fprintf(stderr, "Could not create directory '%s': file exists but is not a directory\n", path);
            return -1;
        }
        
        *curpos++ = '/';
        while (*curpos == '/') {
            curpos++;
        }
    }

    if (mkdir(path, mode)) {
        fprintf(stderr, "Could not create directory '%s': %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

int filesize(FILE *fd) {
    int size;

    fseek(fd, 0L, SEEK_END);
    size = ftell(fd);
    rewind(fd);

    return size;
}
