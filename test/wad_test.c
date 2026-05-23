/* wad_test — Doom WAD format (IWAD + PWAD). C peer of test/wad.bats.
 *
 * 12-byte header (magic + numlumps u32 LE + infotableofs u32 LE)
 * followed by lump payloads, with the directory at infotableofs
 * holding numlumps × 16-byte entries (filepos u32 LE + size u32 LE
 * + 8-byte NUL-padded name). IWAD and PWAD share the layout — the
 * magic only distinguishes id-shipped base archives (IWAD) from modder
 * patch archives (PWAD). pakka treats them as label variants of one
 * impl, mirroring PK3/PK4. */

#include "fs.h"
#include "proc.h"
#include "test_macros.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAD_HEADER_SIZE 12
#define WAD_DIR_ENTRY   16
#define WAD_NAME_FIELD   8

static const char *g_pakka_path;
static char       *g_scratch;

static char *under_scratch(const char *sub)
{
    return fs_join(g_scratch, sub);
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

typedef struct {
    const char *name;
    const char *payload; /* NULL = marker (filepos=0, size=0) */
    size_t      payload_len;
} wad_entry_t;

/* Build a WAD with `entries` lumps. Marker convention: NULL payload OR
 * payload_len == 0 records filepos=0, size=0 (matches the bats
 * write_wad_synth pattern). */
static int write_wad_synth(const char         *path,
                           const char         *magic, /* "IWAD" or "PWAD" */
                           const wad_entry_t  *entries,
                           size_t              n)
{
    /* First pass: compute layout. */
    uint32_t offset       = WAD_HEADER_SIZE;
    size_t   payload_total = 0;
    for (size_t i = 0; i < n; i++) {
        if (entries[i].payload && entries[i].payload_len > 0) {
            payload_total += entries[i].payload_len;
        }
    }
    uint32_t infotableofs = WAD_HEADER_SIZE + (uint32_t)payload_total;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    unsigned char header[WAD_HEADER_SIZE];
    memcpy(header, magic, 4);
    put_u32_le(header, 4, (uint32_t)n);
    put_u32_le(header, 8, infotableofs);
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return -1;
    }

    /* Payloads, in order, immediately after the header. */
    for (size_t i = 0; i < n; i++) {
        if (entries[i].payload && entries[i].payload_len > 0) {
            if (fwrite(entries[i].payload, 1, entries[i].payload_len, f) !=
                entries[i].payload_len) {
                fclose(f);
                return -1;
            }
        }
    }

    /* Directory entries. Compute filepos as we go so marker lumps
     * (payload NULL or length 0) record filepos=0 / size=0. */
    offset = WAD_HEADER_SIZE;
    for (size_t i = 0; i < n; i++) {
        unsigned char row[WAD_DIR_ENTRY] = {0};
        uint32_t      filepos = 0;
        uint32_t      size    = 0;
        if (entries[i].payload && entries[i].payload_len > 0) {
            filepos = offset;
            size    = (uint32_t)entries[i].payload_len;
            offset += size;
        }
        put_u32_le(row, 0, filepos);
        put_u32_le(row, 4, size);
        size_t nlen = strlen(entries[i].name);
        if (nlen > WAD_NAME_FIELD) {
            fclose(f);
            return -1;
        }
        memcpy(row + 8, entries[i].name, nlen);
        if (fwrite(row, 1, sizeof(row), f) != sizeof(row)) {
            fclose(f);
            return -1;
        }
    }

    return fclose(f) == 0 ? 0 : -1;
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

#define RUN_PAKKA_OK_CWD(out_result, opts, ...) do {                        \
    const char *_argv[] = { g_pakka_path, __VA_ARGS__, NULL };              \
    if (proc_run(_argv, (opts), (out_result)) != 0)                         \
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

/* Return file size in bytes, or -1 on error. */
static long file_size(const char *path)
{
    size_t         n   = 0;
    unsigned char *buf = fs_read_file(path, &n);
    if (!buf) return -1;
    free(buf);
    return (long)n;
}

/* ---------- tests ---------- */

static void test_empty_iwad_has_12_byte_header(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("empty")), 0);
    char *pak = under_scratch("empty/empty.wad");

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", "--format", "iwad", pak);
    proc_result_free(&r);
    EXPECT_EQ(file_size(pak), 12);

    size_t         n   = 0;
    unsigned char *buf = fs_read_file(pak, &n);
    EXPECT_EQ((long long)n, 12);
    EXPECT_MEM_EQ(buf, "IWAD", 4);
    EXPECT_EQ((long long)get_u32_le(buf, 4), 0);
    EXPECT_EQ((long long)get_u32_le(buf, 8), 12);
    free(buf);
    free(pak);
}

