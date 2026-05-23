/* sin_test — SiN (Ritual, 1998) pak format end-to-end tests.
 *
 * Differs from Quake/Q2 PAK in three places: "SPAK" magic, 120-byte
 * filename field (vs PAK's 56), 128-byte directory entry (vs 64).
 * Payloads are still uncompressed. */

#include "fs.h"
#include "proc.h"
#include "test_macros.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIN_HEADER_SIZE  12
#define SIN_NAME_FIELD  120
#define SIN_DIR_ENTRY   128 /* 120-byte name + 4-byte offset + 4-byte length */

static const char *g_pakka_path;
static char       *g_scratch;

static char *under_scratch(const char *sub)
{
    /* t_track adds the malloc'd path to the per-test arena; t_test_end
     * frees it after each test. Tests must NOT explicitly free the
     * result — that would double-free against the arena cleanup. */
    return (char *)t_track(fs_join(g_scratch, sub));
}

/* Write a u32 little-endian to a fixed-size buffer at the given offset. */
static void put_u32_le(unsigned char *buf, size_t off, uint32_t v)
{
    buf[off + 0] = (unsigned char)(v & 0xFF);
    buf[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    buf[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    buf[off + 3] = (unsigned char)((v >> 24) & 0xFF);
}

/* Build a single-entry SiN pak with the given entry name and payload.
 * Layout: 12-byte header (SPAK + diroffset + dirlength) + payload +
 * 128-byte directory entry (120-byte name field + offset + length). */
static int write_sin_one_entry(const char *path, const char *name, const char *payload)
{
    size_t name_len = strlen(name);
    if (name_len > SIN_NAME_FIELD) {
        return -1;
    }
    size_t payload_len = strlen(payload);

    unsigned char header[SIN_HEADER_SIZE];
    memcpy(header, "SPAK", 4);
    put_u32_le(header, 4, (uint32_t)(SIN_HEADER_SIZE + payload_len)); /* diroffset */
    put_u32_le(header, 8, SIN_DIR_ENTRY);                              /* dirlength */

    unsigned char dir[SIN_DIR_ENTRY];
    memset(dir, 0, sizeof(dir));
    memcpy(dir, name, name_len);
    put_u32_le(dir, SIN_NAME_FIELD,     SIN_HEADER_SIZE);          /* entry offset */
    put_u32_le(dir, SIN_NAME_FIELD + 4, (uint32_t)payload_len);    /* entry length */

    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        (payload_len > 0 && fwrite(payload, 1, payload_len, f) != payload_len) ||
        fwrite(dir, 1, sizeof(dir), f) != sizeof(dir)) {
        fclose(f);
        return -1;
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

/* Read the first four bytes of a file and compare to the given magic.
 * Returns 0 on match, non-zero on mismatch / error. */
static int file_starts_with(const char *path, const char magic[4])
{
    size_t         len = 0;
    unsigned char *buf = fs_read_file(path, &len);
    if (!buf || len < 4) {
        t_free(buf);
        return -1;
    }
    int rc = memcmp(buf, magic, 4);
    t_free(buf);
    return rc;
}

/* ---------- tests ---------- */

static void test_open_spak_magic_recognised(void)
{
    char *pak = under_scratch("open/demo.sin");
    EXPECT_EQ(fs_mkdir_p(under_scratch("open")), 0);
    EXPECT_EQ(write_sin_one_entry(pak, "maps/sintest.bsp", "hello-sin"), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", pak);
    EXPECT_NOT_NULL(r.stdout_buf);
    EXPECT_STR_CONTAINS(r.stdout_buf, "maps/sintest.bsp");
    proc_result_free(&r);
}

static void test_extract_round_trips(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("extract")), 0);
    char *pak     = under_scratch("extract/demo.sin");
    char *out_dir = under_scratch("extract/out");
    EXPECT_EQ(write_sin_one_entry(pak, "maps/sintest.bsp", "hello-sin-roundtrip"), 0);
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out_dir, pak);
    proc_result_free(&r);

    char *extracted = fs_join(out_dir, "maps/sintest.bsp");
    EXPECT_TRUE(fs_is_file(extracted));

    size_t         n   = 0;
    unsigned char *got = fs_read_file(extracted, &n);
    EXPECT_NOT_NULL(got);
    EXPECT_EQ((long long)n, (long long)strlen("hello-sin-roundtrip"));
    EXPECT_MEM_EQ(got, "hello-sin-roundtrip", n);

    t_free(got);
    t_free(extracted);
}

static void test_create_sin_extension_produces_spak(void)
{
    char *src = under_scratch("create_ext/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *ftxt = fs_join(src, "file.txt");
    EXPECT_EQ(fs_write_file(ftxt, "payload\n", 8), 0);

    char *pak = under_scratch("create_ext/created.sin");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, "file.txt");
    proc_result_free(&r);

    EXPECT_TRUE(fs_is_file(pak));
    EXPECT_EQ(file_starts_with(pak, "SPAK"), 0);

    t_free(ftxt);
}

static void test_format_override_overrides_extension(void)
{
    char *src = under_scratch("create_fmt/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *ftxt = fs_join(src, "file.txt");
    EXPECT_EQ(fs_write_file(ftxt, "payload\n", 8), 0);

    /* .pak extension but --format sin should produce SPAK. */
    char *pak = under_scratch("create_fmt/created.pak");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, "--format", "sin", "file.txt");
    proc_result_free(&r);

    EXPECT_TRUE(fs_is_file(pak));
    EXPECT_EQ(file_starts_with(pak, "SPAK"), 0);

    t_free(ftxt);
}

static void test_create_add_list_extract_round_trip(void)
{
    char *src = under_scratch("rt/src");
    EXPECT_EQ(fs_mkdir_p(t_track(fs_join(src, "maps"))), 0);
    char *maps_e1m1 = fs_join(src, "maps/e1m1.bsp");
    char *p_blast   = fs_join(src, "p_blast.mdl");
    EXPECT_EQ(fs_write_file(maps_e1m1, "level data\n", 11), 0);
    EXPECT_EQ(fs_write_file(p_blast, "model bytes\n", 12), 0);

    char *pak = under_scratch("rt/full.sin");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, "maps", "p_blast.mdl");
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "-l", pak);
    EXPECT_NOT_NULL(r.stdout_buf);
    EXPECT_STR_CONTAINS(r.stdout_buf, "maps/e1m1.bsp");
    EXPECT_STR_CONTAINS(r.stdout_buf, "p_blast.mdl");
    proc_result_free(&r);

    char *out_dir = under_scratch("rt/out");
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);
    RUN_PAKKA_OK(&r, "-x", "-C", out_dir, pak);
    proc_result_free(&r);

    EXPECT_EQ(fs_diff_tree(src, out_dir), 0);

    t_free(maps_e1m1);
    t_free(p_blast);
}

