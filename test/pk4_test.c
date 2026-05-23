/* pk4_test — pakka's handling of the PK4 (Doom 3) container.
 *
 * Partial C peer of test/pk4.bats. Ports the 4 cases that build their
 * own fixtures via pakka itself; the 3 bats cases that depend on
 * /usr/bin/zip stay in pk4.bats during the migration window and are
 * exercised by the Unix bats path.
 *
 * PK4 is byte-identical to PK3 on disk; this suite covers the format-
 * labeling layer (extension sniff, ZIP create dispatch, magic-wins-
 * over-extension) plus the delete/rebuild commit path under a PK4
 * label. */

#include "fs.h"
#include "proc.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_pakka_path;
static char       *g_scratch;

/* Built once in main() by build_deflate_pk4_fixture; reused by the
 * three deflate-fixture-backed tests. NULL if the build failed; tests
 * EXPECT_NOT_NULL it before proceeding. */
static char       *g_deflate_pk4;
static char       *g_deflate_src; /* source tree, kept for fs_diff_tree */

static char *under_scratch(const char *sub)
{
    return (char *)t_track(fs_join(g_scratch, sub));
}

static int write_text(const char *path, const char *content)
{
    return fs_write_file(path, content, strlen(content));
}

/* Run pakka and require exit_code == 0. On failure: dump captured
 * stdout/stderr and propagate via FAIL (which returns from the caller). */
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

static int copy_file(const char *src, const char *dst)
{
    size_t         n   = 0;
    unsigned char *buf = fs_read_file(src, &n);
    if (!buf) return -1;
    int rc = fs_write_file(dst, buf, n);
    t_free(buf);
    return rc;
}

/* ---------- tests ---------- */

static void test_create_builds_valid_zip(void)
{
    /* Mirrors pk4.bats "pk4 create: -c .pk4 builds a valid ZIP and round-trips". */
    char *src     = under_scratch("create/src");
    char *src_d   = fs_join(src, "d");
    EXPECT_EQ(fs_mkdir_p(src_d), 0);
    char *atxt = fs_join(src, "a.txt");
    char *btxt = fs_join(src_d, "b.txt");
    EXPECT_EQ(write_text(atxt, "a\n"), 0);
    EXPECT_EQ(write_text(btxt, "b nested\n"), 0);

    char *built = under_scratch("create/built.pk4");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", built, "a.txt", "d");
    proc_result_free(&r);
    EXPECT_TRUE(fs_is_file(built));

    /* ZIP local-file-header magic: 0x50 0x4b 0x03 0x04 ("PK\3\4"). */
    size_t         blen = 0;
    unsigned char *bp   = fs_read_file(built, &blen);
    EXPECT_NOT_NULL(bp);
    EXPECT_TRUE(blen >= 4);
    EXPECT_MEM_EQ(bp, "PK\x03\x04", 4);
    t_free(bp);

    char *out_dir = under_scratch("create/out");
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);
    RUN_PAKKA_OK(&r, "-x", "-C", out_dir, built);
    proc_result_free(&r);

    EXPECT_EQ(fs_diff_tree(src, out_dir), 0);

    t_free(src);
    t_free(src_d);
    t_free(atxt);
    t_free(btxt);
    t_free(built);
    t_free(out_dir);
}

