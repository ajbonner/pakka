/* dk_test — Daikatana pak format end-to-end tests.
 *
 * Same "PACK" magic as Quake, but directory entries are 72 bytes (extra
 * trailing u32 pair for compressed_size + is_compressed). Custom byte-
 * codec compression with five opcode classes — see src/dk_codec.c. */

#include "fs.h"
#include "proc.h"
#include "test_macros.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DK_HEADER_SIZE  12
#define DK_NAME_FIELD   56
#define DK_DIR_ENTRY    72 /* 56-byte name + offset + length + csize + is_compressed */

static const char *g_pakka_path;
static const char *g_repo_root; /* needed to find test/fixtures/dk/ */
static char       *g_scratch;

static char *under_scratch(const char *sub)
{
    return (char *)t_track(fs_join(g_scratch, sub));
}

static void put_u32_le(unsigned char *buf, size_t off, uint32_t v)
{
    buf[off + 0] = (unsigned char)(v & 0xFF);
    buf[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    buf[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    buf[off + 3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint32_t get_u32_le(const unsigned char *buf, size_t off)
{
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

/* Build a 72-byte DK directory row in `dir` for an entry with the
 * given name + offsets/sizes. Returns 0 on success, -1 if name is
 * too long for the 56-byte field. */
static int fill_dk_dir_row(unsigned char dir[DK_DIR_ENTRY], const char *name,
                           uint32_t offset, uint32_t length,
                           uint32_t csize, uint32_t is_compressed)
{
    size_t nlen = strlen(name);
    if (nlen > DK_NAME_FIELD) return -1;
    memset(dir, 0, DK_DIR_ENTRY);
    memcpy(dir, name, nlen);
    put_u32_le(dir, DK_NAME_FIELD + 0,  offset);
    put_u32_le(dir, DK_NAME_FIELD + 4,  length);
    put_u32_le(dir, DK_NAME_FIELD + 8,  csize);
    put_u32_le(dir, DK_NAME_FIELD + 12, is_compressed);
    return 0;
}

/* Write a single-entry DK pak with a STORED payload. */
static int write_dk_one_stored(const char *path, const char *name, const char *payload)
{
    size_t plen = strlen(payload);

    unsigned char header[DK_HEADER_SIZE];
    memcpy(header, "PACK", 4);
    put_u32_le(header, 4, (uint32_t)(DK_HEADER_SIZE + plen));
    put_u32_le(header, 8, DK_DIR_ENTRY);

    unsigned char dir[DK_DIR_ENTRY];
    if (fill_dk_dir_row(dir, name, DK_HEADER_SIZE, (uint32_t)plen, 0, 0) != 0) {
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        (plen > 0 && fwrite(payload, 1, plen, f) != plen) ||
        fwrite(dir, 1, sizeof(dir), f) != sizeof(dir)) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Write a single-entry DK pak whose payload is encoded with a literal-
 * run opcode followed by `payload_len` bytes and the 0xFF terminator.
 * Opcode (payload_len - 1) is the "literal run of payload_len bytes"
 * encoding (b in 0..63). */
static int write_dk_one_literal_compressed(const char *path, const char *name,
                                           const char *payload)
{
    size_t plen = strlen(payload);
    if (plen < 1 || plen > 64) return -1;
    size_t clen = 1 + plen + 1; /* opcode + bytes + 0xFF terminator */

    unsigned char header[DK_HEADER_SIZE];
    memcpy(header, "PACK", 4);
    put_u32_le(header, 4, (uint32_t)(DK_HEADER_SIZE + clen));
    put_u32_le(header, 8, DK_DIR_ENTRY);

    unsigned char dir[DK_DIR_ENTRY];
    if (fill_dk_dir_row(dir, name, DK_HEADER_SIZE, (uint32_t)plen,
                        (uint32_t)clen, 1) != 0) {
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return -1;
    }
    unsigned char opcode = (unsigned char)(plen - 1);
    if (fwrite(&opcode, 1, 1, f) != 1 ||
        fwrite(payload, 1, plen, f) != plen) {
        fclose(f);
        return -1;
    }
    unsigned char terminator = 0xFF;
    if (fwrite(&terminator, 1, 1, f) != 1 ||
        fwrite(dir, 1, sizeof(dir), f) != sizeof(dir)) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Pak with dirlength = 576 (divisible by both 64 and 72) — exercises
 * the PACK/DK ambiguity probe path. Every directory row is zero so
 * both layout probes fail offset validation. */
static int write_ambiguous_576(const char *path)
{
    unsigned char header[DK_HEADER_SIZE];
    memcpy(header, "PACK", 4);
    put_u32_le(header, 4, DK_HEADER_SIZE);
    put_u32_le(header, 8, 576);
    unsigned char zeros[576] = {0};

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        fwrite(zeros, 1, sizeof(zeros), f) != sizeof(zeros)) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Read directory entries from a DK pak. Returns -1 on malformed
 * header, otherwise the entry count (0 on empty archive). Caller
 * provides a fixed-size out array. */
typedef struct {
    char     name[DK_NAME_FIELD + 1];
    uint32_t offset;
    uint32_t length;
    uint32_t csize;
    uint32_t is_compressed;
} dk_entry_t;

static int probe_dk_dir(const char *path, dk_entry_t *out, size_t cap)
{
    size_t         n   = 0;
    unsigned char *buf = fs_read_file(path, &n);
    if (!buf) return -1;
    if (n < DK_HEADER_SIZE || memcmp(buf, "PACK", 4) != 0) {
        t_free(buf);
        return -1;
    }
    uint32_t diroffset = get_u32_le(buf, 4);
    uint32_t dirlength = get_u32_le(buf, 8);
    if (dirlength % DK_DIR_ENTRY != 0 || diroffset + dirlength > n) {
        t_free(buf);
        return -1;
    }
    size_t entries = dirlength / DK_DIR_ENTRY;
    if (entries > cap) entries = cap;

    for (size_t i = 0; i < entries; i++) {
        const unsigned char *row = buf + diroffset + i * DK_DIR_ENTRY;
        memset(out[i].name, 0, sizeof(out[i].name));
        memcpy(out[i].name, row, DK_NAME_FIELD);
        out[i].name[DK_NAME_FIELD] = '\0';
        out[i].offset         = get_u32_le(row, DK_NAME_FIELD + 0);
        out[i].length         = get_u32_le(row, DK_NAME_FIELD + 4);
        out[i].csize          = get_u32_le(row, DK_NAME_FIELD + 8);
        out[i].is_compressed  = get_u32_le(row, DK_NAME_FIELD + 12);
    }

    t_free(buf);
    return (int)entries;
}

/* End-of-payload offset = max(entry_offset + extent) across the
 * directory. For a well-formed DK pak this equals diroffset. */
static uint32_t probe_dk_payload_end(const char *path)
{
    dk_entry_t entries[64];
    int        n = probe_dk_dir(path, entries, 64);
    if (n < 0) return 0;
    uint32_t end = DK_HEADER_SIZE;
    for (int i = 0; i < n; i++) {
        uint32_t extent = entries[i].is_compressed ? entries[i].csize : entries[i].length;
        uint32_t e      = entries[i].offset + extent;
        if (e > end) end = e;
    }
    return end;
}

static dk_entry_t *find_entry_by_name(dk_entry_t *entries, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, name) == 0) return &entries[i];
    }
    return NULL;
}

#define RUN_PAKKA_OK(out_result, ...) do {                                  \
    const char *_argv[] = { g_pakka_path, __VA_ARGS__, NULL };              \
    if (proc_run(_argv, NULL, (out_result)) != 0)                           \
        FAIL("proc_run failed to launch pakka");                            \
    if ((out_result)->exit_code != 0) {                                     \
        fprintf(stderr, "    pakka exit=%d\n    stdout: %s\n    stderr: %s\n", \
                (out_result)->exit_code,                                    \
                (out_result)->stdout_buf ? (out_result)->stdout_buf : "",   \
                (out_result)->stderr_buf ? (out_result)->stderr_buf : "");  \
        FAIL("pakka exited non-zero");                                      \
    }                                                                       \
} while (0)

static int run_pakka_capture(proc_result_t *r, const char *const *argv)
{
    if (proc_run(argv, NULL, r) != 0) return -1;
    return 0;
}

static int copy_file(const char *src, const char *dst)
{
    size_t         n   = 0;
    unsigned char *buf = fs_read_file(src, &n);
    if (!buf) return -1;
    int rc = fs_write_file(dst, buf, n);
    t_free(buf);
    return rc;
}

/* Build an LCG-driven 256-byte high-entropy buffer for the
 * incompressible-fallback assertion: r = r*1103515245 + 12345, take byte at
 * (r >> 16) & 0xFF. Seed 0xC0FFEE. */
static void build_lcg_buffer(unsigned char buf[256])
{
    uint32_t r = 0xC0FFEEU;
    for (int i = 0; i < 256; i++) {
        r      = r * 1103515245U + 12345U;
        buf[i] = (unsigned char)((r >> 16) & 0xFFU);
    }
}

/* Fill `buf` with `len` bytes of constant `value`. */
static void fill_const(unsigned char *buf, size_t len, unsigned char value)
{
    memset(buf, value, len);
}

/* ---------- tests ---------- */

static void test_open_stored_only_lists(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("stored")), 0);
    char *pak = under_scratch("stored/stored.pak");
    EXPECT_EQ(write_dk_one_stored(pak, "weapons/blast.mdl", "model-bytes-here"), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", pak, "--format", "daikatana");
    EXPECT_STR_CONTAINS(r.stdout_buf, "weapons/blast.mdl");
    proc_result_free(&r);
    t_free(pak);
}

static void test_extract_stored_round_trip(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("xstored")), 0);
    char *pak     = under_scratch("xstored/stored.pak");
    char *out_dir = under_scratch("xstored/out");
    EXPECT_EQ(write_dk_one_stored(pak, "weapons/blast.mdl", "model-bytes-here"), 0);
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "--format", "daikatana", "-C", out_dir, pak);
    proc_result_free(&r);

    char *out_file = fs_join(out_dir, "weapons/blast.mdl");
    EXPECT_TRUE(fs_is_file(out_file));
    size_t         n   = 0;
    unsigned char *got = fs_read_file(out_file, &n);
    EXPECT_EQ((long long)n, 16);
    EXPECT_MEM_EQ(got, "model-bytes-here", 16);
    t_free(got);
    t_free(out_file);
    t_free(out_dir);
    t_free(pak);
}

