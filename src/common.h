#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "compat.h"
#include "filesystem.h"

#define OS_PATH_MAX PATH_MAX
#define OS_NAME_MAX NAME_MAX
#define PAKFILE_SIGNATURE_LEN 4
/* On-disk filename field is 56 bytes. The in-memory buffer is one byte
 * larger so we can force a NUL terminator after fread() — names that fill
 * all 56 bytes on disk would otherwise overread into adjacent struct
 * fields when handed to strlen/strcmp/printf("%s"). */
#define PAKFILE_PATH_MAX 56
#define PAKFILE_PATH_BUF 57
#define PAKFILE_DIR_ENTRY_SIZE 64
#define PAKFILE_HEADER_SIZE 12
/* Sanity cap on directory entry count. id's pak0.pak has 339 entries;
 * a million is generous and prevents calloc-loop DoS on crafted headers. */
#define PAKFILE_MAX_ENTRIES 1048576u
/* Buffer size for streaming file copies during add. Bounds peak RSS at
 * 64 KiB regardless of input file size. */
#define PAKFILE_COPY_CHUNK 65536u

typedef struct Pakfileentry_s {
    char filename[PAKFILE_PATH_BUF];
    uint32_t offset;
    uint32_t length;
    struct Pakfileentry_s *next;
} Pakfileentry_t;

typedef struct {
    char signature[PAKFILE_SIGNATURE_LEN];
    uint32_t diroffset;
    uint32_t dirlength;
    uint32_t num_entries;
    uint64_t file_size;
    Pakfileentry_t *head;
} Pak_t;

/* C99-compatible compile-time check via typedef'd array of length 1 or
 * -1. Used to guard the constants the on-disk format depends on; if a
 * future change accidentally edits a constant without updating peers
 * (PAKFILE_HEADER_SIZE != signature + diroffset + dirlength bytes,
 * say) the build fails here rather than silently producing corrupt
 * paks. */
#define PAKKA_STATIC_ASSERT(cond, name) \
    typedef char pakka_assert_##name[(cond) ? 1 : -1]

PAKKA_STATIC_ASSERT(
    PAKFILE_HEADER_SIZE == PAKFILE_SIGNATURE_LEN + 2 * (int)sizeof(uint32_t),
    header_size_matches_fields);
PAKKA_STATIC_ASSERT(
    PAKFILE_DIR_ENTRY_SIZE == PAKFILE_PATH_MAX + 2 * (int)sizeof(uint32_t),
    dir_entry_size_matches_fields);
PAKKA_STATIC_ASSERT(
    PAKFILE_PATH_BUF == PAKFILE_PATH_MAX + 1,
    path_buf_has_nul_guard);

void error_exit(const char *format, ...);
/* As error_exit, but uses an explicitly captured errno instead of the
 * current global. Use this from any call site where a library call
 * (free/closedir/fclose/snprintf, etc.) may run between the failing
 * operation and the error_exit call — those intermediates can reset
 * errno and produce a misleading strerror in the message. Pass 0 to
 * suppress the strerror suffix entirely. */
void error_exit_e(int saved_errno, const char *format, ...);

/* Write s to out with any control byte (0x00-0x1F or 0x7F) replaced
 * by '?'. Defense in depth for the list/tree/debug printers: pak entry
 * names are 56 bytes of attacker-controlled data, and even though
 * extract refuses to materialize control bytes on disk, listing a
 * malicious pak shouldn't let it inject ANSI escapes that reflow a
 * user's terminal. */
void fprint_sanitized(FILE *out, const char *s);

/* Copy src into dst with the same sanitization, NUL-terminated, up
 * to dstsz-1 bytes. Returns dst. Use to inject pak-derived names
 * into error_exit format strings without leaking control bytes into
 * the user's terminal. */
char *sanitize_name(char *dst, size_t dstsz, const char *src);
/* Open an existing pak. writable=0 opens "rb" (works on read-only
 * paks; required for list/extract), writable=1 opens "r+b" (required
 * for add/delete). */
Pak_t *open_pakfile(const char *pakpath, int writable);
Pak_t *create_pakfile(const char *pakpath);
int close_pakfile(Pak_t *pak);
void list_files(Pak_t *pak);
void list_files_tree(Pak_t *pak);
void extract_files(Pak_t *pak, char *dest, char **paths, int path_count);
void delete_files(Pak_t *pak, char **paths, int path_count);
void debug_header(Pak_t *pak);
void debug_directory_entry(Pakfileentry_t *entry);

int add_file(Pak_t *pak, char *path);
int add_to_pak(Pak_t *pak, char *path);
int add_folder(Pak_t *pak, char *path);
int add_files(Pak_t *pak, char **paths, int path_count);
