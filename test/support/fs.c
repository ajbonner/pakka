#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

char *fs_join(const char *a, const char *b)
{
    size_t alen   = strlen(a);
    int    needs  = alen > 0 && a[alen - 1] != '/';
    size_t blen   = strlen(b);
    size_t total  = alen + (needs ? 1 : 0) + blen + 1;
    char  *result = (char *)malloc(total);
    if (!result) {
        return NULL;
    }
    memcpy(result, a, alen);
    size_t pos = alen;
    if (needs) {
        result[pos++] = '/';
    }
    memcpy(result + pos, b, blen);
    result[pos + blen] = '\0';
    return result;
}

int fs_mkdir_p(const char *path)
{
    char *copy = strdup(path);
    if (!copy) {
        return -1;
    }
    /* Walk forward, creating each prefix in turn. */
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (MKDIR(copy) != 0 && errno != EEXIST) {
                free(copy);
                return -1;
            }
            *p = '/';
        }
    }
    if (MKDIR(copy) != 0 && errno != EEXIST) {
        free(copy);
        return -1;
    }
    free(copy);
    return 0;
}

unsigned char *fs_read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long pos = ftell(f);
    if (pos < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    size_t         len = (size_t)pos;
    unsigned char *buf = (unsigned char *)malloc(len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    if (out_len) {
        *out_len = len;
    }
    return buf;
}

int fs_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    size_t wrote = (len > 0) ? fwrite(data, 1, len, f) : 0;
    int    err   = (wrote == len) ? 0 : -1;
    if (fclose(f) != 0) {
        err = -1;
    }
    return err;
}

int fs_is_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
#ifdef _WIN32
    return (st.st_mode & _S_IFREG) != 0;
#else
    return S_ISREG(st.st_mode);
#endif
}

int fs_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}