static void test_extract_compressed_literal_decodes(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("xcmp")), 0);
    char *pak     = under_scratch("xcmp/cmp.pak");
    char *out_dir = under_scratch("xcmp/out");
    EXPECT_EQ(write_dk_one_literal_compressed(pak, "textures/wall.tga", "wall-pixels"), 0);
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "--format", "daikatana", "-C", out_dir, pak);
    proc_result_free(&r);

    char *out_file = fs_join(out_dir, "textures/wall.tga");
    EXPECT_TRUE(fs_is_file(out_file));
    size_t         n   = 0;
    unsigned char *got = fs_read_file(out_file, &n);
    EXPECT_EQ((long long)n, 11);
    EXPECT_MEM_EQ(got, "wall-pixels", 11);
    t_free(got);
    t_free(out_file);
    t_free(out_dir);
    t_free(pak);
}

static void test_create_empty_then_open_with_hint(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("empty")), 0);
    char *pak = under_scratch("empty/empty.pak");

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak, "--format", "daikatana");
    EXPECT_TRUE(fs_is_file(pak));
    proc_result_free(&r);

    /* AUTO-open of an empty PACK biases to Quake PAK; --format daikatana
     * verifies identity. */
    RUN_PAKKA_OK(&r, "-l", pak, "--format", "daikatana");
    proc_result_free(&r);
    t_free(pak);
}