/* Run pakka and return its exit code; caller decides whether to
 * assert on the value (used for the "must fail" cases). Captures
 * output in `r` for the caller's substring assertions. */
static int run_pakka_capture(proc_result_t *r, const char *const *argv)
{
    if (proc_run(argv, NULL, r) != 0) {
        return -1;
    }
    return 0;
}

static void test_add_accepts_119_byte_name(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("longname")), 0);
    char *pak = under_scratch("longname/init.sin");
    char *src = under_scratch("longname/payload.txt");
    EXPECT_EQ(fs_write_file(src, "p\n", 2), 0);

    /* Create an empty SiN pak via --format sin (no positional args). */
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak, "--format", "sin");
    proc_result_free(&r);

    char longname[120];
    memset(longname, 'a', 119);
    longname[119] = '\0';

    RUN_PAKKA_OK(&r, "-a", pak, "--as", longname, src);
    proc_result_free(&r);

}

static void test_add_rejects_120_byte_name(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("toolong")), 0);
    char *pak = under_scratch("toolong/init.sin");
    char *src = under_scratch("toolong/payload.txt");
    EXPECT_EQ(fs_write_file(src, "p\n", 2), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak, "--format", "sin");
    proc_result_free(&r);

    char toolong[121];
    memset(toolong, 'a', 120);
    toolong[120] = '\0';

    const char *argv[] = {g_pakka_path, "-a", pak, "--as", toolong, src, NULL};
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);

    /* The error message must mention name length. Check combined
     * stdout + stderr because pakka writes diagnostics to either. */
    int found = 0;
    if (r.stdout_buf && strstr(r.stdout_buf, "too long")) found = 1;
    if (r.stderr_buf && strstr(r.stderr_buf, "too long")) found = 1;
    if (!found) {
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        FAIL("expected 'too long' diagnostic for 120-byte name");
    }
    proc_result_free(&r);

}

