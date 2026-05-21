#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "platform.h"
#include "filesystem.h"
#include "pakka.h"

#define OS_PATH_MAX PATH_MAX
#define OS_NAME_MAX NAME_MAX
#define PAKFILE_SIGNATURE_LEN 4
/* On-disk PAK filename field is 56 bytes. The in-memory buffer is sized
 * to PAKKA_ENTRY_NAME_SIZE (256) to also accommodate ZIP entry names
 * (PK3/PK4), which can be up to that limit. PAK code paths use only
 * the first 57 bytes (PAKFILE_PATH_BUF) and leave the rest zero; the
 * static assert below guards against undersizing. */
#define PAKFILE_PATH_MAX 56
#define PAKFILE_PATH_BUF 57
#define PAKFILE_DIR_ENTRY_SIZE 64
#define PAKFILE_HEADER_SIZE 12
/* Sanity cap on directory entry count. id's pak0.pak has 339 entries;
 * a million is generous and prevents calloc-loop DoS on crafted headers.
 * PK3/PK4 reuse the same cap. */
#define PAKFILE_MAX_ENTRIES 1048576u
/* Buffer size for streaming file copies during add. Bounds peak RSS at
 * 64 KiB regardless of input file size. */
#define PAKFILE_COPY_CHUNK 65536u

/* ZIP container constants — shared between PK3 and PK4. The PK3_*
 * macro prefix is historical; the values are pure ZIP. */
#define PK3_LFH_SIGNATURE  "PK\003\004"
#define PK3_CDR_SIGNATURE  "PK\001\002"
#define PK3_EOCD_SIGNATURE "PK\005\006"
#define PK3_LFH_SIZE        30u   /* fixed prefix of a local file header */
#define PK3_CDR_SIZE        46u   /* fixed prefix of a central directory record */
#define PK3_EOCD_SIZE       22u   /* fixed size of the end-of-central-directory record */
/* EOCD can be preceded by up to 64 KiB of comment bytes; scan window
 * is comment_max + EOCD_SIZE + a 1-byte slack. */
#define PK3_EOCD_SCAN_MAX   65557u
/* DEFLATE / STORED — pakka accepts only these two methods. */
#define PK3_METHOD_STORED   0u
#define PK3_METHOD_DEFLATE  8u
/* Unix file-type bits in CDR external-attrs (upper 16 bits encode the
 * Unix mode). Use raw octal masks so the headers don't need <sys/stat.h>
 * (which doesn't define S_IFLNK on MSVC). */
#define PK3_UNIX_IFMT       0170000u
#define PK3_UNIX_IFLNK      0120000u
/* Default cap on per-entry decompressed bytes — see
 * pakka_set_max_decompressed_size. 64 MiB has headroom for legitimate
 * Q3 content (largest demo asset is ~5 MiB) while bounding peak RSS. */
#define PK3_DEFAULT_MAX_DECOMPRESSED ((uint64_t)64u * 1024u * 1024u)

/* Hard ceiling on the EOCD-declared CDR size at open time. cdr_size
 * is a u32 (up to 4 GiB); a hostile archive can declare an inflated
 * value and force malloc(cdr_size) before any per-entry validation
 * runs. 64 MiB is an intentional metadata cap that clears every
 * realistic PK3 / PK4 the formats target; the standard spec permits
 * larger CDRs (extras/comments can run to u16 max per record), so
 * the gate is a pakka policy, not a spec limit. Distinct from
 * max_decompressed because pk3_load_cdr runs inside
 * pakka_pk3_open_impl, before the caller can call
 * pakka_set_max_decompressed_size. */
#define PK3_MAX_CDR_SIZE ((uint64_t)64u * 1024u * 1024u)

/* IEEE 802.3 CRC32 lookup table — one entry per byte value. */
#define PK3_CRC32_TABLE_SIZE 256u

/* These struct tags match the opaque forward declarations in
 * include/pakka.h. Public consumers see only the tag; the full
 * definitions stay internal. */
struct pakka_entry {
    char filename[PAKKA_ENTRY_NAME_SIZE];
    uint32_t offset;
    uint32_t length;
    struct pakka_entry *next;

    /* ZIP-class fields (PK3/PK4). Zero for PAK entries. */
    uint32_t pk3_compressed_size;   /* bytes between LFH and CDR */
    uint32_t pk3_payload_offset;    /* cached lfh_offset + 30 + name + extra */
    uint32_t pk3_lfh_offset;        /* offset of LFH for verify cross-check */
    uint32_t pk3_crc32;
    uint16_t pk3_method;            /* PK3_METHOD_STORED or PK3_METHOD_DEFLATE */