static void test_add_non_compressible_extension_stays_stored(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("add_stored")), 0);
    char *pak = under_scratch("add_stored/add_stored.pak");
    char *src = under_scratch("add_stored/test.txt");
    EXPECT_EQ(fs_write_file(src, "hello world", 11), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak, "--format", "daikatana");
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pak, "--format", "daikatana", "--as", "test.txt", src);
    proc_result_free(&r);

    dk_entry_t entries[8];
    int        n = probe_dk_dir(pak, entries, 8);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(entries[0].name, "test.txt");
    EXPECT_EQ((long long)entries[0].length, 11);
    EXPECT_EQ((long long)entries[0].csize, 0);
    EXPECT_EQ((long long)entries[0].is_compressed, 0);

    char *out_dir = under_scratch("add_stored/out");
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);
    RUN_PAKKA_OK(&r, "-x", "--format", "daikatana", "-C", out_dir, pak);
    proc_result_free(&r);
    char  *extracted = fs_join(out_dir, "test.txt");
    size_t got_n     = 0;
    unsigned char *got = fs_read_file(extracted, &got_n);
    EXPECT_EQ((long long)got_n, 11);
    EXPECT_MEM_EQ(got, "hello world", 11);
    t_free(got);
    t_free(extracted);
    t_free(out_dir);
    t_free(pak);
    t_free(src);
}