static void test_freshly_created_spak_lists_ok(void)
{
    /* User-visible signal that pakka recognises the SPAK format on
     * the read side after creating one. CLI-only check; the
     * pakka_format() API itself is exercised via c_api_test. */
    char *src = under_scratch("fmt/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *xtxt = fs_join(src, "x.txt");
    EXPECT_EQ(fs_write_file(xtxt, "p\n", 2), 0);

    char *pak = under_scratch("fmt/fmt.sin");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, "x.txt");
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "-l", pak);
    proc_result_free(&r);

    t_free(xtxt);
}

static void test_format_pak_rejects_spak(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("hint")), 0);
    char *pak = under_scratch("hint/demo.sin");
    EXPECT_EQ(write_sin_one_entry(pak, "x", "y"), 0);

    const char *argv[] = {g_pakka_path, "-l", pak, "--format", "pak", NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);

    int found = 0;
    if (r.stdout_buf && strstr(r.stdout_buf, "format_hint")) found = 1;
    if (r.stderr_buf && strstr(r.stderr_buf, "format_hint")) found = 1;
    if (!found) {
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        FAIL("expected 'format_hint' diagnostic for --format pak on SPAK");
    }
    proc_result_free(&r);
}

static void test_delete_rebuild_keeps_remaining_entries(void)
{
    char *src = under_scratch("del/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *a = fs_join(src, "a.txt");
    char *b = fs_join(src, "b.txt");
    char *c = fs_join(src, "c.txt");
    EXPECT_EQ(fs_write_file(a, "keep1\n", 6), 0);
    EXPECT_EQ(fs_write_file(b, "drop\n", 5), 0);
    EXPECT_EQ(fs_write_file(c, "keep2\n", 6), 0);

    char *pak = under_scratch("del/del.sin");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, "a.txt", "b.txt", "c.txt");
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "-d", pak, "b.txt");
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "-l", pak);
    EXPECT_NOT_NULL(r.stdout_buf);
    EXPECT_STR_CONTAINS(r.stdout_buf, "a.txt");
    EXPECT_STR_CONTAINS(r.stdout_buf, "c.txt");
    if (strstr(r.stdout_buf, "b.txt") != NULL) {
        proc_result_free(&r);
        FAIL("b.txt should have been deleted but still appears in -l");
    }
    proc_result_free(&r);

    t_free(a);
    t_free(b);
    t_free(c);
}

int main(void)
{
    const char *pakka = getenv("PAKKA");
    if (!pakka || !*pakka) {
        fprintf(stderr, "sin_test: PAKKA env var not set\n");
        return 1;
    }
    g_pakka_path = pakka;

    const char *scratch = getenv("SIN_TEST_SCRATCH");
    if (!scratch || !*scratch) {
        scratch = "build/test/sin";
    }
    fs_rmtree(scratch);
    if (fs_mkdir_p(scratch) != 0) {
        fprintf(stderr, "sin_test: mkdir(%s) failed\n", scratch);
        return 1;
    }
    g_scratch = strdup(scratch);

    RUN_TEST(test_open_spak_magic_recognised);
    RUN_TEST(test_extract_round_trips);
    RUN_TEST(test_create_sin_extension_produces_spak);
    RUN_TEST(test_format_override_overrides_extension);
    RUN_TEST(test_create_add_list_extract_round_trip);
    RUN_TEST(test_add_accepts_119_byte_name);
    RUN_TEST(test_add_rejects_120_byte_name);
    RUN_TEST(test_freshly_created_spak_lists_ok);
    RUN_TEST(test_format_pak_rejects_spak);
    RUN_TEST(test_delete_rebuild_keeps_remaining_entries);

    t_free(g_scratch);
    return t_summary();
}