static void test_create_uppercase_pk4_extension(void)
{
    /* Mirrors pk4.bats "pk4 create: uppercase .PK4 extension is also recognised". */
    char *src = under_scratch("upper/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *xtxt = fs_join(src, "x.txt");
    EXPECT_EQ(write_text(xtxt, "x\n"), 0);

    char *built = under_scratch("upper/UPPER.PK4");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", built, "x.txt");
    proc_result_free(&r);

    size_t         blen = 0;
    unsigned char *bp   = fs_read_file(built, &blen);
    EXPECT_NOT_NULL(bp);
    EXPECT_TRUE(blen >= 4);
    EXPECT_MEM_EQ(bp, "PK\x03\x04", 4);
    t_free(bp);

    t_free(src);
    t_free(xtxt);
    t_free(built);
}

static void test_delete_rebuild_produces_valid_pk4(void)
{
    /* Mirrors pk4.bats "pk4 delete + close: rebuild path produces a valid PK4". */
    char *src = under_scratch("del/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *keep   = fs_join(src, "keep.txt");
    char *remove = fs_join(src, "remove.txt");
    EXPECT_EQ(write_text(keep, "keep\n"), 0);
    EXPECT_EQ(write_text(remove, "remove\n"), 0);

    char *pak = under_scratch("del/del.pk4");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, "keep.txt", "remove.txt");
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "-d", pak, "remove.txt");
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "--verify", pak);
    proc_result_free(&r);

    char *out_dir = under_scratch("del/out");
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);
    RUN_PAKKA_OK(&r, "-x", "-C", out_dir, pak);
    proc_result_free(&r);

    char *out_keep   = fs_join(out_dir, "keep.txt");
    char *out_remove = fs_join(out_dir, "remove.txt");
    EXPECT_TRUE(fs_is_file(out_keep));
    EXPECT_FALSE(fs_is_file(out_remove));

    t_free(src);
    t_free(keep);
    t_free(remove);
    t_free(pak);
    t_free(out_dir);
    t_free(out_keep);
    t_free(out_remove);
}

/* Build a multi-entry DEFLATE PK4 via pakka -c --compress so the
 * round-trip tests have a fixture without needing /usr/bin/zip. The
 * resulting archive's DEFLATE-encoded entries exercise the same reader
 * path the bats tests cover. Returns absolute path to the built pak,
 * or NULL on any setup failure; the caller frees the path. */
static char *build_deflate_pk4_fixture(void)
{
    char *src    = under_scratch("deflate_fixture/src");
    char *src_d  = fs_join(src, "sub");
    if (fs_mkdir_p(src_d) != 0) return NULL;
    char *hello  = fs_join(src, "hello.txt");
    char *nested = fs_join(src_d, "nested.txt");
    if (write_text(hello, "hello pk4") != 0) return NULL;
    if (write_text(nested, "nested\n") != 0) return NULL;

    /* Compressible payload large enough that pakka picks DEFLATE — the
     * defining trait of a real-world Doom 3 PK4 vs a fresh-made PK3. */
    char   line[] = "The quick brown fox jumps over the lazy dog. ";
    size_t ln     = sizeof(line) - 1;
    size_t total  = ln * 250;
    char  *lorem_data = (char *)malloc(total);
    if (!lorem_data) return NULL;
    for (size_t i = 0; i < 250; i++) memcpy(lorem_data + i * ln, line, ln);
    char *lorem = fs_join(src, "lorem.txt");
    int   rc    = fs_write_file(lorem, lorem_data, total);
    t_free(lorem_data);
    if (rc != 0) return NULL;

    char *pak = under_scratch("deflate_fixture/mixed.pk4");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    const char *argv[] = {g_pakka_path, "-c", "--compress", pak,
                          "hello.txt", "sub", "lorem.txt", NULL};
    if (proc_run(argv, &opts, &r) != 0) return NULL;
    int exit_code = r.exit_code;
    proc_result_free(&r);
    if (exit_code != 0) return NULL;

    t_free(src_d);
    t_free(hello);
    t_free(nested);
    t_free(lorem);

    /* Stash src for the diff_tree-based round-trip test; pak path is
     * the function's return value. Caller frees both. */
    g_deflate_src = src;
    return pak;
}

static void test_list_enumerates_deflate_pk4_entries(void)
{
    EXPECT_NOT_NULL(g_deflate_pk4);
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", g_deflate_pk4);
    EXPECT_STR_CONTAINS(r.stdout_buf, "hello.txt");
    EXPECT_STR_CONTAINS(r.stdout_buf, "sub/nested.txt");
    EXPECT_STR_CONTAINS(r.stdout_buf, "lorem.txt");
    proc_result_free(&r);
}