static void test_wad_extension_defaults_to_pwad(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("pwad_default")), 0);
    char *pak = under_scratch("pwad_default/default.wad");

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak);
    proc_result_free(&r);

    size_t         n   = 0;
    unsigned char *buf = fs_read_file(pak, &n);
    EXPECT_EQ((long long)n, 12);
    EXPECT_MEM_EQ(buf, "PWAD", 4);
    EXPECT_EQ((long long)get_u32_le(buf, 4), 0);
    free(buf);
    free(pak);
}

static void test_two_entry_iwad_lists_in_dir_order(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("two")), 0);
    char *pak = under_scratch("two/two.wad");

    wad_entry_t entries[] = {
        {"PLAYPAL",  "palette-bytes", strlen("palette-bytes")},
        {"COLORMAP", "cmap-bytes",    strlen("cmap-bytes")},
    };
    EXPECT_EQ(write_wad_synth(pak, "IWAD", entries, 2), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", pak);
    EXPECT_NOT_NULL(r.stdout_buf);
    char *pos_pal = strstr(r.stdout_buf, "PLAYPAL");
    char *pos_cmp = strstr(r.stdout_buf, "COLORMAP");
    EXPECT_NOT_NULL(pos_pal);
    EXPECT_NOT_NULL(pos_cmp);
    EXPECT_TRUE(pos_pal < pos_cmp);
    proc_result_free(&r);
    free(pak);
}

static void test_extract_single_entry_byte_identical(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("extract")), 0);
    char *pak     = under_scratch("extract/extract.wad");
    char *out_dir = under_scratch("extract/out");

    wad_entry_t entries[] = {{"PLAYPAL", "palette-bytes", strlen("palette-bytes")}};
    EXPECT_EQ(write_wad_synth(pak, "IWAD", entries, 1), 0);
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out_dir, pak);
    proc_result_free(&r);

    char *extracted = fs_join(out_dir, "PLAYPAL");
    EXPECT_TRUE(fs_is_file(extracted));
    size_t         n   = 0;
    unsigned char *got = fs_read_file(extracted, &n);
    EXPECT_EQ((long long)n, (long long)strlen("palette-bytes"));
    EXPECT_MEM_EQ(got, "palette-bytes", n);
    free(got);
    free(extracted);
    free(out_dir);
    free(pak);
}

static void test_eight_char_lump_name_accepted(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("eight")), 0);
    char *pak = under_scratch("eight/eight.wad");
    char *src = under_scratch("eight/payload.bin");
    EXPECT_EQ(fs_write_file(src, "lines-data", 10), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", "--format", "pwad", pak);
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pak, "--as", "LINEDEFS", src);
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-l", pak);
    EXPECT_STR_CONTAINS(r.stdout_buf, "LINEDEFS");
    proc_result_free(&r);

    free(pak);
    free(src);
}

static void test_directory_row_order_filepos_size_name(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("order")), 0);
    char *pak = under_scratch("order/order.wad");

    wad_entry_t entries[] = {{"PLAYPAL", "palette-bytes", strlen("palette-bytes")}};
    EXPECT_EQ(write_wad_synth(pak, "IWAD", entries, 1), 0);

    size_t         n   = 0;
    unsigned char *buf = fs_read_file(pak, &n);
    EXPECT_NOT_NULL(buf);
    /* Header: 1 lump, infotableofs = 12 + 13 = 25. */
    EXPECT_EQ((long long)get_u32_le(buf, 4), 1);
    EXPECT_EQ((long long)get_u32_le(buf, 8), 25);
    /* First row at offset 25: filepos=12, size=13, name="PLAYPAL". */
    EXPECT_EQ((long long)get_u32_le(buf, 25), 12);
    EXPECT_EQ((long long)get_u32_le(buf, 29), 13);
    EXPECT_MEM_EQ(buf + 33, "PLAYPAL\0", 8);

    free(buf);
    free(pak);
}

