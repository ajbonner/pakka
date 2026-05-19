#ifndef PAKKA_COMPAT_H
#define PAKKA_COMPAT_H

/* Bridges POSIX calls used elsewhere in pakka to their Win32 equivalents
 * under cl.exe. On POSIX builds, every compat_* function is a thin
 * forwarding wrapper; on Windows builds it routes to _fullpath, _mkdir,
 * GetTempPathA, MoveFileExA, etc. The point is that the rest of the
 * codebase only ever names POSIX (or pakka-internal) primitives. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #include "vendor/wingetopt/getopt.h"
  #include "vendor/dirent/dirent.h"

  /* MSVC's POSIX-named CRT entry points carry a leading underscore.
   * dirent.h above defines PATH_MAX/NAME_MAX from MAX_PATH if absent. */

  /* UCRT's <sys/stat.h> defines S_ISREG/S_ISDIR, but the SDK headers
   * older toolsets bundle don't — guard defensively. */
  #ifndef S_ISREG
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
  #endif
  #ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
  #endif
  /* Win32 has no POSIX symlinks; reparse-point safety is handled
   * separately. S_ISLNK as a 0 macro lets the symlink-rejection
   * branches in pakfile.c compile cleanly under MSVC. */
  #ifndef S_ISLNK
    #define S_ISLNK(m) 0
  #endif
#else
  #include <unistd.h>
  #include <libgen.h>
  #include <dirent.h>
  #include <limits.h>
#endif

#define PATH_SEPARATOR "/"

char *pakka_compat_realpath(const char *path, char *resolved);
char *pakka_compat_getcwd(char *buf, size_t size);

/* Wide-offset fseek/ftell. Raw fseek/ftell use `long`, which is 32-bit
 * on LLP64 Windows and 32-bit POSIX — paks in [2 GiB, 4 GiB) silently
 * truncate. Forwards to fseeko/ftello (POSIX) or _fseeki64/_ftelli64
 * (MSVC). The pak wire format still caps offsets at 4 GiB (u32). */
int     pakka_compat_fseek(FILE *fp, int64_t offset, int whence);
int64_t pakka_compat_ftell(FILE *fp);

/* Open a fresh temp file in binary read/write mode ("w+b") and write
 * the resolved path into out_path (capacity out_path_size). The
 * basename suffix must contain "XXXXXX" for mkstemp/_mktemp_s
 * substitution. Read access matters for pakka_commit's rebuild path,
 * which seeks back to read payloads it appended through this same
 * handle; the legacy create/delete paths only write so the upgraded
 * mode is a strict superset.
 *
 * Tries to create the temp in the same directory as target_path (or
 * the target directory itself when used for create / delete rebuilds)
 * so the eventual rename is intra-filesystem and atomic. Falls back
 * to the system temp dir if the target's directory is unwritable.
 * target_path may be NULL to skip the same-dir attempt. Returns NULL
 * on failure. */
FILE *pakka_compat_mkstemp_open(const char *target_path,
                          const char *basename_template,
                          char *out_path, size_t out_path_size);

/* Returns the directory part of path. May mutate path in place
 * (POSIX dirname() contract). Callers must pass a throwaway copy. */
char *pakka_compat_dirname(char *path);

int   pakka_compat_mkdir(const char *path, int mode);
char *pakka_compat_strdup(const char *s);

/* lstat() on POSIX (does not dereference the final path component);
 * falls back to stat() on Windows where reparse-point checks belong
 * to a separate code path. Used by the recursive-add path so symlinks
 * are detected explicitly rather than silently followed. */
int pakka_compat_lstat(const char *path, struct stat *sb);

/* Returns 1 if path is a symlink (POSIX) or a reparse point /
 * junction / mount point (Windows), else 0. Errors are reported as 0
 * (caller's subsequent stat will surface the same problem with a
 * better message). Used by the recursive-add path to reject indirections
 * that would otherwise let an attacker-controlled tree pull files
 * from outside the requested directory. */
int pakka_compat_is_reparse_or_symlink(const char *path);

/* Try to acquire an exclusive non-blocking lock on the open FILE*.
 * POSIX: flock(LOCK_EX | LOCK_NB). Windows: _locking on byte 0. Lock
 * is released when the FILE* is closed. Returns 0 on success, -1 if
 * contended or unsupported. Used in writable opens to prevent two
 * pakka invocations from corrupting a pak in parallel — multiple
 * concurrent add/delete operations would silently clobber each
 * other's directory rewrites. */
int pakka_compat_try_exclusive_lock(FILE *fp);

/* Open a write FILE* at dest_dir/rel_path, refusing to follow any
 * symlink (POSIX) or reparse point (Windows) along rel_path. Missing
 * intermediate directories are created. dest_dir itself MAY be a
 * symlink — the user supplied it and any indirection there is
 * intentional. Returns NULL on failure (symlink blocked, mkdir or
 * open error).
 *
 * Three implementations:
 *   - Modern POSIX (default): openat(2)/mkdirat(2) with O_NOFOLLOW on
 *     every descent so symlink refusal is atomic with the open.
 *   - Legacy POSIX (glibc < 2.4, gate `PAKKA_LEGACY_EXTRACT` in
 *     src/compat.c): fchdir-based emulation. open(O_NOFOLLOW |
 *     O_DIRECTORY) for each intermediate then fchdir into it;
 *     open(O_NOFOLLOW) on the leaf. Symlink refusal is still atomic
 *     per-step; the modern and legacy paths share the same
 *     mkdir→reopen window on missing intermediates and nothing more.
 *   - Windows: GetFileAttributesA + CreateFile with a short TOCTOU
 *     window. */
FILE *pakka_compat_open_extract_target(const char *dest_dir, const char *rel_path);

/* Rename src→dst, replacing dst if it exists. Returns 0 on success
 * (POSIX rename convention), -1 on failure. Callers MUST close every
 * open FILE* on dst before calling: CRT fopen on Windows omits
 * FILE_SHARE_DELETE, so an outstanding handle blocks the replace.
 *
 * win32_code (optional, may be NULL): on Windows MoveFileExA does not
 * set errno; the implementation captures GetLastError() into *win32_code
 * immediately after the failing call so callers can populate
 * pakka_error_t.system_code with a real DWORD. POSIX implementations
 * leave *win32_code untouched and rely on errno as before. */
int pakka_compat_rename_replace(const char *src, const char *dst,
                          uint32_t *win32_code);

/* Rename src→dst with no-replace semantics: fails if dst exists.
 * Used by create mode to close the TOCTOU window between create_pakfile's
 * stat() existence check and close_pakfile's final rename. Returns 0
 * on success, -1 on failure (including dst-already-exists). Same FILE*
 * close-first rule as pakka_compat_rename_replace.
 *
 * win32_code as for pakka_compat_rename_replace. */
int pakka_compat_rename_noreplace(const char *src, const char *dst,
                            uint32_t *win32_code);

#endif
