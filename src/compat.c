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
#include <fcntl.h>
#include <sys/locking.h>

char *compat_realpath(const char *path, char *resolved) {
    return _fullpath(resolved, path, _MAX_PATH);
}

char *compat_getcwd(char *buf, size_t size) {
    return _getcwd(buf, (int)size);
}

/* Convert an exclusive-open Win32 HANDLE to a binary write FILE*.
 * Closing the FILE* via fclose tears down the handle as well. */
static FILE *handle_to_wb_file(HANDLE h) {
    int fd;
    FILE *fp;
    fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        return NULL;
    }
    fp = _fdopen(fd, "wb");
    if (fp == NULL) {
        _close(fd);  /* also tears down the underlying HANDLE */
        return NULL;
    }
    return fp;
}

/* Atomically create a temp file under template_dir using basename_template.
 * _mktemp_s only picks a name (verified non-existent at that instant);
 * the subsequent CreateFileA with CREATE_NEW is what closes the race:
 * if a file appears in the gap (or a reparse point gets planted)
 * CreateFileA fails with ERROR_FILE_EXISTS and we re-roll. */
static FILE *mkstemp_open_at(const char *dir,
                             const char *basename_template,
                             char *out_path, size_t out_path_size) {
    char template_save[MAX_PATH];
    HANDLE h;
    int n, retries;

    n = snprintf(out_path, out_path_size, "%s\\%s", dir, basename_template);
    if (n < 0 || (size_t)n >= out_path_size) return NULL;
    if ((size_t)n >= sizeof(template_save)) return NULL;
    strcpy(template_save, out_path);

    for (retries = 8; retries > 0; retries--) {
        strcpy(out_path, template_save);
        if (_mktemp_s(out_path, out_path_size) != 0) return NULL;
        h = CreateFileA(out_path, GENERIC_WRITE, 0, NULL,
                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            FILE *fp = handle_to_wb_file(h);
            if (fp == NULL) DeleteFileA(out_path);
            return fp;
        }
        if (GetLastError() != ERROR_FILE_EXISTS
            && GetLastError() != ERROR_ALREADY_EXISTS) {
            return NULL;
        }
    }
    return NULL;
}

/* Best-effort: try the target's parent directory first. */
static FILE *mkstemp_open_near(const char *target_path,
                               const char *basename_template,
                               char *out_path, size_t out_path_size) {
    char target_copy[PATH_MAX];
    const char *dir;

    if (target_path == NULL) return NULL;
    if (strlen(target_path) >= sizeof(target_copy)) return NULL;
    strcpy(target_copy, target_path);
    dir = compat_dirname(target_copy);
    return mkstemp_open_at(dir, basename_template, out_path, out_path_size);
}

FILE *compat_mkstemp_open(const char *target_path,
                          const char *basename_template,
                          char *out_path, size_t out_path_size) {
    FILE *fp;
    char tempdir[MAX_PATH];
    DWORD len;

    fp = mkstemp_open_near(target_path, basename_template, out_path, out_path_size);
    if (fp != NULL) return fp;

    /* Fallback to GetTempPathA when same-dir creation failed (e.g.
     * read-only target directory). The eventual rename may then hit
     * cross-FS errors — same limitation as before this fix. */
    len = GetTempPathA((DWORD)sizeof(tempdir), tempdir);
    if (len == 0 || len >= sizeof(tempdir)) return NULL;
    /* GetTempPathA appends a trailing backslash; strip it so the
     * dir+template join in mkstemp_open_at produces a single sep. */
    if (len > 0 && (tempdir[len - 1] == '\\' || tempdir[len - 1] == '/')) {
        tempdir[len - 1] = '\0';
    }
    return mkstemp_open_at(tempdir, basename_template, out_path, out_path_size);
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
    /* Windows has no POSIX symlinks. The recursive-add path checks
     * for reparse points via compat_is_reparse_or_symlink — keep
     * compat_lstat as a thin stat() shim for the regular/dir test. */
    return stat(path, sb);
}