    /* Daikatana fields. Zero for PAK/SiN entries. */
    uint32_t dk_compressed_size;    /* on-disk bytes for DK compressed entries */
    uint8_t  dk_is_compressed;      /* 0 = STORED, 1 = custom byte-codec */

    /* ZIP queued-add bookkeeping. When non-NULL, the entry's payload
     * hasn't been written into the archive yet — pakka_pk3_add_file_impl
     * staged metadata only, and pk3_commit's rebuild path will stream
     * from the source path or in-memory buffer at commit time. Pairs
     * exclusively (a queued entry has exactly one of source/data set,
     * not both). Both NULL for entries loaded from the on-disk archive
     * and for non-ZIP entries. */
    char    *pk3_pending_source;    /* strdup'd source path */
    void    *pk3_pending_data;      /* malloc'd payload buffer */
    size_t   pk3_pending_data_len;  /* length of pk3_pending_data */
};

typedef struct pakka_entry Pakfileentry_t;

struct pakka_reader {
    struct pakka_archive *archive;
    /* Re-seek position into the archive's single FILE* — multiple
     * readers stay coherent because pakka_reader_read seeks here
     * before every fread. */
    uint64_t next_offset;
    uint64_t remaining;

    /* ZIP-class DEFLATE path only (PK3/PK4). NULL for PAK and for
     * STORED ZIP entries. The compressed payload is read once,
     * inflated through pakka_inflate, and inflated_buf serves bytes
     * through reader_read. */
    unsigned char *inflated_buf;
    size_t         inflated_len;
    size_t         inflated_cursor;
};

/* True when `f` names a ZIP-container format (PK3 or PK4). PK4 is
 * byte-identical to PK3 on disk — Doom 3's archive — and both share
 * every internal dispatch into src/pk3file.c. */
static inline int pakka_format_is_zip(pakka_format_t f) {
    return f == PAKKA_FORMAT_PK3 || f == PAKKA_FORMAT_PK4;
}

/* Per-format geometry for PAK-class archives (Quake PAK, SiN, Daikatana).
 * pakka_pak_geometry(fmt) returns NULL for ZIP-class or any non-PAK
 * label; internal callers dispatch on (geometry != NULL ? PAK-class :
 * ZIP-class) before consulting the per-row fields below. */
typedef struct {
    const char *signature;       /* "PACK" or "SPAK" — 4 bytes, no NUL */
    size_t      name_field_len;  /* 56 (PAK/DK) or 120 (SiN) */
    size_t      dir_entry_size;  /* 64 (PAK) / 72 (DK) / 128 (SiN) */
    int         has_compression; /* 0 (PAK/SiN), 1 (DK) */
} pakka_pak_geometry_t;

const pakka_pak_geometry_t *pakka_pak_geometry(pakka_format_t fmt);

/* Release an entry plus any owned pending-add bookkeeping (ZIP queued
 * source paths or in-memory payload buffers). Safe on NULL. Replaces
 * raw free(entry) at every entry-release site so adding a new owned
 * field to pakka_entry can't leak. */
void pakka_entry_free(Pakfileentry_t *e);

struct pakka_archive {
    pakka_format_t format;          /* PAKKA_FORMAT_PAK, _PK3, or _PK4 */
    char signature[PAKFILE_SIGNATURE_LEN];
    uint32_t diroffset;
    uint32_t dirlength;
    uint32_t num_entries;
    uint64_t file_size;
    Pakfileentry_t *head;

    /* close fp before every rename — Windows CRT fopen omits
     * FILE_SHARE_DELETE, so a live handle blocks MoveFileEx. */
    FILE *fp;
    char cur_pakpath[OS_PATH_MAX];  /* set by open_pakfile */
    char new_pakpath[OS_PATH_MAX];  /* set by create_pakfile (destination) */
    char tmp_pakpath[OS_PATH_MAX];  /* mkstemp scratch; renamed into place on commit */
    int is_new;                     /* 1 if created via create_pakfile, 0 otherwise */
    int writable;                   /* 1 if pak->fp is r+b (PAKKA_OPEN_READ_WRITE or create) */
    int dirty;                      /* 1 if pakka_add/delete have unflushed changes */
    int needs_rebuild;              /* 1 if any pakka_delete touched the entry list — commit must rebuild via temp */

    /* Caller-supplied format hint from pakka_open_ex. AUTO means "probe
     * and decide from the on-disk magic + layout"; any other value
     * skips the PACK→Daikatana layout probe and asserts the on-disk
     * magic is compatible. Set before load_pakfile runs. */
    pakka_format_t format_hint;

