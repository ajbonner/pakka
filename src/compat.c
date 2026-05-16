/* On macOS, the Makefile's -D_XOPEN_SOURCE=700 (needed for realpath()
 * on glibc) suppresses non-XPG7 declarations, including O_NOFOLLOW
 * and openat()/mkdirat() in fcntl.h. Re-enable the Darwin extensions
 * so the symlink-safe extract path below compiles. Linux/BSD don't
 * need this — they expose openat under _XOPEN_SOURCE=700 already. */
#ifdef __APPLE__
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
#define _DARWIN_C_SOURCE
#endif

#include "compat.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sys/locking.h>

char *compat_realpath(const char *path, char *resolved) {
    return _fullpath(resolved, path, _MAX_PATH);
}

char *compat_getcwd(char *buf, size_t size) {
    return _getcwd(buf, (int)size);
}

/* Best-effort: try `<target_dir>\<template>`. Returns a FILE* on
 * success or NULL on any failure (caller falls back). */
static FILE *mkstemp_open_in_dir(const char *target_path,
                                 const char *basename_template,
                                 char *out_path, size_t out_path_size) {
    char target_copy[PATH_MAX];
    const char *dir;
    int n;

    if (target_path == NULL) return NULL;
    if (strlen(target_path) >= sizeof(target_copy)) return NULL;
    strcpy(target_copy, target_path);
    dir = compat_dirname(target_copy);

    n = snprintf(out_path, out_path_size, "%s\\%s", dir, basename_template);
    if (n < 0 || (size_t)n >= out_path_size) return NULL;

    if (_mktemp_s(out_path, out_path_size) != 0) return NULL;
    return fopen(out_path, "wb");
}

FILE *compat_mkstemp_open(const char *target_path,
                          const char *basename_template,
                          char *out_path, size_t out_path_size) {
    FILE *fp;
    DWORD len;

    fp = mkstemp_open_in_dir(target_path, basename_template, out_path, out_path_size);
    if (fp != NULL) return fp;

    /* Fallback to GetTempPathA when same-dir creation failed (e.g.
     * read-only target directory). The eventual rename may then hit
     * cross-FS errors — same limitation as before this fix. */
    len = GetTempPathA((DWORD)out_path_size, out_path);
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

int compat_rename_noreplace(const char *src, const char *dst) {
    /* MoveFileExA without MOVEFILE_REPLACE_EXISTING fails with
     * ERROR_ALREADY_EXISTS if dst exists — that's exactly the
     * no-replace semantic we want. */
    return MoveFileExA(src, dst, 0) ? 0 : -1;
}

int compat_lstat(const char *path, struct stat *sb) {
    /* Windows has no POSIX symlinks. Reparse-point detection on the
     * recursive-add path is out of scope here; junctions and bind-
     * mounts inside a tree the user explicitly hands us are treated
     * as legitimate content. */
    return stat(path, sb);
}

int compat_try_exclusive_lock(FILE *fp) {
    /* _locking with _LK_NBLCK is exclusive + non-blocking. Lock the
     * first byte from position 0; load_pakfile's first read happens
     * right after open with the stream still at 0, so we don't need
     * to seek-save/restore here. */
    int fd = _fileno(fp);
    if (fd < 0) return -1;
    if (fseek(fp, 0L, SEEK_SET) != 0) return -1;
    return _locking(fd, _LK_NBLCK, 1);
}

/* Helper: append "<sep>name" (or just "name" if path is empty/separator
 * already at end) to path, respecting capacity. Returns 0 on success,
 * -1 if it wouldn't fit. */
static int append_path_component(char *path, size_t cap, const char *name,
                                 size_t name_len, char sep) {
    size_t pos = strlen(path);
    int need_sep = (pos > 0 && path[pos - 1] != '/' && path[pos - 1] != '\\');
    size_t need = name_len + (need_sep ? 1 : 0) + 1;
    if (pos + need > cap) return -1;
    if (need_sep) path[pos++] = sep;
    memcpy(path + pos, name, name_len);
    path[pos + name_len] = '\0';
    return 0;
}

FILE *compat_open_extract_target(const char *dest_dir, const char *rel_path) {
    char path[PATH_MAX];
    DWORD attr;
    const char *p, *seg;
    size_t seg_len;
    int is_leaf;

    if (dest_dir == NULL || rel_path == NULL) return NULL;
    if (strlen(dest_dir) >= sizeof(path)) return NULL;
    strcpy(path, dest_dir);

    seg = rel_path;
    for (p = rel_path; ; p++) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            seg_len = (size_t)(p - seg);
            is_leaf = (*p == '\0');

            if (seg_len == 0) {
                if (is_leaf) return NULL;  /* trailing separator → bad name */
                seg = p + 1;
                continue;
            }

            if (append_path_component(path, sizeof(path), seg, seg_len, '\\') != 0) {
                return NULL;
            }

            attr = GetFileAttributesA(path);
            if (attr != INVALID_FILE_ATTRIBUTES) {
                /* Existing component: refuse if it's a reparse point
                 * (junction, mount point, symlink). Brief TOCTOU window
                 * between this check and the open below is acceptable
                 * — the realistic attack is a pre-planted reparse
                 * point, not a race. */
                if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return NULL;
                if (!is_leaf && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
            } else if (!is_leaf) {
                if (_mkdir(path) != 0) return NULL;
            }

            if (is_leaf) {
                return fopen(path, "wb");
            }

            seg = p + 1;
        }
    }
}