static void test_create_and_add_round_trips_row_order(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("rt_order")), 0);
    char *pak = under_scratch("rt_order/order_rw.wad");
    char *src = under_scratch("rt_order/p.bin");
    EXPECT_EQ(fs_write_file(src, "palette-bytes", 13), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", "--format", "iwad", pak);
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pak, "--as", "PLAYPAL", src);
    proc_result_free(&r);

    size_t         n   = 0;
    unsigned char *buf = fs_read_file(pak, &n);
    EXPECT_NOT_NULL(buf);
    EXPECT_MEM_EQ(buf, "IWAD", 4);
    EXPECT_EQ((long long)get_u32_le(buf, 4), 1);
    EXPECT_EQ((long long)get_u32_le(buf, 8), 25);
    EXPECT_EQ((long long)get_u32_le(buf, 25), 12);
    EXPECT_EQ((long long)get_u32_le(buf, 29), 13);
    EXPECT_MEM_EQ(buf + 33, "PLAYPAL\0", 8);

    free(buf);
    free(pak);
    free(src);
}

static void test_marker_lump_accepted(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("marker")), 0);
    char *pak = under_scratch("marker/marker.wad");

    wad_entry_t entries[] = {{"F_START", NULL, 0}};
    EXPECT_EQ(write_wad_synth(pak, "IWAD", entries, 1), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", pak);
    EXPECT_STR_CONTAINS(r.stdout_buf, "F_START");
    proc_result_free(&r);
    free(pak);
}

static void test_duplicate_lump_names_load(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("dup_load")), 0);
    char *pak = under_scratch("dup_load/dup.wad");

    wad_entry_t entries[] = {
        {"THINGS", "map01-things", strlen("map01-things")},
        {"THINGS", "map02-things", strlen("map02-things")},
    };
    EXPECT_EQ(write_wad_synth(pak, "IWAD", entries, 2), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", pak);
    EXPECT_NOT_NULL(r.stdout_buf);
    /* Both entries should be listed. */
    size_t hits = 0;
    for (size_t i = 0; i < r.line_count; i++) {
        if (strncmp(r.lines[i], "THINGS", 6) == 0) hits++;
    }
    EXPECT_EQ((long long)hits, 2);
    proc_result_free(&r);
    free(pak);
}

static void test_dup_name_pwad_ok_pak_rejects(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("dup_add")), 0);
    char *pwad = under_scratch("dup_add/dup_add.wad");
    char *pak  = under_scratch("dup_add/dup_add.pak");
    char *src  = under_scratch("dup_add/dup_src.bin");
    EXPECT_EQ(fs_write_file(src, "dup-payload", 11), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", "--format", "pwad", pwad);
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pwad, "--as", "THINGS", src);
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pwad, "--as", "THINGS", src);
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "-c", "--format", "pak", pak);
    proc_result_free(&r);
    RUN_PAKKA_OK(&r, "-a", pak, "--as", "THINGS", src);
    proc_result_free(&r);
    const char *argv[] = {g_pakka_path, "-a", pak, "--as", "THINGS", src, NULL};
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    if (r.stdout_buf &&
        (strstr(r.stdout_buf, "Duplicate") || strstr(r.stdout_buf, "duplicate"))) {
        found = 1;
    }
    if (r.stderr_buf &&
        (strstr(r.stderr_buf, "Duplicate") || strstr(r.stderr_buf, "duplicate"))) {
        found = 1;
    }
    if (!found) {
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        FAIL("expected 'Duplicate' diagnostic for PAK dup-name add");
    }
    proc_result_free(&r);

    free(pwad);
    free(pak);
    free(src);
}

