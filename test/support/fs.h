#ifndef PAKKA_TEST_FS_H
#define PAKKA_TEST_FS_H

#include <stddef.h>

/* Join "a" + "/" + "b". Caller frees. */
char *fs_join(const char *a, const char *b);

/* mkdir -p semantics. 0 on success, -1 on failure (errno set). */
int fs_mkdir_p(const char *path);

/* Read entire file into a malloc'd buffer. Returns NULL on error.
 * *out_len holds the byte count on success. */
unsigned char *fs_read_file(const char *path, size_t *out_len);

/* Write `data` of length `len` to `path` (creates / truncates).
 * Parent directory must already exist. 0 on success, -1 on failure. */
int fs_write_file(const char *path, const void *data, size_t len);

/* True if `path` exists and is a regular file. */
int fs_is_file(const char *path);

/* True if `path` exists and is a directory. */
int fs_is_dir(const char *path);

/* Recursively compare directories `a` and `b`. Returns 0 if they
 * contain the same files with identical contents, non-zero on the
 * first mismatch; writes a one-line diagnostic to stderr on diff.
 * "." and ".." are excluded; entries are visited in name order so
 * the diagnostic on failure is deterministic. */
int fs_diff_tree(const char *a, const char *b);

/* Recursive delete (rm -rf). No-op (returns 0) if `path` doesn't
 * exist; returns -1 on any failure during the walk. */
int fs_rmtree(const char *path);

#endif