#else

#include <fcntl.h>
#include <errno.h>

char *compat_realpath(const char *path, char *resolved) {
    return realpath(path, resolved);
}

char *compat_getcwd(char *buf, size_t size) {
    return getcwd(buf, size);
}

/* Best-effort: try mkstemp in target_path's parent directory. NULL on
 * any failure (caller falls back to the system temp dir). */
static FILE *mkstemp_open_in_dir(const char *target_path,
                                 const char *basename_template,
                                 char *out_path, size_t out_path_size) {
    char target_copy[PATH_MAX];
    char *dir;
    FILE *fp;
    int n, fd;

    if (target_path == NULL) return NULL;
    if (strlen(target_path) >= sizeof(target_copy)) return NULL;
    strcpy(target_copy, target_path);
    dir = compat_dirname(target_copy);

    n = snprintf(out_path, out_path_size, "%s/%s", dir, basename_template);
    if (n < 0 || (size_t)n >= out_path_size) return NULL;

    fd = mkstemp(out_path);
    if (fd < 0) return NULL;
    fp = fdopen(fd, "wb");
    if (fp == NULL) {
        close(fd);
        unlink(out_path);
        return NULL;
    }
    return fp;
}

FILE *compat_mkstemp_open(const char *target_path,
                          const char *basename_template,
                          char *out_path, size_t out_path_size) {
    FILE *fp;
    int n, fd;

    fp = mkstemp_open_in_dir(target_path, basename_template, out_path, out_path_size);
    if (fp != NULL) return fp;

    /* Fallback to /tmp. Keeps create / delete working on read-only
     * destination directories, at the cost of a potentially cross-FS
     * rename — same constraint as before this fix. */
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

int compat_rename_noreplace(const char *src, const char *dst) {
    /* POSIX rename() always replaces — there is no portable no-replace
     * flag (renameat2's RENAME_NOREPLACE is Linux-specific). Use the
     * link(2)/unlink(2) two-step: link fails with EEXIST when dst
     * already exists, otherwise the source is unlinked. Same EXDEV
     * limitation as plain rename(); both share that constraint, so
     * this isn't a new failure mode. */
    if (link(src, dst) != 0) {
        return -1;
    }
    if (unlink(src) != 0) {
        /* dst is in place; the leftover temp on src is best-effort.
         * Return success so the user-visible operation completed. */
        return 0;
    }
    return 0;
}

int compat_lstat(const char *path, struct stat *sb) {
    return lstat(path, sb);
}

int compat_try_exclusive_lock(FILE *fp) {
    /* fcntl(F_SETLK) is POSIX-mandatory and visible under
     * _XOPEN_SOURCE=700 on every CI target. flock(2) is BSD-derived
     * and the BSDs hide it behind __BSD_VISIBLE, which is 0 in strict
     * XPG7 mode — switching to fcntl avoids the platform-specific
     * feature-test dance. F_SETLK is non-blocking: it fails with
     * EACCES or EAGAIN if another process holds a conflicting lock.
     * Lock is released when fd is closed. */
    struct flock lock;
    int fd = fileno(fp);
    if (fd < 0) return -1;
    lock.l_type   = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start  = 0;
    lock.l_len    = 0;  /* 0 means "to EOF / whole file" */
    lock.l_pid    = 0;
    return fcntl(fd, F_SETLK, &lock);
}

/* O_NOFOLLOW on the trailing component, mkdirat / openat for every
 * step under dest_dir. Each descent re-opens the new dir relative to
 * the previous fd, so even if an attacker swaps a component for a
 * symlink between syscalls, the next openat with O_NOFOLLOW fails
 * atomically. */
FILE *compat_open_extract_target(const char *dest_dir, const char *rel_path) {
    int dirfd, newfd, leaf_fd;
    const char *p, *seg;
    size_t seg_len;
    char comp[PATH_MAX];
    int is_leaf;

    if (dest_dir == NULL || rel_path == NULL) return NULL;

    /* dest_dir may legitimately be a symlink (the user pointed -C at
     * it intentionally). Don't apply O_NOFOLLOW here; the protection
     * starts at the first rel_path component. */
    dirfd = open(dest_dir, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (dirfd < 0) return NULL;

    seg = rel_path;
    for (p = rel_path; ; p++) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            seg_len = (size_t)(p - seg);
            is_leaf = (*p == '\0');

            if (seg_len == 0) {
                if (is_leaf) { close(dirfd); return NULL; }
                seg = p + 1;
                continue;
            }
            if (seg_len >= sizeof(comp)) {
                close(dirfd);
                return NULL;
            }
            memcpy(comp, seg, seg_len);
            comp[seg_len] = '\0';

            if (is_leaf) {
                leaf_fd = openat(dirfd, comp,
                                 O_WRONLY | O_CREAT | O_TRUNC
                                 | O_NOFOLLOW | O_CLOEXEC,
                                 0666);
                close(dirfd);
                if (leaf_fd < 0) return NULL;
                return fdopen(leaf_fd, "wb");
            }

            newfd = openat(dirfd, comp,
                           O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (newfd < 0 && errno == ENOENT) {
                if (mkdirat(dirfd, comp, 0777) != 0) {
                    close(dirfd);
                    return NULL;
                }
                newfd = openat(dirfd, comp,
                               O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            }
            close(dirfd);
            if (newfd < 0) return NULL;
            dirfd = newfd;
            seg = p + 1;
        }
    }
}

#endif
