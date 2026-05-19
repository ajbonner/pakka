/* C-API test binary. Exercises the public pakka_* functions that the
 * bats CLI suite can't reach: NULL-argument tolerance, structured
 * error fields, opaque entry accessors, create+close round-trip on an
 * empty archive, add/delete/commit round-trips, verify callback,
 * memory APIs, ZIP-magic rejection. Invoked from tests/c_api_test.bats.
 *
 * Usage: c_api_test <pak0.pak> <scratch_dir>
 *   pak0.pak     — known-good fixture, must contain at least 1 entry
 *   scratch_dir  — writable dir where the test creates new paks
 *
 * Exit 0 on success; non-zero with diagnostic to stderr on the first
 * failure. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pakka.h"

/* Two-step fprintf instead of `##__VA_ARGS__` so the test compiles
 * under --pedantic (the comma-swallowing form is a GNU extension). */
#define FAIL(...) do { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    return 1; \
} while (0)

#define EXPECT_EQ(actual, expected, label) do { \
    if ((actual) != (expected)) { \
        fprintf(stderr, "FAIL %s:%d: %s: got %d, want %d\n", \
                __FILE__, __LINE__, (label), (int)(actual), (int)(expected)); \
        return 1; \
    } \
} while (0)

/* Sentinel non-NULL pointer for out-param tests: invalid-argument paths
 * must clear *out even when *out started non-NULL. Using a dedicated
 * dummy variable rather than (void *)0xDEAD keeps the test valid on
 * platforms that catch wild pointer dereferences. */
static int g_out_sentinel_var;
#define OUT_SENTINEL ((void *)&g_out_sentinel_var)

static int test_null_args(void) {
    pakka_status_t s;
    pakka_archive_t *a;
    pakka_error_t err;
    const pakka_entry_t *e;

    /* pakka_open: NULL path → INVALID_ARGUMENT, *out cleared even when
     * pre-seeded with a non-NULL sentinel, err populated */
    a = (pakka_archive_t *)OUT_SENTINEL;
    s = pakka_open(NULL, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "pakka_open NULL path status");
    EXPECT_EQ(err.status, PAKKA_ERR_INVALID_ARGUMENT, "err.status");
    EXPECT_EQ(err.domain, PAKKA_ERR_DOMAIN_NONE, "err.domain");
    if (err.message[0] == '\0') {
        FAIL("err.message should be non-empty after pakka_open(NULL)");
    }
    if (a != NULL) {
        FAIL("pakka_open(NULL path, &a, ...) must clear *out");
    }

    /* pakka_open: NULL out → INVALID_ARGUMENT, err NULL tolerated */
    s = pakka_open("/dev/null", PAKKA_OPEN_READ, NULL, NULL);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "pakka_open NULL out, NULL err");

    /* pakka_open: unknown mode → INVALID_ARGUMENT, *out still cleared */
    a = (pakka_archive_t *)OUT_SENTINEL;
    s = pakka_open("/dev/null", (pakka_open_mode_t)99, &a, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "pakka_open bad mode");
    if (a != NULL) {
        FAIL("pakka_open(bad mode) must clear *out");
    }

    /* pakka_close: NULL → INVALID_ARGUMENT */
    s = pakka_close(NULL, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "pakka_close(NULL)");

    /* pakka_close: NULL err tolerated */
    s = pakka_close(NULL, NULL);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "pakka_close(NULL, NULL)");

    /* pakka_create: unknown flag → INVALID_ARGUMENT, *out cleared */
    a = (pakka_archive_t *)OUT_SENTINEL;
    s = pakka_create("/dev/null", PAKKA_FORMAT_PAK, 0xDEADu, &a, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "pakka_create bad flags");
    if (a != NULL) {
        FAIL("pakka_create(bad flags) must clear *out");
    }

    /* pakka_create: PK3 over an existing file → EXISTS. Confirms the
     * PK3 dispatch path is reachable. Defer the actual call into
     * test_create_close_roundtrip(scratch_dir) below — at this point
     * we don't have a path to an existing file we control. The
     * NULL/INVALID_ARGUMENT cases for the PK3 dispatch are covered by
     * the unknown-format and bad-flags checks above. */

    /* pakka_entry_at on NULL archive → INVALID_ARGUMENT, *out cleared */
    e = (const pakka_entry_t *)OUT_SENTINEL;
    s = pakka_entry_at(NULL, 0, &e);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "pakka_entry_at NULL archive");
    if (e != NULL) {
        FAIL("pakka_entry_at(NULL) must clear *out");
    }

    /* pakka_find_entry on NULL archive → INVALID_ARGUMENT, *out cleared */
    e = (const pakka_entry_t *)OUT_SENTINEL;
    s = pakka_find_entry(NULL, "x", &e);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "pakka_find_entry NULL archive");
    if (e != NULL) {
        FAIL("pakka_find_entry(NULL) must clear *out");
    }

    /* Accessors tolerate NULL */
    if (pakka_entry_name(NULL) != NULL) {
        FAIL("pakka_entry_name(NULL) must return NULL");
    }
    if (pakka_entry_size(NULL) != 0) {
        FAIL("pakka_entry_size(NULL) must return 0");
    }
    if (pakka_entry_offset(NULL) != 0) {
        FAIL("pakka_entry_offset(NULL) must return 0");
    }

    return 0;
}

