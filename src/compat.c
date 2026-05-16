#include "compat.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

char *compat_realpath(const char *path, char *resolved) {
    return _fullpath(resolved, path, _MAX_PATH);
}

char *compat_getcwd(char *buf, size_t size) {
    return _getcwd(buf, (int)size);
}

FILE *compat_mkstemp_open(const char *basename_template,
                          char *out_path, size_t out_path_size) {
    DWORD len = GetTempPathA((DWORD)out_path_size, out_path);
    if (len == 0 || len + strlen(basename_template) + 1 > out_path_size) {
        return NULL;
    }
    strcat(out_path, basename_template);
    if (_mktemp_s(out_path, out_path_size) != 0) {
        return NULL;
    }
    return fopen(out_path, "wb");
}

char *compat_dirname(char *path) {
    char *slash, *backslash, *last;
    size_t len;

    if (path == NULL || *path == '\0') {
        return ".";
    }

    /* POSIX dirname strips trailing separators first: dirname("a/") == "."
     * but dirname("a/b/") == "a". Apply the same rule for both / and \. */
    len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        path[--len] = '\0';
    }

    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    /* Pointer comparison between unrelated pointers is implementation-
     * defined; avoid it by picking by NULL-presence first. */
    if (slash == NULL) {
        last = backslash;
    } else if (backslash == NULL) {
        last = slash;
    } else {
        last = (slash > backslash) ? slash : backslash;
    }

    if (last == NULL) {
        return ".";
    }
    if (last == path) {
        last[1] = '\0';
    } else {
        *last = '\0';
    }
    return path;
}

int compat_mkdir(const char *path, int mode) {
    (void)mode;
    return _mkdir(path);
}

char *compat_strdup(const char *s) {
    return _strdup(s);
}

int compat_rename_replace(const char *src, const char *dst) {
    return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

#else

#include <fcntl.h>

char *compat_realpath(const char *path, char *resolved) {
    return realpath(path, resolved);
}

char *compat_getcwd(char *buf, size_t size) {
    return getcwd(buf, size);
}

FILE *compat_mkstemp_open(const char *basename_template,
                          char *out_path, size_t out_path_size) {
    FILE *fp;
    int n, fd;

    n = snprintf(out_path, out_path_size, "/tmp/%s", basename_template);
    if (n < 0 || (size_t)n >= out_path_size) {
        return NULL;
    }
    if ((fd = mkstemp(out_path)) < 0) {
        return NULL;
    }
    if ((fp = fdopen(fd, "wb")) == NULL) {
        close(fd);
        unlink(out_path);
        return NULL;
    }
    return fp;
}

char *compat_dirname(char *path) {
    return dirname(path);
}

int compat_mkdir(const char *path, int mode) {
    return mkdir(path, (mode_t)mode);
}

char *compat_strdup(const char *s) {
    return strdup(s);
}

int compat_rename_replace(const char *src, const char *dst) {
    return rename(src, dst);
}

#endif