static void test_extract_all_refuses_dup_collision(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("dup_ex")), 0);
    char *pak     = under_scratch("dup_ex/dup_extract.wad");
    char *out_dir = under_scratch("dup_ex/out_dup");

    wad_entry_t entries[] = {
        {"THINGS", "first",  5},
        {"THINGS", "second", 6},
    };
    EXPECT_EQ(write_wad_synth(pak, "PWAD", entries, 2), 0);
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);

    const char *argv[] = {g_pakka_path, "-x", "-C", out_dir, pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    /* "collide" or "collision" must appear in either stream. */
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] &&
            (strstr(streams[s], "collide") || strstr(streams[s], "collision") ||
             strstr(streams[s], "Collide") || strstr(streams[s], "Collision"))) {
            found = 1;
        }
    }
    if (!found) {
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        FAIL("expected 'collide/collision' diagnostic on duplicate extract");
    }
    proc_result_free(&r);

    free(pak);
    free(out_dir);
}

static void test_extract_by_name_first_match(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("dup_byname")), 0);
    char *pak     = under_scratch("dup_byname/dup_byname.wad");
    char *out_dir = under_scratch("dup_byname/out_byname");

    wad_entry_t entries[] = {
        {"THINGS", "first",  5},
        {"THINGS", "second", 6},
    };
    EXPECT_EQ(write_wad_synth(pak, "PWAD", entries, 2), 0);
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out_dir, pak, "THINGS");
    proc_result_free(&r);

    char *extracted = fs_join(out_dir, "THINGS");
    EXPECT_TRUE(fs_is_file(extracted));
    size_t         n   = 0;
    unsigned char *got = fs_read_file(extracted, &n);
    EXPECT_EQ((long long)n, 5);
    EXPECT_MEM_EQ(got, "first", 5);
    free(got);
    free(extracted);
    free(out_dir);
    free(pak);
}

static void test_delete_removes_first_match(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("dup_del")), 0);
    char *pak = under_scratch("dup_del/dup_delete.wad");

    wad_entry_t entries[] = {
        {"THINGS", "first",  5},
        {"THINGS", "second", 6},
    };
    EXPECT_EQ(write_wad_synth(pak, "PWAD", entries, 2), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-d", pak, "THINGS");
    proc_result_free(&r);

    size_t         n   = 0;
    unsigned char *buf = fs_read_file(pak, &n);
    EXPECT_NOT_NULL(buf);
    EXPECT_MEM_EQ(buf, "PWAD", 4);
    EXPECT_EQ((long long)get_u32_le(buf, 4), 1);
    free(buf);
    free(pak);
}

static void test_rebuild_middle_delete_preserves_survivors(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("three")), 0);
    char *pak     = under_scratch("three/three.wad");
    char *out_dir = under_scratch("three/out_three");

    wad_entry_t entries[] = {
        {"AAA", "alpha",   5},
        {"BBB", "bravo",   5},
        {"CCC", "charlie", 7},
    };
    EXPECT_EQ(write_wad_synth(pak, "PWAD", entries, 3), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-d", pak, "BBB");
    proc_result_free(&r);

    /* After delete: PWAD, 2 lumps, infotableofs = 12 + 5 + 7 = 24. */
    size_t         n   = 0;
    unsigned char *buf = fs_read_file(pak, &n);
    EXPECT_NOT_NULL(buf);
    EXPECT_MEM_EQ(buf, "PWAD", 4);
    EXPECT_EQ((long long)get_u32_le(buf, 4), 2);
    EXPECT_EQ((long long)get_u32_le(buf, 8), 24);
    free(buf);

    EXPECT_EQ(fs_mkdir_p(out_dir), 0);
    RUN_PAKKA_OK(&r, "-x", "-C", out_dir, pak);
    proc_result_free(&r);

    char *aaa = fs_join(out_dir, "AAA");
    char *bbb = fs_join(out_dir, "BBB");
    char *ccc = fs_join(out_dir, "CCC");
    EXPECT_TRUE(fs_is_file(aaa));
    EXPECT_FALSE(fs_is_file(bbb));
    EXPECT_TRUE(fs_is_file(ccc));

    free(aaa);
    free(bbb);
    free(ccc);
    free(out_dir);
    free(pak);
}