    /* ZIP-class (PK3/PK4). The central directory starts here in the
     * file. New LFHs are written from this offset (replacing any prior
     * CDR + EOCD), with the new CDR + EOCD rewritten at commit. */
    uint32_t pk3_cdr_offset;
    /* Sticky compression mode applied to subsequent pakka_add_file /
     * pakka_add_memory calls — PK3_METHOD_STORED or PK3_METHOD_DEFLATE.
     * Zero-initialised by calloc so the default at pakka_open / create
     * time is STORED. Mutated only via pakka_set_compression (which
     * also enforces the read-only / non-ZIP guards). */
    uint16_t pk3_compression;
    /* Cap on per-entry decompressed bytes for pakka_open_entry and
     * pakka_read_entry_alloc. 0 disables the cap. Applies to both ZIP-
     * class (PK3/PK4) DEFLATE entries and Daikatana compressed entries —
     * the field used to be PK3-only (hence the historical name); see
     * pakka_set_max_decompressed_size for the public API. */
    uint64_t max_decompressed;
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
PAKKA_STATIC_ASSERT(
    PAKKA_ENTRY_NAME_SIZE >= PAKFILE_PATH_BUF,
    entry_name_size_fits_pak_path);

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

/* Internal ZIP dispatch helpers — called from src/pakfile.c when the
 * public pakka_* functions detect a ZIP-class archive (PK3 or PK4).
 * Both formats share the same implementation; the pakka_pk3_ prefix is
 * historical. Defined in src/pk3file.c. Not part of include/pakka.h;
 * pakka_-prefixed so they pass symbol-audit. */
pakka_status_t pakka_pk3_open_impl(struct pakka_archive *pak,
                                   pakka_error_t *err);
pakka_status_t pakka_pk3_open_entry_impl(struct pakka_archive *pak,
                                         struct pakka_reader *reader,
                                         struct pakka_entry *entry,
                                         pakka_error_t *err);
pakka_status_t pakka_pk3_reader_read_impl(struct pakka_reader *reader,
                                          void *buf, size_t len,
                                          size_t *nread,
                                          pakka_error_t *err);
void pakka_pk3_reader_close_impl(struct pakka_reader *reader);
pakka_status_t pakka_pk3_create_impl(struct pakka_archive *pak,
                                     pakka_error_t *err);
pakka_status_t pakka_pk3_add_file_impl(struct pakka_archive *pak,
                                       const char *source_path,
                                       const char *entry_name,
                                       pakka_error_t *err);
pakka_status_t pakka_pk3_add_memory_impl(struct pakka_archive *pak,
                                         const char *entry_name,
                                         const void *data, size_t len,
                                         pakka_error_t *err);
pakka_status_t pakka_pk3_commit_impl(struct pakka_archive *pak,
                                     pakka_error_t *err);
/* Deep verify: decompress an entry through pakka_inflate (or read as
 * STORED), confirm the uncompressed bytes hash to entry->pk3_crc32 and
 * total entry->length bytes. Returns OK or a populated err on
 * mismatch. */
pakka_status_t pakka_pk3_deep_verify_entry(struct pakka_archive *pak,
                                           struct pakka_entry *entry,
                                           pakka_error_t *err);

/* Daikatana custom byte-codec decoder. Whole-buffer decode: in/in_len is
 * the compressed payload, out/out_len is the destination buffer (must
 * equal entry->length — DK does not record decompressed size in the
 * stream itself). Strict bounds + exact-length contract:
 *  - every input read checked against in_len
 *  - every output write checked against out_len
 *  - back-reference distance must be 1..produced (no underflow)
 *  - LZ overlap (copy_len > distance) is legal and decoded byte-by-byte
 *  - termination accepts 0xFF terminator OR clean input exhaustion,
 *    but only if produced == out_len; partial fill returns
 *    PAKKA_ERR_FORMAT
 *  - opcode b == 254 is invalid and returns PAKKA_ERR_FORMAT
 * Returns PAKKA_OK on success or a populated err on any violation. */
pakka_status_t pakka_dk_inflate(const unsigned char *in, size_t in_len,
                                unsigned char *out, size_t out_len,
                                pakka_error_t *err);

/* Daikatana byte-codec encoder. Whole-buffer encode: in/in_len is the
 * raw payload, out/out_cap is the caller-allocated output buffer; on
 * success *out_len receives the bytes written.
 *
 * Worst case is in_len + ceil(in_len / 64) + 1. Callers size out_cap
 * to that worst case and clamp to UINT32_MAX (on-disk compressed_size
 * is u32); if the encoded payload isn't smaller than the source, fall
 * back to STORED.
 *
 * Returns PAKKA_ERR_LIMIT when out_cap can't hold the worst case.
 * Output is decodable by pakka_dk_inflate. */
pakka_status_t pakka_dk_deflate(const unsigned char *in, size_t in_len,
                                unsigned char *out, size_t out_cap,
                                size_t *out_len, pakka_error_t *err);
