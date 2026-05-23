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

#endif