static void test_format_hint_mismatch(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("hint")), 0);
    char *wad = under_scratch("hint/hint.wad");
    char *pk3 = under_scratch("hint/hint.pk3");

    wad_entry_t entries[] = {{"PLAYPAL", "palette-bytes", strlen("palette-bytes")}};
    EXPECT_EQ(write_wad_synth(wad, "IWAD", entries, 1), 0);

    /* IWAD opened with --format pwad → must reject. */
    const char *argv1[] = {g_pakka_path, "-l", "--format", "pwad", wad, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv1), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);

    /* PK3 opened with --format iwad → must reject. */
    RUN_PAKKA_OK(&r, "-c", "--format", "pk3", pk3);
    proc_result_free(&r);
    const char *argv2[] = {g_pakka_path, "-l", "--format", "iwad", pk3, NULL};
    EXPECT_EQ(run_pakka_capture(&r, argv2), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);

    free(wad);
    free(pk3);
}

static void test_auto_resolves_to_iwad_without_pack_probe(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("auto")), 0);
    char *pak = under_scratch("auto/auto.wad");

    wad_entry_t entries[] = {{"PLAYPAL", "palette-bytes", strlen("palette-bytes")}};
    EXPECT_EQ(write_wad_synth(pak, "IWAD", entries, 1), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", pak);
    /* Daikatana mention would mean the ambiguous-PACK probe fired —
     * AUTO on a WAD must not surface it. */
    if (r.stdout_buf && strstr(r.stdout_buf, "Daikatana")) {
        proc_result_free(&r);
        FAIL("AUTO mode on WAD surfaced a Daikatana-related diagnostic");
    }
    proc_result_free(&r);
    free(pak);
}

static void test_oversize_numlumps_rejected(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("huge")), 0);
    char *pak = under_scratch("huge/huge.wad");

    /* PAKFILE_MAX_ENTRIES is 1,048,576; 0x10000000 (268,435,456) is
     * way over the cap and would imply a 4 GiB directory. */
    unsigned char hdr[12];
    memcpy(hdr, "IWAD", 4);
    put_u32_le(hdr, 4, 0x10000000U);
    put_u32_le(hdr, 8, 12);
    EXPECT_EQ(fs_write_file(pak, hdr, 12), 0);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] &&
            (strstr(streams[s], "too many") || strstr(streams[s], "Too many") ||
             strstr(streams[s], "max") || strstr(streams[s], "Max"))) {
            found = 1;
        }
    }
    if (!found) {
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        FAIL("expected 'too many' / 'max' diagnostic on oversize numlumps");
    }
    proc_result_free(&r);
    free(pak);
}

static void test_garbage_filepos_rejected(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("bad")), 0);
    char *pak = under_scratch("bad/bad.wad");

    /* IWAD with 1 lump named "BOGUS"; filepos = 0xFFFFFFFF (wraps past EOF). */
    unsigned char hdr[12];
    memcpy(hdr, "IWAD", 4);
    put_u32_le(hdr, 4, 1);
    put_u32_le(hdr, 8, 12);
    unsigned char row[16] = {0};
    put_u32_le(row, 0, 0xFFFFFFFFU);
    put_u32_le(row, 4, 4);
    memcpy(row + 8, "BOGUS", 5);

    FILE *f = fopen(pak, "wb");
    EXPECT_NOT_NULL(f);
    fwrite(hdr, 1, 12, f);
    fwrite(row, 1, 16, f);
    fclose(f);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] &&
            (strstr(streams[s], "out of range") ||
             strstr(streams[s], "bytes out"))) {
            found = 1;
        }
    }
    if (!found) {
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        FAIL("expected 'out of range' / 'bytes out' diagnostic");
    }
    proc_result_free(&r);
    free(pak);
}

static void test_verify_valid_passes_truncated_fails(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("verify")), 0);

    char *ok_pak = under_scratch("verify/verify_ok.wad");
    wad_entry_t entries[] = {{"PLAYPAL", "palette-bytes", strlen("palette-bytes")}};
    EXPECT_EQ(write_wad_synth(ok_pak, "IWAD", entries, 1), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "--verify", ok_pak);
    proc_result_free(&r);

    /* Truncated entry: size points past EOF. */
    char *bad_pak = under_scratch("verify/verify_bad.wad");
    unsigned char hdr[12];
    memcpy(hdr, "IWAD", 4);
    put_u32_le(hdr, 4, 1);
    put_u32_le(hdr, 8, 12);
    unsigned char row[16] = {0};
    put_u32_le(row, 0, 12);    /* filepos */
    put_u32_le(row, 4, 1000);  /* size far past EOF */
    memcpy(row + 8, "BAD", 3);

    FILE *f = fopen(bad_pak, "wb");
    EXPECT_NOT_NULL(f);
    fwrite(hdr, 1, 12, f);
    fwrite(row, 1, 16, f);
    fclose(f);

    const char *argv[] = {g_pakka_path, "--verify", bad_pak, NULL};
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);

    free(ok_pak);
    free(bad_pak);
}

