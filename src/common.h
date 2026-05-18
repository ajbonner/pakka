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

/* Tagged so it matches the opaque forward declaration in
 * include/pakka.h (`typedef struct pakka_entry pakka_entry_t;`). The
 * full definition stays internal. */
struct pakka_entry {
    char filename[PAKFILE_PATH_BUF];
    uint32_t offset;
    uint32_t length;
    struct pakka_entry *next;
};

typedef struct pakka_entry Pakfileentry_t;

/* Tagged to match include/pakka.h's
 * `typedef struct pakka_reader pakka_reader_t;` forward declaration.
 * Each reader owns a re-seek position into the archive's single FILE*
 * so multiple readers on the same archive remain coherent — every
 * pakka_reader_read seeks to next_offset before fread. */
struct pakka_reader {
    struct pakka_archive *archive;
    uint64_t next_offset;
    uint64_t remaining;
};

/* Tagged so it matches the opaque forward declaration in include/pakka.h
 * (`typedef struct pakka_archive pakka_archive_t;`). The full definition
 * stays internal — consumers of the public header see only the tag. */
struct pakka_archive {
    char signature[PAKFILE_SIGNATURE_LEN];
    uint32_t diroffset;
    uint32_t dirlength;
    uint32_t num_entries;
    uint64_t file_size;
    Pakfileentry_t *head;

    /* Moved from src/pakfile.c file-local globals in Phase 2 of the
     * libpakka migration. The archive handle now owns its file handle
     * and paths; only close_pakfile and delete_entries may close fp,
     * and both must do so before any rename — Windows CRT fopen omits
     * FILE_SHARE_DELETE, so a live handle blocks MoveFileEx. */
    FILE *fp;
    char cur_pakpath[OS_PATH_MAX];  /* set by open_pakfile */
    char new_pakpath[OS_PATH_MAX];  /* set by create_pakfile (destination) */
    char tmp_pakpath[OS_PATH_MAX];  /* mkstemp scratch; renamed into place on commit */
    int is_new;                     /* 1 if created via create_pakfile, 0 otherwise */
    int writable;                   /* 1 if pak->fp is r+b (PAKKA_OPEN_READ_WRITE or create) */
    int dirty;                      /* 1 if pakka_add/delete have unflushed changes */
    int needs_rebuild;              /* 1 if any pakka_delete touched the entry list — commit must rebuild via temp */
};

typedef struct pakka_archive Pak_t;

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

/* pakka_die, pakka_die_e, pakka_fprint_sanitized, pakka_sanitize_name
 * live in src/main.c (the CLI). Their prototypes are intentionally NOT
 * declared here so library code can't accidentally call them — the
 * library returns pakka_status_t + pakka_error_t, and only the CLI
 * translates those into exit/stderr. */

/* The pak header diroffset/dirlength and entry offset/length fields
 * are stored little-endian on disk. Use these to read/write them so
 * the code is correct on big-endian hosts (s390x, sparc, ppc).
 * Return 0 on success, -1 on I/O failure (errno set by stdio). */
int pakka_read_u32_le(FILE *fp, uint32_t *out);
int pakka_write_u32_le(FILE *fp, uint32_t value);

/* Internal helpers exposed to the CLI for preflight checks. Not part
 * of the public include/pakka.h surface, but still pakka_*-prefixed so
 * the symbol-audit gate passes. */
int pakka_unsafe_entry_name(const char *path);
void pakka_normalize_entry_name(const char *src, char *dst, size_t dstsz);