static void test_add_compressible_redundant_payload_encoded(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("add_cmp")), 0);
    char *pak = under_scratch("add_cmp/add_compress.pak");
    char *src = under_scratch("add_cmp/zeros.bmp");

    unsigned char zeros[4096];
    fill_const(zeros, sizeof(zeros), 0);
    EXPECT_EQ(fs_write_file(src, zeros, sizeof(zeros)), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak, "--format", "daikatana");
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pak, "--format", "daikatana", "--as", "zeros.bmp", src);
    proc_result_free(&r);

    dk_entry_t entries[8];
    int        n = probe_dk_dir(pak, entries, 8);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(entries[0].name, "zeros.bmp");
    EXPECT_EQ((long long)entries[0].length, 4096);
    EXPECT_EQ((long long)entries[0].is_compressed, 1);
    EXPECT_TRUE(entries[0].csize > 0);
    EXPECT_TRUE(entries[0].csize < 4096);

    char *out_dir = under_scratch("add_cmp/out");
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);
    RUN_PAKKA_OK(&r, "-x", "--format", "daikatana", "-C", out_dir, pak);
    proc_result_free(&r);
    char *extracted = fs_join(out_dir, "zeros.bmp");
    size_t got_n    = 0;
    unsigned char *got = fs_read_file(extracted, &got_n);
    EXPECT_EQ((long long)got_n, 4096);
    EXPECT_MEM_EQ(got, zeros, sizeof(zeros));
    t_free(got);
    t_free(extracted);
    t_free(out_dir);
    t_free(pak);
    t_free(src);
}

static void test_add_incompressible_falls_back_to_stored(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("add_fb")), 0);
    char *pak = under_scratch("add_fb/add_fallback.pak");
    char *src = under_scratch("add_fb/random.bmp");

    /* LCG output is high-entropy: byte-RLE / back-ref opcodes don't
     * reach the min-3 threshold, so literal-only encoding (~261 bytes)
     * overshoots the 256-byte source and the encoder's STORED auto-
     * fallback fires. */
    unsigned char buf[256];
    build_lcg_buffer(buf);
    EXPECT_EQ(fs_write_file(src, buf, sizeof(buf)), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak, "--format", "daikatana");
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pak, "--format", "daikatana", "--as", "random.bmp", src);
    proc_result_free(&r);

    dk_entry_t entries[8];
    int        n = probe_dk_dir(pak, entries, 8);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(entries[0].name, "random.bmp");
    EXPECT_EQ((long long)entries[0].length, 256);
    EXPECT_EQ((long long)entries[0].is_compressed, 0);

    char *out_dir = under_scratch("add_fb/out");
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);
    RUN_PAKKA_OK(&r, "-x", "--format", "daikatana", "-C", out_dir, pak);
    proc_result_free(&r);
    char *extracted = fs_join(out_dir, "random.bmp");
    size_t got_n    = 0;
    unsigned char *got = fs_read_file(extracted, &got_n);
    EXPECT_EQ((long long)got_n, 256);
    EXPECT_MEM_EQ(got, buf, sizeof(buf));
    t_free(got);
    t_free(extracted);
    t_free(out_dir);
    t_free(pak);
    t_free(src);
}

