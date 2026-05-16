#ifndef PAKKA_COMPAT_H
#define PAKKA_COMPAT_H

/* Bridges POSIX calls used elsewhere in pakka to their Win32 equivalents
 * under cl.exe. On POSIX builds, every compat_* function is a thin
 * forwarding wrapper; on Windows builds it routes to _fullpath, _mkdir,
 * GetTempPathA, MoveFileExA, etc. The point is that the rest of the
 * codebase only ever names POSIX (or pakka-internal) primitives. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #include "win/wingetopt/getopt.h"
  #include "win/dirent/dirent.h"

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
#else
  #include <unistd.h>
  #include <libgen.h>
  #include <dirent.h>
  #include <limits.h>
#endif

#define PATH_SEPARATOR "/"

char *compat_realpath(const char *path, char *resolved);
char *compat_getcwd(char *buf, size_t size);

/* Open a fresh temp file in binary write mode and write the resolved
 * path into out_path (capacity out_path_size). The basename suffix
 * must contain "XXXXXX" for mkstemp/_mktemp_s substitution.
 * On POSIX, files are created under /tmp/; on Windows, under the
 * directory returned by GetTempPathA. Returns NULL on failure. */
FILE *compat_mkstemp_open(const char *basename_template,
                          char *out_path, size_t out_path_size);

/* Returns the directory part of path. May mutate path in place
 * (POSIX dirname() contract). Callers must pass a throwaway copy. */
char *compat_dirname(char *path);

int   compat_mkdir(const char *path, int mode);
char *compat_strdup(const char *s);

/* Rename src→dst, replacing dst if it exists. Returns 0 on success
 * (POSIX rename convention), -1 on failure. Callers MUST close every
 * open FILE* on dst before calling: CRT fopen on Windows omits
 * FILE_SHARE_DELETE, so an outstanding handle blocks the replace. */
int compat_rename_replace(const char *src, const char *dst);

#endif