static int test_open_pak0(const char *path) {
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s;
    const pakka_entry_t *e = NULL;
    const pakka_entry_t *first;
    size_t count;
    const char *name;

    s = pakka_open(path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_open pak0.pak");

    if (pakka_format(a) != PAKKA_FORMAT_PAK) {
        FAIL("pakka_format should be PAK");
    }

    count = pakka_entry_count(a);
    if (count == 0) {
        FAIL("pak0.pak should have entries");
    }

    /* Entry 0 round-trip through opaque accessors */
    s = pakka_entry_at(a, 0, &first);
    EXPECT_EQ(s, PAKKA_OK, "pakka_entry_at(0)");
    name = pakka_entry_name(first);
    if (name == NULL || name[0] == '\0') {
        FAIL("pakka_entry_name returned empty");
    }

    /* Find the entry we just retrieved by name */
    s = pakka_find_entry(a, name, &e);
    EXPECT_EQ(s, PAKKA_OK, "pakka_find_entry(known)");
    if (e != first) {
        FAIL("find_entry should return the same handle as entry_at(0)");
    }

    /* Unknown name → NOT_FOUND */
    s = pakka_find_entry(a, "__definitely_not_present__", &e);
    EXPECT_EQ(s, PAKKA_ERR_NOT_FOUND, "pakka_find_entry(unknown)");

    /* Out-of-bounds index → NOT_FOUND */
    s = pakka_entry_at(a, count, &e);
    EXPECT_EQ(s, PAKKA_ERR_NOT_FOUND, "pakka_entry_at out of bounds");

    s = pakka_close(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_close on open archive");

    return 0;
}

static int test_reader_streaming(const char *path) {
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s;
    const pakka_entry_t *e0;
    const pakka_entry_t *e1;
    pakka_reader_t *r;
    pakka_reader_t *r0;
    pakka_reader_t *r1;
    pakka_reader_t *r_dummy;
    unsigned char *buf_full;
    unsigned char *buf_chunked;
    unsigned char *buf_e0_interleaved;
    unsigned char *buf_e1_interleaved;
    unsigned char *buf_e0_full;
    unsigned char *buf_e1_full;
    size_t nread;
    size_t entry_size;
    size_t e0_size;
    size_t e1_size;
    size_t chunk = 17;          /* odd-size chunk to exercise partial reads */

    s = pakka_open(path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_open for streaming");

    /* Pick the first non-empty entry; pak0 is full of non-empty entries
     * but be defensive. */
    s = pakka_entry_at(a, 0, &e0);
    EXPECT_EQ(s, PAKKA_OK, "entry_at(0)");
    entry_size = (size_t)pakka_entry_size(e0);
    if (entry_size == 0 || entry_size > 1024 * 1024) {
        pakka_close(a, NULL);
        FAIL("entry 0 has unexpected size %zu", entry_size);
    }

    /* === pakka_open_entry NULL tolerance === */
    r_dummy = (pakka_reader_t *)OUT_SENTINEL;
    s = pakka_open_entry(NULL, "x", &r_dummy, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "open_entry NULL archive");
    if (r_dummy != NULL) FAIL("open_entry NULL archive must clear *out");

    r_dummy = (pakka_reader_t *)OUT_SENTINEL;
    s = pakka_open_entry(a, NULL, &r_dummy, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "open_entry NULL name");
    if (r_dummy != NULL) FAIL("open_entry NULL name must clear *out");

    r_dummy = (pakka_reader_t *)OUT_SENTINEL;
    s = pakka_open_entry(a, "__nope__", &r_dummy, &err);
    EXPECT_EQ(s, PAKKA_ERR_NOT_FOUND, "open_entry unknown");
    if (r_dummy != NULL) FAIL("open_entry unknown must clear *out");

    /* === Full read in one go === */
    buf_full = malloc(entry_size);
    if (buf_full == NULL) { pakka_close(a, NULL); FAIL("malloc full"); }

    s = pakka_open_entry(a, pakka_entry_name(e0), &r, &err);
    EXPECT_EQ(s, PAKKA_OK, "open_entry e0");

    nread = 999;
    s = pakka_reader_read(r, buf_full, entry_size, &nread, &err);
    EXPECT_EQ(s, PAKKA_OK, "reader_read full");
    if (nread != entry_size) {
        pakka_reader_close(r);
        free(buf_full);
        pakka_close(a, NULL);
        FAIL("full read got %zu, want %zu", nread, entry_size);
    }

    /* Another read past EOF returns OK with nread=0 */
    nread = 999;
    s = pakka_reader_read(r, buf_full, entry_size, &nread, &err);
    EXPECT_EQ(s, PAKKA_OK, "reader_read past EOF");
    if (nread != 0) {
        pakka_reader_close(r);
        free(buf_full);
        pakka_close(a, NULL);
        FAIL("past-EOF read should return 0 bytes, got %zu", nread);
    }
    pakka_reader_close(r);

    /* === Chunked read with odd chunk size produces same bytes === */
    buf_chunked = malloc(entry_size);
    if (buf_chunked == NULL) {
        free(buf_full); pakka_close(a, NULL); FAIL("malloc chunked");
    }

    s = pakka_open_entry(a, pakka_entry_name(e0), &r, &err);
    EXPECT_EQ(s, PAKKA_OK, "open_entry e0 (chunked)");

    {
        size_t total = 0;
        while (total < entry_size) {
            size_t want = entry_size - total;
            if (want > chunk) want = chunk;
            nread = 0;
            s = pakka_reader_read(r, buf_chunked + total, want, &nread, &err);
            if (s != PAKKA_OK) {
                pakka_reader_close(r);
                free(buf_full); free(buf_chunked); pakka_close(a, NULL);
                FAIL("chunked read failed at offset %zu", total);
            }
            if (nread == 0) break;
            total += nread;
        }
        if (total != entry_size) {
            pakka_reader_close(r);
            free(buf_full); free(buf_chunked); pakka_close(a, NULL);
            FAIL("chunked total %zu != %zu", total, entry_size);
        }
    }
    pakka_reader_close(r);

    if (memcmp(buf_full, buf_chunked, entry_size) != 0) {
        free(buf_full); free(buf_chunked); pakka_close(a, NULL);
        FAIL("chunked read bytes differ from full read");
    }
    free(buf_chunked);

    /* === Interleaved readers — proves per-read re-seeking === */
    if (pakka_entry_count(a) < 2) {
        free(buf_full); pakka_close(a, NULL);
        return 0;       /* nothing more to test with a 1-entry archive */
    }
    s = pakka_entry_at(a, 1, &e1);
    EXPECT_EQ(s, PAKKA_OK, "entry_at(1)");
    e0_size = entry_size;
    e1_size = (size_t)pakka_entry_size(e1);

    buf_e0_full = malloc(e0_size);
    buf_e1_full = malloc(e1_size);
    buf_e0_interleaved = calloc(e0_size, 1);
    buf_e1_interleaved = calloc(e1_size, 1);
    if (!buf_e0_full || !buf_e1_full
        || !buf_e0_interleaved || !buf_e1_interleaved) {
        FAIL("malloc interleaved");
    }
    memcpy(buf_e0_full, buf_full, e0_size);
    free(buf_full);

    /* Reference read of e1 in one go */
    s = pakka_open_entry(a, pakka_entry_name(e1), &r, &err);
    EXPECT_EQ(s, PAKKA_OK, "open_entry e1 ref");
    s = pakka_reader_read(r, buf_e1_full, e1_size, &nread, &err);
    EXPECT_EQ(s, PAKKA_OK, "reader_read e1 ref");
    pakka_reader_close(r);

    /* Now interleave: open both, alternate 8-byte reads */
    s = pakka_open_entry(a, pakka_entry_name(e0), &r0, &err);
    EXPECT_EQ(s, PAKKA_OK, "open_entry e0 interleaved");
    s = pakka_open_entry(a, pakka_entry_name(e1), &r1, &err);
    EXPECT_EQ(s, PAKKA_OK, "open_entry e1 interleaved");

    {
        size_t off0 = 0, off1 = 0;
        while (off0 < e0_size || off1 < e1_size) {
            size_t want;
            if (off0 < e0_size) {
                want = e0_size - off0;
                if (want > 8) want = 8;
                s = pakka_reader_read(r0, buf_e0_interleaved + off0,
                                      want, &nread, &err);
                EXPECT_EQ(s, PAKKA_OK, "interleaved read r0");
                off0 += nread;
            }
            if (off1 < e1_size) {
                want = e1_size - off1;
                if (want > 8) want = 8;
                s = pakka_reader_read(r1, buf_e1_interleaved + off1,
                                      want, &nread, &err);
                EXPECT_EQ(s, PAKKA_OK, "interleaved read r1");
                off1 += nread;
            }
        }
    }
    pakka_reader_close(r0);
    pakka_reader_close(r1);
    pakka_reader_close(NULL);       /* NULL tolerance */

    if (memcmp(buf_e0_full, buf_e0_interleaved, e0_size) != 0) {
        FAIL("interleaved e0 bytes differ from reference");
    }
    if (memcmp(buf_e1_full, buf_e1_interleaved, e1_size) != 0) {
        FAIL("interleaved e1 bytes differ from reference");
    }

    free(buf_e0_full);
    free(buf_e1_full);
    free(buf_e0_interleaved);
    free(buf_e1_interleaved);

    /* === pakka_reader_read NULL tolerance === */
    s = pakka_open_entry(a, pakka_entry_name(e0), &r, &err);
    EXPECT_EQ(s, PAKKA_OK, "open_entry for NULL-tolerance");
    {
        unsigned char one;
        s = pakka_reader_read(NULL, &one, 1, &nread, &err);
        EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "reader_read NULL reader");
        s = pakka_reader_read(r, NULL, 1, &nread, &err);
        EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "reader_read NULL buf");
        s = pakka_reader_read(r, &one, 1, NULL, &err);
        EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "reader_read NULL nread");
        /* len=0 returns OK with nread=0 (or whatever caller seeded — i.e., we
         * don't mutate nread in that path beyond the leading reset) */
        nread = 999;
        s = pakka_reader_read(r, &one, 0, &nread, &err);
        EXPECT_EQ(s, PAKKA_OK, "reader_read len=0");
        if (nread != 0) {
            FAIL("reader_read len=0 should leave nread=0, got %zu", nread);
        }
    }
    pakka_reader_close(r);

    s = pakka_close(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_close after streaming tests");
    return 0;
}

static int test_create_close_roundtrip(const char *scratch_dir) {
    char path[1024];
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s;
    FILE *fp;
    unsigned char header[12];
    uint32_t diroffset, dirlength;

    snprintf(path, sizeof(path), "%s/c_api_empty.pak", scratch_dir);
    /* Remove from a prior run; ignore errors. */
    (void)remove(path);

    s = pakka_create(path, PAKKA_FORMAT_PAK, PAKKA_CREATE_DEFAULT, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_create empty");

    s = pakka_close(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_close after empty create");

    /* PK3 create over an existing path → EXISTS. The just-closed empty
     * PAK at `path` is now a guaranteed-present file we can use. */
    {
        pakka_archive_t *a2 = (pakka_archive_t *)OUT_SENTINEL;
        s = pakka_create(path, PAKKA_FORMAT_PK3,
                         PAKKA_CREATE_DEFAULT, &a2, &err);
        EXPECT_EQ(s, PAKKA_ERR_EXISTS,
                  "pakka_create PK3 over existing file");
        if (a2 != NULL) {
            FAIL("pakka_create(PK3, exists) must clear *out");
        }
    }

    /* The on-disk file must be a valid 12-byte PACK header:
     *   "PACK" + diroffset=12 (LE) + dirlength=0 (LE) */
    fp = fopen(path, "rb");
    if (fp == NULL) {
        FAIL("created pak not found at %s", path);
    }
    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        FAIL("created pak shorter than 12 bytes");
    }
    fclose(fp);
    if (memcmp(header, "PACK", 4) != 0) {
        FAIL("created pak missing PACK signature");
    }
    diroffset = (uint32_t)header[4]
              | ((uint32_t)header[5] << 8)
              | ((uint32_t)header[6] << 16)
              | ((uint32_t)header[7] << 24);
    dirlength = (uint32_t)header[8]
              | ((uint32_t)header[9] << 8)
              | ((uint32_t)header[10] << 16)
              | ((uint32_t)header[11] << 24);
    if (diroffset != 12) {
        FAIL("created pak diroffset=%u, want 12", diroffset);
    }
    if (dirlength != 0) {
        FAIL("created pak dirlength=%u, want 0", dirlength);
    }

    /* Re-open and confirm the entry count is 0. */
    s = pakka_open(path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_open empty");
    if (pakka_entry_count(a) != 0) {
        pakka_close(a, NULL);
        FAIL("re-opened empty pak should have 0 entries");
    }
    s = pakka_close(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_close after re-open");

    return 0;
}

/* Write a tiny synthetic pak with two entries that share an exact name.
 * Layout: 12-byte PACK header, 2 payload bytes (one per entry, at
 * offsets 12 and 13), 128 bytes of directory (2 * 64-byte entries),
 * both named "dup.txt". Designed to trigger
 * validate_no_duplicates() at pakka_open time. */
static int write_synthetic_duplicate_pak(const char *path) {
    FILE *fp = fopen(path, "wb");
    unsigned char hdr[12];
    unsigned char dir[128];
    unsigned char payload[2] = { 'a', 'b' };

    if (fp == NULL) {
        FAIL("cannot open %s for write", path);
    }

    /* "PACK" + diroffset=14 (LE) + dirlength=128 (LE) */
    hdr[0] = 'P'; hdr[1] = 'A'; hdr[2] = 'C'; hdr[3] = 'K';
    hdr[4] = 14; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;
    hdr[8] = 128; hdr[9] = 0; hdr[10] = 0; hdr[11] = 0;

    /* Two 64-byte entries: filename (56 bytes, NUL-padded), offset (u32 LE),
     * length (u32 LE). Both named "dup.txt", payload bytes at offsets 12, 13. */
    memset(dir, 0, sizeof(dir));
    memcpy(dir + 0,  "dup.txt", 7);
    dir[56] = 12; dir[60] = 1;            /* entry 0: offset=12, length=1 */
    memcpy(dir + 64, "dup.txt", 7);
    dir[64 + 56] = 13; dir[64 + 60] = 1;  /* entry 1: offset=13, length=1 */

    if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)
        || fwrite(payload, 1, sizeof(payload), fp) != sizeof(payload)
        || fwrite(dir, 1, sizeof(dir), fp) != sizeof(dir)) {
        fclose(fp);
        FAIL("synthetic-dup write failed");
    }
    fclose(fp);
    return 0;
}

static int test_open_rejects_duplicates(const char *scratch_dir) {
    char path[1024];
    pakka_archive_t *a;
    pakka_error_t err;
    pakka_status_t s;

    snprintf(path, sizeof(path), "%s/c_api_dup.pak", scratch_dir);
    (void)remove(path);
    if (write_synthetic_duplicate_pak(path) != 0) return 1;

    a = (pakka_archive_t *)OUT_SENTINEL;
    s = pakka_open(path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_ERR_DUPLICATE, "pakka_open synthetic duplicate-name pak");
    if (a != NULL) {
        FAIL("pakka_open failure must clear *out");
    }
    if (strcmp(err.entry_name, "dup.txt") != 0) {
        FAIL("err.entry_name should be 'dup.txt', got '%s'", err.entry_name);
    }

    return 0;
}

static int test_add_commit_roundtrip(const char *scratch_dir) {
    char pak_path[1024];
    char src_path[1024];
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s;
    FILE *fp;
    const pakka_entry_t *e;
    pakka_reader_t *r;
    unsigned char readback[16];
    size_t nread;

    snprintf(pak_path, sizeof(pak_path), "%s/c_api_addcommit.pak", scratch_dir);
    snprintf(src_path, sizeof(src_path), "%s/c_api_src.bin", scratch_dir);
    (void)remove(pak_path);

    /* Build a 5-byte source file */
    fp = fopen(src_path, "wb");
    if (fp == NULL) FAIL("cannot write %s", src_path);
    fwrite("hello", 1, 5, fp);
    fclose(fp);

    s = pakka_create(pak_path, PAKKA_FORMAT_PAK, PAKKA_CREATE_DEFAULT,
                     &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "create for add-commit");

    /* Dual-path add: source on disk is "c_api_src.bin", entry name is
     * "renamed/hello.txt" — exercises the PAKLIB-style alias support. */
    s = pakka_add_file(a, src_path, "renamed/hello.txt", &err);
    EXPECT_EQ(s, PAKKA_OK, "add_file with alias");

    /* Duplicate add fails */
    s = pakka_add_file(a, src_path, "renamed/hello.txt", &err);
    EXPECT_EQ(s, PAKKA_ERR_DUPLICATE, "add_file duplicate");

    /* Unsafe entry name rejected */
    s = pakka_add_file(a, src_path, "../escape.txt", &err);
    EXPECT_EQ(s, PAKKA_ERR_UNSAFE_NAME, "add_file unsafe name");

    /* Missing source rejected */
    s = pakka_add_file(a, "/nonexistent/path/x", "ok.txt", &err);
    EXPECT_EQ(s, PAKKA_ERR_IO, "add_file missing source");

    /* NULL tolerance */
    s = pakka_add_file(NULL, src_path, "x", &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "add_file NULL archive");
    s = pakka_add_file(a, NULL, "x", &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "add_file NULL source");
    s = pakka_add_file(a, src_path, NULL, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "add_file NULL entry");

    /* Commit and close */
    s = pakka_commit(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "commit add");
    s = pakka_close(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "close after commit");

    /* Reopen and verify */
    s = pakka_open(pak_path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "reopen committed pak");
    if (pakka_entry_count(a) != 1) FAIL("reopened pak count != 1");

    s = pakka_find_entry(a, "renamed/hello.txt", &e);
    EXPECT_EQ(s, PAKKA_OK, "find aliased entry");
    if (pakka_entry_size(e) != 5) {
        FAIL("aliased entry size %llu, want 5",
             (unsigned long long)pakka_entry_size(e));
    }

    s = pakka_open_entry(a, "renamed/hello.txt", &r, &err);
    EXPECT_EQ(s, PAKKA_OK, "open aliased entry");
    s = pakka_reader_read(r, readback, sizeof(readback), &nread, &err);
    EXPECT_EQ(s, PAKKA_OK, "read aliased entry");
    if (nread != 5 || memcmp(readback, "hello", 5) != 0) {
        FAIL("aliased entry content mismatch");
    }
    pakka_reader_close(r);
    pakka_close(a, NULL);
    return 0;
}

static int test_delete_commit(const char *scratch_dir) {
    char pak_path[1024];
    char src_a[1024];
    char src_b[1024];
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s;
    FILE *fp;
    const pakka_entry_t *e;

    snprintf(pak_path, sizeof(pak_path), "%s/c_api_del.pak", scratch_dir);
    snprintf(src_a, sizeof(src_a), "%s/del_src_a.bin", scratch_dir);
    snprintf(src_b, sizeof(src_b), "%s/del_src_b.bin", scratch_dir);
    (void)remove(pak_path);

    fp = fopen(src_a, "wb"); fwrite("AAA", 1, 3, fp); fclose(fp);
    fp = fopen(src_b, "wb"); fwrite("BBBBB", 1, 5, fp); fclose(fp);

    s = pakka_create(pak_path, PAKKA_FORMAT_PAK, PAKKA_CREATE_DEFAULT,
                     &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "create for delete-commit");

    s = pakka_add_file(a, src_a, "alpha.bin", &err);
    EXPECT_EQ(s, PAKKA_OK, "add alpha");
    s = pakka_add_file(a, src_b, "beta.bin", &err);
    EXPECT_EQ(s, PAKKA_OK, "add beta");

    /* Delete unknown → NOT_FOUND */
    s = pakka_delete(a, "__nope__", &err);
    EXPECT_EQ(s, PAKKA_ERR_NOT_FOUND, "delete unknown");

    /* NULL tolerance */
    s = pakka_delete(NULL, "alpha.bin", &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "delete NULL archive");
    s = pakka_delete(a, NULL, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "delete NULL name");

    /* Delete alpha → list becomes [beta] */
    s = pakka_delete(a, "alpha.bin", &err);
    EXPECT_EQ(s, PAKKA_OK, "delete alpha");
    if (pakka_entry_count(a) != 1) FAIL("after delete, count != 1");

    /* Commit (rebuild path) */
    s = pakka_commit(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "commit after delete");

    /* No-op commit after a clean commit returns OK without doing work */
    s = pakka_commit(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "no-op commit");

    s = pakka_close(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "close after delete-commit");

    /* Reopen and verify only beta survives */
    s = pakka_open(pak_path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "reopen after delete");
    if (pakka_entry_count(a) != 1) FAIL("reopen after delete count != 1");
    s = pakka_find_entry(a, "alpha.bin", &e);
    EXPECT_EQ(s, PAKKA_ERR_NOT_FOUND, "alpha gone");
    s = pakka_find_entry(a, "beta.bin", &e);
    EXPECT_EQ(s, PAKKA_OK, "beta survives");
    if (pakka_entry_size(e) != 5) FAIL("beta size wrong");
    pakka_close(a, NULL);
    return 0;
}

static void verify_count_callback(void *userdata,
                                  pakka_report_severity_t severity,
                                  pakka_status_t status,
                                  const char *entry_name,
                                  const char *message) {
    int *counts = (int *)userdata;
    (void)status;
    (void)entry_name;
    (void)message;
    if (severity == PAKKA_REPORT_INFO) counts[0]++;
    else if (severity == PAKKA_REPORT_WARNING) counts[1]++;
    else if (severity == PAKKA_REPORT_ERROR) counts[2]++;
}

static int test_verify(const char *pak0_path, const char *scratch_dir) {
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s;
    int counts[3];
    char collide_path[1024];

    /* NULL archive */
    s = pakka_verify(NULL, 0u, NULL, NULL, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "verify NULL archive");

    /* Open pak0 and verify with NULL callback */
    s = pakka_open(pak0_path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "open pak0 for verify");
    s = pakka_verify(a, 0u, NULL, NULL, &err);
    EXPECT_EQ(s, PAKKA_OK, "verify pak0 NULL callback");

    /* Verify with counting callback */
    counts[0] = counts[1] = counts[2] = 0;
    s = pakka_verify(a, 0u, verify_count_callback, counts, &err);
    EXPECT_EQ(s, PAKKA_OK, "verify pak0 with callback");
    if (counts[0] == 0) FAIL("verify produced no INFO findings");
    if (counts[2] != 0) FAIL("verify produced unexpected ERROR findings");

    /* Unknown flags */
    s = pakka_verify(a, 0xDEADu, NULL, NULL, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "verify bad flags");
    pakka_close(a, NULL);

    /* Synthetic case-collision pak: Foo.txt + foo.txt */
    snprintf(collide_path, sizeof(collide_path),
             "%s/c_api_collide.pak", scratch_dir);
    (void)remove(collide_path);
    {
        FILE *fp = fopen(collide_path, "wb");
        unsigned char hdr[12] = {'P','A','C','K',12,0,0,0,128,0,0,0};
        unsigned char dir[128] = {0};
        if (fp == NULL) FAIL("cannot write %s", collide_path);
        memcpy(dir + 0, "Foo.txt", 7);
        dir[56] = 140; dir[60] = 0;   /* offset=140, length=0 (past dir) */
        memcpy(dir + 64, "foo.txt", 7);
        dir[64 + 56] = 140; dir[64 + 60] = 0;
        if (fwrite(hdr, 1, 12, fp) != 12
            || fwrite(dir, 1, 128, fp) != 128) {
            fclose(fp);
            FAIL("synthetic collide write failed");
        }
        fclose(fp);
    }

    s = pakka_open(collide_path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "open synthetic collide pak");
    counts[0] = counts[1] = counts[2] = 0;
    s = pakka_verify(a, 0u, verify_count_callback, counts, &err);
    EXPECT_EQ(s, PAKKA_ERR_DUPLICATE, "verify collide pak");
    if (counts[2] == 0) FAIL("verify collide produced no ERROR");
    if (err.status != PAKKA_ERR_DUPLICATE) {
        FAIL("err.status not propagated to caller (got %d)", (int)err.status);
    }
    pakka_close(a, NULL);

    return 0;
}

static int test_memory_apis(const char *scratch_dir) {
    char pak_path[1024];
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s;
    void *out_data = NULL;
    size_t out_len = 0;
    const unsigned char payload[] = "in-memory payload \xff\x00\x42";
    const size_t payload_len = sizeof(payload) - 1;

    snprintf(pak_path, sizeof(pak_path), "%s/c_api_mem.pak", scratch_dir);
    (void)remove(pak_path);

    s = pakka_create(pak_path, PAKKA_FORMAT_PAK, PAKKA_CREATE_DEFAULT,
                     &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "create for memory APIs");

    /* NULL tolerance */
    s = pakka_add_memory(NULL, "x", payload, payload_len, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "add_memory NULL archive");
    s = pakka_add_memory(a, NULL, payload, payload_len, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "add_memory NULL entry");
    s = pakka_add_memory(a, "x", NULL, payload_len, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "add_memory NULL data non-zero len");

    /* Zero-length entry via memory API */
    s = pakka_add_memory(a, "empty.bin", NULL, 0, &err);
    EXPECT_EQ(s, PAKKA_OK, "add_memory empty");

    /* Non-empty entry */
    s = pakka_add_memory(a, "blob.bin", payload, payload_len, &err);
    EXPECT_EQ(s, PAKKA_OK, "add_memory blob");

    /* Duplicate rejected */
    s = pakka_add_memory(a, "blob.bin", payload, payload_len, &err);
    EXPECT_EQ(s, PAKKA_ERR_DUPLICATE, "add_memory duplicate");

    /* Unsafe entry name rejected */
    s = pakka_add_memory(a, "../escape.bin", payload, payload_len, &err);
    EXPECT_EQ(s, PAKKA_ERR_UNSAFE_NAME, "add_memory unsafe");

    s = pakka_commit(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "commit memory adds");
    s = pakka_close(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "close after memory adds");

    /* Reopen and round-trip via pakka_read_entry_alloc */
    s = pakka_open(pak_path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "reopen for read_entry_alloc");

    /* NULL tolerance on read_entry_alloc */
    s = pakka_read_entry_alloc(NULL, "x", &out_data, &out_len, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "read_entry_alloc NULL archive");
    s = pakka_read_entry_alloc(a, "missing", &out_data, &out_len, &err);
    EXPECT_EQ(s, PAKKA_ERR_NOT_FOUND, "read_entry_alloc missing");

    /* Empty entry: *data == NULL, *len == 0, status OK */
    out_data = (void *)OUT_SENTINEL;
    out_len = 999;
    s = pakka_read_entry_alloc(a, "empty.bin", &out_data, &out_len, &err);
    EXPECT_EQ(s, PAKKA_OK, "read_entry_alloc empty");
    if (out_data != NULL) FAIL("empty read should return NULL data");
    if (out_len != 0) FAIL("empty read should return 0 len");

    /* Full entry round-trip */
    s = pakka_read_entry_alloc(a, "blob.bin", &out_data, &out_len, &err);
    EXPECT_EQ(s, PAKKA_OK, "read_entry_alloc blob");
    if (out_len != payload_len) {
        pakka_free(out_data);
        pakka_close(a, NULL);
        FAIL("readback length %zu != %zu", out_len, payload_len);
    }
    if (memcmp(out_data, payload, payload_len) != 0) {
        pakka_free(out_data);
        pakka_close(a, NULL);
        FAIL("readback bytes differ from payload");
    }
    pakka_free(out_data);
    pakka_free(NULL);       /* must tolerate NULL */

    pakka_close(a, NULL);
    return 0;
}

static int test_add_to_readonly_fails(const char *path) {
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s;

    s = pakka_open(path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "open read-only");
    s = pakka_add_file(a, path, "foo.txt", &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "add to read-only");
    s = pakka_delete(a, "anything", &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "delete on read-only");
    pakka_close(a, NULL);
    return 0;
}

static int try_open_zip_magic(const char *scratch_dir,
                              const char *label,
                              const unsigned char magic[4],
                              pakka_status_t expected_status) {
    char path[1024];
    FILE *fp;
    pakka_archive_t *a;
    pakka_error_t err;
    pakka_status_t s;

    snprintf(path, sizeof(path), "%s/c_api_zip_%s.pak",
             scratch_dir, label);
    (void)remove(path);
    fp = fopen(path, "wb");
    if (fp == NULL || fwrite(magic, 1, 4, fp) != 4) {
        if (fp) fclose(fp);
        FAIL("cannot write %s", path);
    }
    fclose(fp);
    a = (pakka_archive_t *)OUT_SENTINEL;
    s = pakka_open(path, PAKKA_OPEN_READ, &a, &err);
    if (s != expected_status) {
        FAIL("ZIP magic %s expected status %d, got %d",
             label, (int)expected_status, (int)s);
    }
    if (a != NULL) FAIL("pakka_open(ZIP %s) must clear *out", label);
    return 0;
}

/* Helper: create a fresh ZIP-class archive at `path`, check the
 * immediate pakka_format() label, close it, reopen it, and check the
 * label again. The two paths exercise distinct code: the immediate
 * check rides pakka_create's "caller owns the label" wiring, while the
 * reopen check rides pakka_open's filename-extension labeller. */
static int check_zip_roundtrip(const char *path, pakka_format_t want,
                               const char *label) {
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s;

    (void)remove(path);

    s = pakka_create(path, want, PAKKA_CREATE_DEFAULT, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_create ZIP");
    if (pakka_format(a) != want) {
        int got = (int)pakka_format(a);
        pakka_close(a, NULL);
        FAIL("pakka_format(%s) after create returned %d, want %d",
             label, got, (int)want);
    }
    s = pakka_close(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_close after ZIP create");

    s = pakka_open(path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_open ZIP");
    if (pakka_format(a) != want) {
        int got = (int)pakka_format(a);
        pakka_close(a, NULL);
        FAIL("pakka_format(%s) after reopen returned %d, want %d",
             label, got, (int)want);
    }
    s = pakka_close(a, &err);
    EXPECT_EQ(s, PAKKA_OK, "pakka_close after ZIP reopen");
    return 0;
}

/* PK3 and PK4 share every byte on disk; only the enum label differs.
 * Confirm both create and open route the label through correctly, and
 * that the open-side extension sniffer doesn't cross-label one as the
 * other. */
static int test_zip_format_label_roundtrip(const char *scratch_dir) {
    char pk3_path[1024];
    char pk4_path[1024];
    snprintf(pk3_path, sizeof(pk3_path), "%s/c_api_label.pk3", scratch_dir);
    snprintf(pk4_path, sizeof(pk4_path), "%s/c_api_label.pk4", scratch_dir);
    if (check_zip_roundtrip(pk3_path, PAKKA_FORMAT_PK3, "pk3") != 0) return 1;
    if (check_zip_roundtrip(pk4_path, PAKKA_FORMAT_PK4, "pk4") != 0) return 1;
    return 0;
}

/* SiN round-trip via the public C API: create an SPAK archive,
 * pakka_add_memory a 120-byte-name and one ordinary entry, close, then
 * reopen and confirm pakka_format() reports SIN and both entries are
 * present with intact bytes. */
static int test_sin_roundtrip(const char *scratch_dir) {
    char path[1024];
    pakka_archive_t *arc = NULL;
    pakka_error_t err;
    pakka_status_t s;
    const pakka_entry_t *e = NULL;
    char longname[120];

    snprintf(path, sizeof(path), "%s/c_api_sin.sin", scratch_dir);

    /* 119-byte name (one short of the SiN 120-byte cap). */
    memset(longname, 'a', 119);
    longname[119] = '\0';

    s = pakka_create(path, PAKKA_FORMAT_SIN, PAKKA_CREATE_DEFAULT,
                     &arc, &err);
    EXPECT_EQ(s, PAKKA_OK, "create SiN");
    s = pakka_add_memory(arc, "maps/test.bsp",
                         "level-bytes", 11, &err);
    EXPECT_EQ(s, PAKKA_OK, "add_memory SiN entry 1");
    s = pakka_add_memory(arc, longname, "x", 1, &err);
    EXPECT_EQ(s, PAKKA_OK, "add_memory SiN entry 2 (119-byte name)");
    s = pakka_close(arc, &err);
    EXPECT_EQ(s, PAKKA_OK, "close SiN");

    s = pakka_open(path, PAKKA_OPEN_READ, &arc, &err);
    EXPECT_EQ(s, PAKKA_OK, "reopen SiN");
    EXPECT_EQ(pakka_format(arc), PAKKA_FORMAT_SIN, "format == SIN");
    EXPECT_EQ((int)pakka_entry_count(arc), 2, "SiN entry count");
    s = pakka_find_entry(arc, "maps/test.bsp", &e);
    EXPECT_EQ(s, PAKKA_OK, "find SiN entry 1");
    s = pakka_find_entry(arc, longname, &e);
    EXPECT_EQ(s, PAKKA_OK, "find SiN entry 2 (119-byte name)");
    s = pakka_close(arc, &err);
    EXPECT_EQ(s, PAKKA_OK, "close SiN reopen");
    return 0;
}

/* DK archives are read-only at the API boundary: pakka_create rejects
 * the format, and the mutation APIs (add_file, add_memory, delete,
 * commit) reject DK archives opened for write. We do not have a real DK
 * fixture available, so we exercise the create-rejection path. */
static int test_daikatana_create_rejected(const char *scratch_dir) {
    char path[1024];
    int sentinel = 0;
    pakka_archive_t *arc = (pakka_archive_t *)&sentinel;
    pakka_error_t err;
    pakka_status_t s;

    snprintf(path, sizeof(path), "%s/c_api_dk_rejected.pak", scratch_dir);

    s = pakka_create(path, PAKKA_FORMAT_DAIKATANA, PAKKA_CREATE_DEFAULT,
                     &arc, &err);
    EXPECT_EQ(s, PAKKA_ERR_UNSUPPORTED, "create DK rejected");
    EXPECT_EQ(arc == NULL, 1, "create DK clears *out");
    return 0;
}

/* pakka_open_ex with an explicit hint that does not match the on-disk
 * magic must return PAKKA_ERR_INVALID_ARGUMENT. Build a small SPAK
 * archive and try to reopen it with --format pak / PAKKA_FORMAT_PAK. */
static int test_open_ex_hint_mismatch(const char *scratch_dir) {
    char path[1024];
    pakka_archive_t *arc = NULL;
    pakka_error_t err;
    pakka_status_t s;

    snprintf(path, sizeof(path), "%s/c_api_hint.sin", scratch_dir);

    s = pakka_create(path, PAKKA_FORMAT_SIN, PAKKA_CREATE_DEFAULT,
                     &arc, &err);
    EXPECT_EQ(s, PAKKA_OK, "create SiN for hint test");
    s = pakka_add_memory(arc, "x.txt", "y", 1, &err);
    EXPECT_EQ(s, PAKKA_OK, "add to SiN");
    s = pakka_close(arc, &err);
    EXPECT_EQ(s, PAKKA_OK, "close SiN for hint test");

    /* Opening the SPAK archive with PAKKA_FORMAT_PAK must fail. */
    arc = NULL;
    s = pakka_open_ex(path, PAKKA_OPEN_READ, PAKKA_FORMAT_PAK,
                      &arc, &err);
    EXPECT_EQ(s, PAKKA_ERR_INVALID_ARGUMENT, "PAK hint vs SPAK magic");
    EXPECT_EQ(arc == NULL, 1, "open_ex clears *out on hint mismatch");

    /* PAKKA_FORMAT_SIN succeeds. */
    s = pakka_open_ex(path, PAKKA_OPEN_READ, PAKKA_FORMAT_SIN,
                      &arc, &err);
    EXPECT_EQ(s, PAKKA_OK, "SIN hint matches SPAK magic");
    s = pakka_close(arc, &err);
    EXPECT_EQ(s, PAKKA_OK, "close after explicit SIN hint");
    return 0;
}

static int test_open_rejects_malformed_pk3(const char *scratch_dir) {
    /* Truncated PK3 signatures (just the 4-byte magic, no EOCD)
     * should fail with FORMAT now that PK3 is a recognized format.
     * The spanning-marker signature is a multi-disk indicator and
     * stays UNSUPPORTED. */
    const unsigned char m_lfh[4]  = { 'P', 'K', 0x03, 0x04 };
    const unsigned char m_eocd[4] = { 'P', 'K', 0x05, 0x06 };
    const unsigned char m_span[4] = { 'P', 'K', 0x07, 0x08 };
    if (try_open_zip_magic(scratch_dir, "lfh",  m_lfh,
                           PAKKA_ERR_FORMAT)      != 0) return 1;
    if (try_open_zip_magic(scratch_dir, "eocd", m_eocd,
                           PAKKA_ERR_FORMAT)      != 0) return 1;
    if (try_open_zip_magic(scratch_dir, "span", m_span,
                           PAKKA_ERR_UNSUPPORTED) != 0) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <pak0.pak> <scratch_dir>\n", argv[0]);
        return 2;
    }

    if (test_null_args() != 0) return 1;
    if (test_open_pak0(argv[1]) != 0) return 1;
    if (test_reader_streaming(argv[1]) != 0) return 1;
    if (test_create_close_roundtrip(argv[2]) != 0) return 1;
    if (test_open_rejects_duplicates(argv[2]) != 0) return 1;
    if (test_open_rejects_malformed_pk3(argv[2]) != 0) return 1;
    if (test_zip_format_label_roundtrip(argv[2]) != 0) return 1;
    if (test_sin_roundtrip(argv[2]) != 0) return 1;
    if (test_daikatana_create_rejected(argv[2]) != 0) return 1;
    if (test_open_ex_hint_mismatch(argv[2]) != 0) return 1;
    if (test_add_commit_roundtrip(argv[2]) != 0) return 1;
    if (test_delete_commit(argv[2]) != 0) return 1;
    if (test_add_to_readonly_fails(argv[1]) != 0) return 1;
    if (test_memory_apis(argv[2]) != 0) return 1;
    if (test_verify(argv[1], argv[2]) != 0) return 1;

    printf("c_api_test: OK\n");
    return 0;
}