static void test_mixed_extensions_layout_consistent(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("mixed")), 0);
    char *pak = under_scratch("mixed/mixed.pak");
    char *txt = under_scratch("mixed/notes.txt");
    char *bmp = under_scratch("mixed/sky.bmp");

    EXPECT_EQ(fs_write_file(txt, "plain text\n", 11), 0);
    unsigned char bmp_buf[2048];
    fill_const(bmp_buf, sizeof(bmp_buf), 0xAA);
    EXPECT_EQ(fs_write_file(bmp, bmp_buf, sizeof(bmp_buf)), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak, "--format", "daikatana");
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pak, "--format", "daikatana", "--as", "notes.txt", txt);
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pak, "--format", "daikatana", "--as", "sky.bmp", bmp);
    proc_result_free(&r);

    dk_entry_t entries[8];
    int        n = probe_dk_dir(pak, entries, 8);
    EXPECT_EQ(n, 2);
    dk_entry_t *notes = find_entry_by_name(entries, n, "notes.txt");
    dk_entry_t *sky   = find_entry_by_name(entries, n, "sky.bmp");
    EXPECT_NOT_NULL(notes);
    EXPECT_NOT_NULL(sky);
    EXPECT_EQ((long long)notes->length, 11);
    EXPECT_EQ((long long)notes->is_compressed, 0);
    EXPECT_EQ((long long)sky->length, 2048);
    EXPECT_EQ((long long)sky->is_compressed, 1);

    /* diroffset must equal the highest payload-end across all entries. */
    size_t         m   = 0;
    unsigned char *buf = fs_read_file(pak, &m);
    EXPECT_NOT_NULL(buf);
    uint32_t diroffset = get_u32_le(buf, 4);
    t_free(buf);
    uint32_t end = probe_dk_payload_end(pak);
    EXPECT_EQ((long long)diroffset, (long long)end);

    t_free(pak);
    t_free(txt);
    t_free(bmp);
}

static void test_delete_keeps_survivor_bit_identical(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("delsurv")), 0);
    char *pak = under_scratch("delsurv/delsurv.pak");
    char *a   = under_scratch("delsurv/keep.bmp");
    char *b   = under_scratch("delsurv/dropme.txt");

    /* keep.bmp gets compressed, dropme.txt stays STORED. */
    unsigned char keep_buf[1024];
    memset(keep_buf, 0x33, 512);
    memset(keep_buf + 512, 0x55, 512);
    EXPECT_EQ(fs_write_file(a, keep_buf, sizeof(keep_buf)), 0);
    EXPECT_EQ(fs_write_file(b, "this entry will be deleted\n", 27), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak, "--format", "daikatana");
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pak, "--format", "daikatana", "--as", "keep.bmp", a);
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pak, "--format", "daikatana", "--as", "dropme.txt", b);
    proc_result_free(&r);

    char *pre = under_scratch("delsurv/pre");
    EXPECT_EQ(fs_mkdir_p(pre), 0);
    RUN_PAKKA_OK(&r, "-x", "--format", "daikatana", "-C", pre, pak);
    proc_result_free(&r);
    char  *pre_keep = fs_join(pre, "keep.bmp");
    size_t got_n    = 0;
    unsigned char *got = fs_read_file(pre_keep, &got_n);
    EXPECT_EQ((long long)got_n, 1024);
    EXPECT_MEM_EQ(got, keep_buf, sizeof(keep_buf));
    t_free(got);
    t_free(pre_keep);

    /* Delete drops dropme.txt and forces a rebuild. */
    RUN_PAKKA_OK(&r, "-d", pak, "--format", "daikatana", "dropme.txt");
    proc_result_free(&r);

    char *post = under_scratch("delsurv/post");
    EXPECT_EQ(fs_mkdir_p(post), 0);
    RUN_PAKKA_OK(&r, "-x", "--format", "daikatana", "-C", post, pak);
    proc_result_free(&r);
    char *post_keep = fs_join(post, "keep.bmp");
    got = fs_read_file(post_keep, &got_n);
    EXPECT_EQ((long long)got_n, 1024);
    EXPECT_MEM_EQ(got, keep_buf, sizeof(keep_buf));
    t_free(got);

    char *post_drop = fs_join(post, "dropme.txt");
    EXPECT_FALSE(fs_is_file(post_drop));
    t_free(post_drop);

    dk_entry_t entries[8];
    int        n = probe_dk_dir(pak, entries, 8);
    EXPECT_EQ(n, 1);

    t_free(post_keep);
    t_free(pre);
    t_free(post);
    t_free(pak);
    t_free(a);
    t_free(b);
}