static void test_verify_duplicate_lump_names_warning(void)
{
    /* Real Doom maps repeat lump names across every map (THINGS, LINEDEFS, ...).
     * pakka_verify documents these as PAKKA_REPORT_WARNING on WAD only — the
     * run must still exit OK so `pakka --verify` against a stock IWAD doesn't
     * spuriously fail. */
    EXPECT_EQ(fs_mkdir_p(under_scratch("verify_dup")), 0);
    char *pak = under_scratch("verify_dup/dup_lumps.wad");

    wad_entry_t entries[] = {
        {"THINGS", "map1-things", strlen("map1-things")},
        {"THINGS", "map2-things", strlen("map2-things")},
    };
    EXPECT_EQ(write_wad_synth(pak, "IWAD", entries, 2), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "--verify", pak);
    /* Output must mention a WARNING about duplicate lumps. */
    int saw_warning_dup = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && strstr(streams[s], "WARNING") &&
            strstr(streams[s], "uplicate lump")) {
            saw_warning_dup = 1;
        }
    }
    if (!saw_warning_dup) {
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        FAIL("expected WARNING + 'duplicate lump' diagnostic");
    }
    /* And the relaxation must be WAD-specific — no normalized-collision ERROR. */
    for (int s = 0; s < 2; s++) {
        if (streams[s] && strstr(streams[s], "ERROR") &&
            strstr(streams[s], "Normalized collision")) {
            proc_result_free(&r);
            FAIL("Normalized collision ERROR should not appear on WAD verify");
        }
    }
    proc_result_free(&r);
    free(pak);
}

int main(void)
{
    const char *pakka = getenv("PAKKA");
    if (!pakka || !*pakka) {
        fprintf(stderr, "wad_test: PAKKA env var not set\n");
        return 1;
    }
    g_pakka_path = pakka;

    const char *scratch = getenv("WAD_TEST_SCRATCH");
    if (!scratch || !*scratch) {
        scratch = "build/test/wad";
    }
    fs_rmtree(scratch);
    if (fs_mkdir_p(scratch) != 0) {
        fprintf(stderr, "wad_test: mkdir(%s) failed\n", scratch);
        return 1;
    }
    g_scratch = strdup(scratch);

    RUN_TEST(test_empty_iwad_has_12_byte_header);
    RUN_TEST(test_wad_extension_defaults_to_pwad);
    RUN_TEST(test_two_entry_iwad_lists_in_dir_order);
    RUN_TEST(test_extract_single_entry_byte_identical);
    RUN_TEST(test_eight_char_lump_name_accepted);
    RUN_TEST(test_directory_row_order_filepos_size_name);
    RUN_TEST(test_create_and_add_round_trips_row_order);
    RUN_TEST(test_marker_lump_accepted);
    RUN_TEST(test_duplicate_lump_names_load);
    RUN_TEST(test_dup_name_pwad_ok_pak_rejects);
    RUN_TEST(test_extract_all_refuses_dup_collision);
    RUN_TEST(test_extract_by_name_first_match);
    RUN_TEST(test_delete_removes_first_match);
    RUN_TEST(test_rebuild_middle_delete_preserves_survivors);
    RUN_TEST(test_format_hint_mismatch);
    RUN_TEST(test_auto_resolves_to_iwad_without_pack_probe);
    RUN_TEST(test_oversize_numlumps_rejected);
    RUN_TEST(test_garbage_filepos_rejected);
    RUN_TEST(test_verify_valid_passes_truncated_fails);
    RUN_TEST(test_verify_duplicate_lump_names_warning);

    free(g_scratch);
    return t_summary();
}