int compat_is_reparse_or_symlink(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    return (attr & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
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
                if (_mkdir(path) != 0) {
                    /* Tolerate EEXIST so two extractors sharing a -C
                     * destination can both create the same subdir
                     * without spuriously aborting; re-check the
                     * attributes after the race to make sure the
                     * winner created a real directory rather than a
                     * reparse point. */
                    if (errno != EEXIST) return NULL;
                    attr = GetFileAttributesA(path);
                    if (attr == INVALID_FILE_ATTRIBUTES) return NULL;
                    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return NULL;
                    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
                }
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

/* glibc 2.3 doesn't expose these under _XOPEN_SOURCE=700 (POSIX-2008
 * hadn't standardized them yet), but they've been Linux kernel ABI
 * since 2.1.126. Block must sit after #include <fcntl.h> so the
 * system header can't shadow our fallback. */
#ifdef __linux__
#  ifndef O_DIRECTORY
#    define O_DIRECTORY 0200000
#  endif
#  ifndef O_NOFOLLOW
#    define O_NOFOLLOW  0400000
#  endif
#endif

/* glibc < 2.4 lacks openat/mkdirat. The legacy extract path emulates
 * openat descent with fchdir + open(O_NOFOLLOW); one whole-impl gate
 * instead of per-symbol probes. */
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#  if !__GLIBC_PREREQ(2, 4)
#    define PAKKA_LEGACY_EXTRACT 1
#  endif
#elif defined(__GLIBC__) && defined(__GLIBC_MINOR__)
#  if __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 4)
#    define PAKKA_LEGACY_EXTRACT 1
#  endif
#endif

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
        /* dst is in place, but src is still a live hardlink to the
         * same inode. Reporting success would leave the caller with
         * two mutable names for the same file. Treat as a failure so
         * the caller surfaces an actionable error. */
        return -1;
    }
    return 0;
}

int compat_lstat(const char *path, struct stat *sb) {
    return lstat(path, sb);
}

int compat_is_reparse_or_symlink(const char *path) {
    struct stat sb;
    if (lstat(path, &sb) != 0) return 0;
    return S_ISLNK(sb.st_mode) ? 1 : 0;
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

#ifndef PAKKA_LEGACY_EXTRACT
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
                /* Tolerate EEXIST so two extractors sharing a -C
                 * directory can both create the same subdir without
                 * spuriously aborting. The subsequent openat with
                 * O_NOFOLLOW still rejects the case where the racing
                 * winner created a symlink instead of a dir. */
                if (mkdirat(dirfd, comp, 0777) != 0 && errno != EEXIST) {
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
#else
/* Pre-glibc-2.4 fallback for compat_open_extract_target. fchdir into
 * each opened intermediate so we never name a path that could be
 * raced under us; O_NOFOLLOW makes each step's symlink refusal atomic
 * with the open. The mkdir→reopen window on missing intermediates is
 * the same one the modern openat path has between mkdirat and the
 * next openat(O_NOFOLLOW) — no honest safety regression.
 *
 * Cwd mutation is safe because pakka is single-threaded; we still
 * have to restore via fchdir(saved_cwd) on every return path or the
 * caller's next syscall would resolve against the wrong directory. */
FILE *compat_open_extract_target(const char *dest_dir, const char *rel_path) {
    int dest_fd, saved_cwd, dir_fd, leaf_fd;
    const char *p, *seg;
    size_t seg_len;
    char comp[NAME_MAX + 1];
    int is_leaf;
    FILE *fp = NULL;

    if (dest_dir == NULL || rel_path == NULL) return NULL;

    /* dest_dir may legitimately be a symlink (user-supplied -C);
     * protection starts on rel_path, not here. */
    dest_fd = open(dest_dir, O_RDONLY | O_DIRECTORY);
    if (dest_fd < 0) return NULL;
    saved_cwd = open(".", O_RDONLY | O_DIRECTORY);
    if (saved_cwd < 0) { close(dest_fd); return NULL; }
    if (fchdir(dest_fd) != 0) {
        close(dest_fd);
        close(saved_cwd);
        return NULL;
    }
    close(dest_fd);

    seg = rel_path;
    for (p = rel_path; ; p++) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            seg_len = (size_t)(p - seg);
            is_leaf = (*p == '\0');
            if (seg_len == 0) {
                if (is_leaf) goto out;
                seg = p + 1;
                continue;
            }
            if (seg_len >= sizeof(comp)) goto out;
            memcpy(comp, seg, seg_len);
            comp[seg_len] = '\0';

            if (is_leaf) {
                leaf_fd = open(comp,
                               O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                               0666);
                if (leaf_fd < 0) goto out;
                fp = fdopen(leaf_fd, "wb");
                if (fp == NULL) close(leaf_fd);
                goto out;
            }

            dir_fd = open(comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (dir_fd < 0 && errno == ENOENT) {
                if (mkdir(comp, 0777) != 0 && errno != EEXIST) goto out;
                dir_fd = open(comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            }
            if (dir_fd < 0) goto out;
            if (fchdir(dir_fd) != 0) {
                close(dir_fd);
                goto out;
            }
            close(dir_fd);
            seg = p + 1;
        }
    }
out:
    /* If the cwd restore fails, returning a successful fp would leave
     * the caller's process cwd pointing inside dest_dir — treat as
     * failure rather than silently corrupting later path resolution. */
    if (fchdir(saved_cwd) != 0 && fp != NULL) {
        fclose(fp);
        fp = NULL;
    }
    close(saved_cwd);
    return fp;
}
#endif

#endif