static void test_deep_verify_accepts_well_formed_compressed(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("vgood")), 0);
    char *pak = under_scratch("vgood/cmp.pak");
    EXPECT_EQ(write_dk_one_literal_compressed(pak, "textures/wall.tga", "good"), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "--verify", "--deep", pak, "--format", "daikatana");
    proc_result_free(&r);
    t_free(pak);
}

static void test_pakka_built_passes_deep_verify(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("vbuilt")), 0);
    char *pak = under_scratch("vbuilt/builtbypakka.pak");
    char *src = under_scratch("vbuilt/big.bmp");

    unsigned char buf[2048];
    memset(buf, 0x11, 1024);
    memset(buf + 1024, 0x22, 1024);
    EXPECT_EQ(fs_write_file(src, buf, sizeof(buf)), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak, "--format", "daikatana");
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pak, "--format", "daikatana", "--as", "big.bmp", src);
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "--verify", "--deep", pak, "--format", "daikatana");
    proc_result_free(&r);
    t_free(pak);
    t_free(src);
}

/* Build a malformed DK pak: 5-byte literal opcode (carries 5 bytes
 * "abcde") with declared file_length=0 — the codec must reject. */
static int write_dk_malformed_flen0(const char *path)
{
    unsigned char header[DK_HEADER_SIZE];
    memcpy(header, "PACK", 4);
    put_u32_le(header, 4, DK_HEADER_SIZE + 6);
    put_u32_le(header, 8, DK_DIR_ENTRY);

    unsigned char dir[DK_DIR_ENTRY];
    if (fill_dk_dir_row(dir, "cheat.tga", DK_HEADER_SIZE, 0, 6, 1) != 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    /* Layout: header + (opcode 0x04 + "abcde" + 0xFF) + dir row. */
    unsigned char payload[7] = {0x04, 'a', 'b', 'c', 'd', 'e', 0xFF};
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        fwrite(payload, 1, sizeof(payload), f) != sizeof(payload) ||
        fwrite(dir, 1, sizeof(dir), f) != sizeof(dir)) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static void test_deep_verify_rejects_flen0_with_stream(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("vflen0")), 0);
    char *pak = under_scratch("vflen0/dk_flen0.pak");
    EXPECT_EQ(write_dk_malformed_flen0(pak), 0);

    const char   *argv[] = {g_pakka_path, "--verify", "--deep", pak, "--format", "daikatana", NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

static void test_deep_verify_rejects_csize0_with_declared_length(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("vcsize0")), 0);
    char *pak = under_scratch("vcsize0/dk_csize0.pak");

    /* Claim file_length=5 but compressed_size=0 — zero input cannot
     * produce 5 output. */
    unsigned char header[DK_HEADER_SIZE];
    memcpy(header, "PACK", 4);
    put_u32_le(header, 4, DK_HEADER_SIZE);
    put_u32_le(header, 8, DK_DIR_ENTRY);
    unsigned char dir[DK_DIR_ENTRY];
    EXPECT_EQ(fill_dk_dir_row(dir, "cheat.tga", DK_HEADER_SIZE, 5, 0, 1), 0);

    FILE *f = fopen(pak, "wb");
    EXPECT_NOT_NULL(f);
    fwrite(header, 1, sizeof(header), f);
    fwrite(dir, 1, sizeof(dir), f);
    fclose(f);

    const char   *argv[] = {g_pakka_path, "--verify", "--deep", pak, "--format", "daikatana", NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

static void test_read_entry_alloc_rejects_flen0_stream(void)
{
    /* Same malformed shape as the verify case; extract path drives
     * read_entry_alloc directly. */
    EXPECT_EQ(fs_mkdir_p(under_scratch("alloc_flen0")), 0);
    char *pak     = under_scratch("alloc_flen0/dk_alloc_flen0.pak");
    char *out_dir = under_scratch("alloc_flen0/out");
    EXPECT_EQ(write_dk_malformed_flen0(pak), 0);
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);

    const char   *argv[] = {g_pakka_path, "-x", "--format", "daikatana", "-C", out_dir, pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
    t_free(out_dir);
}

static void test_deep_verify_rejects_truncated_stream(void)
{
    /* Opcode 0x04 = literal run of 5 bytes, but only 3 bytes follow
     * before the 0xFF terminator. */
    EXPECT_EQ(fs_mkdir_p(under_scratch("vtrunc")), 0);
    char *pak = under_scratch("vtrunc/bad.pak");

    unsigned char header[DK_HEADER_SIZE];
    memcpy(header, "PACK", 4);
    put_u32_le(header, 4, DK_HEADER_SIZE + 5);
    put_u32_le(header, 8, DK_DIR_ENTRY);
    unsigned char dir[DK_DIR_ENTRY];
    EXPECT_EQ(fill_dk_dir_row(dir, "broken", DK_HEADER_SIZE, 5, 5, 1), 0);

    FILE *f = fopen(pak, "wb");
    EXPECT_NOT_NULL(f);
    fwrite(header, 1, sizeof(header), f);
    unsigned char payload[5] = {0x04, 'a', 'b', 'c', 0xFF};
    fwrite(payload, 1, sizeof(payload), f);
    fwrite(dir, 1, sizeof(dir), f);
    fclose(f);

    const char   *argv[] = {g_pakka_path, "--verify", "--deep", pak, "--format", "daikatana", NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

static void test_ambiguous_576_rejected(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("amb")), 0);
    char *pak = under_scratch("amb/amb.pak");
    EXPECT_EQ(write_ambiguous_576(pak), 0);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

static void test_hint_daikatana_parses_as_dk(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("hint")), 0);
    char *pak = under_scratch("hint/dk_hint.pak");
    EXPECT_EQ(write_dk_one_stored(pak, "test", "abc"), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", pak, "--format", "daikatana");
    EXPECT_STR_CONTAINS(r.stdout_buf, "test");
    proc_result_free(&r);
    t_free(pak);
}

static void test_real_fixture_cross_validates(void)
{
    /* External-packer fixture under test/fixtures/dk/. Cross-validates
     * pakka's decoder against another implementation's encoded stream.
     * Hard fail (not skip) when the committed pak is missing so CI
     * cannot lose this coverage to an accidental git-rm. */
    char *fixture = fs_join(g_repo_root, "test/fixtures/dk/user.pak");
    char *inputs  = fs_join(g_repo_root, "test/fixtures/dk/inputs");
    if (!fs_is_file(fixture)) {
        fprintf(stderr, "FATAL: expected DK fixture missing at %s\n", fixture);
        t_free(fixture);
        t_free(inputs);
        FAIL("DK real fixture missing");
    }

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", fixture, "--format", "daikatana");
    EXPECT_STR_CONTAINS(r.stdout_buf, "textures/zeros.bmp");
    EXPECT_STR_CONTAINS(r.stdout_buf, "textures/striped.tga");
    EXPECT_STR_CONTAINS(r.stdout_buf, "maps/random.bsp");
    EXPECT_STR_CONTAINS(r.stdout_buf, "notes.txt");
    proc_result_free(&r);

    /* Directory inspection: compressed-extension entries with redundant
     * content must be encoded; the random-bytes .bsp falls back to
     * STORED via the encoder's "encoded >= source" gate; the .txt
     * entry is STORED via the extension policy. */
    dk_entry_t entries[16];
    int        n = probe_dk_dir(fixture, entries, 16);
    EXPECT_TRUE(n >= 4);
    dk_entry_t *zeros   = find_entry_by_name(entries, n, "textures/zeros.bmp");
    dk_entry_t *striped = find_entry_by_name(entries, n, "textures/striped.tga");
    dk_entry_t *random_ = find_entry_by_name(entries, n, "maps/random.bsp");
    dk_entry_t *notes   = find_entry_by_name(entries, n, "notes.txt");
    EXPECT_NOT_NULL(zeros);
    EXPECT_NOT_NULL(striped);
    EXPECT_NOT_NULL(random_);
    EXPECT_NOT_NULL(notes);
    EXPECT_EQ((long long)zeros->length, 1024);
    EXPECT_EQ((long long)zeros->is_compressed, 1);
    EXPECT_EQ((long long)striped->length, 512);
    EXPECT_EQ((long long)striped->is_compressed, 1);
    EXPECT_EQ((long long)random_->length, 256);
    EXPECT_EQ((long long)random_->is_compressed, 0);
    EXPECT_EQ((long long)notes->length, 33);
    EXPECT_EQ((long long)notes->is_compressed, 0);

    /* Extract every entry and byte-compare against the source inputs. */
    char *realout = under_scratch("realfixture/realout");
    EXPECT_EQ(fs_mkdir_p(realout), 0);
    RUN_PAKKA_OK(&r, "-x", "--format", "daikatana", "-C", realout, fixture);
    proc_result_free(&r);

    struct {
        const char *input_rel;
        const char *output_rel;
    } pairs[] = {
        {"zeros.bmp",   "textures/zeros.bmp"},
        {"striped.tga", "textures/striped.tga"},
        {"random.bsp",  "maps/random.bsp"},
        {"notes.txt",   "notes.txt"},
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        char  *want_path = fs_join(inputs, pairs[i].input_rel);
        char  *got_path  = fs_join(realout, pairs[i].output_rel);
        size_t wn        = 0;
        size_t gn        = 0;
        unsigned char *want = fs_read_file(want_path, &wn);
        unsigned char *got  = fs_read_file(got_path, &gn);
        if (!want || !got || wn != gn || memcmp(want, got, wn) != 0) {
            fprintf(stderr, "    mismatch on %s vs %s (wn=%zu gn=%zu)\n",
                    want_path, got_path, wn, gn);
            t_free(want);
            t_free(got);
            t_free(want_path);
            t_free(got_path);
            t_free(realout);
            t_free(fixture);
            t_free(inputs);
            FAIL("decoded fixture entry differs from source input");
        }
        t_free(want);
        t_free(got);
        t_free(want_path);
        t_free(got_path);
    }

    RUN_PAKKA_OK(&r, "--verify", "--deep", fixture, "--format", "daikatana");
    proc_result_free(&r);

    t_free(realout);
    t_free(fixture);
    t_free(inputs);
}

int main(void)
{
    const char *pakka = getenv("PAKKA");
    if (!pakka || !*pakka) {
        fprintf(stderr, "dk_test: PAKKA env var not set\n");
        return 1;
    }
    g_pakka_path = pakka;

    const char *root = getenv("REPO_ROOT");
    if (!root || !*root) root = ".";
    g_repo_root = root;

    const char *scratch = getenv("DK_TEST_SCRATCH");
    if (!scratch || !*scratch) scratch = "build/test/dk";
    fs_rmtree(scratch);
    if (fs_mkdir_p(scratch) != 0) {
        fprintf(stderr, "dk_test: mkdir(%s) failed\n", scratch);
        return 1;
    }
    g_scratch = strdup(scratch);

    RUN_TEST(test_open_stored_only_lists);
    RUN_TEST(test_extract_stored_round_trip);
    RUN_TEST(test_extract_compressed_literal_decodes);
    RUN_TEST(test_create_empty_then_open_with_hint);
    RUN_TEST(test_add_non_compressible_extension_stays_stored);
    RUN_TEST(test_add_compressible_redundant_payload_encoded);
    RUN_TEST(test_add_incompressible_falls_back_to_stored);
    RUN_TEST(test_mixed_extensions_layout_consistent);
    RUN_TEST(test_delete_keeps_survivor_bit_identical);
    RUN_TEST(test_deep_verify_accepts_well_formed_compressed);
    RUN_TEST(test_pakka_built_passes_deep_verify);
    RUN_TEST(test_deep_verify_rejects_flen0_with_stream);
    RUN_TEST(test_deep_verify_rejects_csize0_with_declared_length);
    RUN_TEST(test_read_entry_alloc_rejects_flen0_stream);
    RUN_TEST(test_deep_verify_rejects_truncated_stream);
    RUN_TEST(test_ambiguous_576_rejected);
    RUN_TEST(test_hint_daikatana_parses_as_dk);
    RUN_TEST(test_real_fixture_cross_validates);

    t_free(g_scratch);
    /* copy_file is unused in dk_test today (kept for parity with other
     * ports) — silence -Wunused-function. */
    (void)copy_file;
    return t_summary();
}