static void test_extract_deflate_round_trips(void)
{
    EXPECT_NOT_NULL(g_deflate_pk4);
    char *out_dir = under_scratch("deflate_fixture/out");
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out_dir, g_deflate_pk4);
    proc_result_free(&r);

    EXPECT_EQ(fs_diff_tree(g_deflate_src, out_dir), 0);
    t_free(out_dir);
}

static void test_verify_synthetic_pk4_passes_deep_checks(void)
{
    EXPECT_NOT_NULL(g_deflate_pk4);
    proc_result_t r;
    RUN_PAKKA_OK(&r, "--verify", "--deep", g_deflate_pk4);
    proc_result_free(&r);
}

static void test_pack_magic_in_pk4_opens_as_pak(void)
{
    /* Mirrors pk4.bats "pk4 open: PACK magic in a .pk4 file opens as
     * PAK (magic wins)". Extension is only the tiebreaker between PK3
     * and PK4 labels — magic still decides what container is being
     * read. A PAK file renamed to .pk4 must still read as a PAK. */
    char *src = under_scratch("magic/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *ptxt = fs_join(src, "p.txt");
    EXPECT_EQ(write_text(ptxt, "pak content\n"), 0);

    char *real_pak  = under_scratch("magic/real.pak");
    char *disguised = under_scratch("magic/disguised.pk4");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", real_pak, "p.txt");
    proc_result_free(&r);

    EXPECT_EQ(copy_file(real_pak, disguised), 0);

    RUN_PAKKA_OK(&r, "-l", disguised);
    EXPECT_NOT_NULL(r.stdout_buf);
    EXPECT_STR_CONTAINS(r.stdout_buf, "p.txt");
    proc_result_free(&r);

    t_free(src);
    t_free(ptxt);
    t_free(real_pak);
    t_free(disguised);
}

int main(void)
{
    const char *pakka = getenv("PAKKA");
    if (!pakka || !*pakka) {
        fprintf(stderr, "pk4_test: PAKKA env var not set\n");
        return 1;
    }
    g_pakka_path = pakka;

    const char *scratch = getenv("PK4_TEST_SCRATCH");
    if (!scratch || !*scratch) {
        scratch = "build/test/pk4";
    }
    /* Tear down any prior run's fixtures. pakka -c refuses to
     * overwrite an existing destination, so leftover .pk4 files
     * from the last run would fail every test on rerun. */
    fs_rmtree(scratch);
    if (fs_mkdir_p(scratch) != 0) {
        fprintf(stderr, "pk4_test: mkdir(%s) failed\n", scratch);
        return 1;
    }
    g_scratch = strdup(scratch);

    /* Build the DEFLATE fixture once — the three round-trip tests
     * share it (mirroring the bats setup_file pattern). The fixture
     * builder uses under_scratch internally, which is arena-tracked
     * and freed at t_test_end; strdup'ing here detaches the strings
     * so they outlive the per-test cleanup. */
    char *raw_pk4 = build_deflate_pk4_fixture();
    g_deflate_pk4 = raw_pk4 ? strdup(raw_pk4) : NULL;
    char *src_dup = g_deflate_src ? strdup(g_deflate_src) : NULL;
    g_deflate_src = src_dup;

    RUN_TEST(test_create_builds_valid_zip);
    RUN_TEST(test_create_uppercase_pk4_extension);
    RUN_TEST(test_delete_rebuild_produces_valid_pk4);
    RUN_TEST(test_list_enumerates_deflate_pk4_entries);
    RUN_TEST(test_extract_deflate_round_trips);
    RUN_TEST(test_verify_synthetic_pk4_passes_deep_checks);
    RUN_TEST(test_pack_magic_in_pk4_opens_as_pak);

    t_free(g_scratch);
    return t_summary();
}
