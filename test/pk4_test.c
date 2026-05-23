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

static char *under_scratch(const char *sub)
{
    return fs_join(g_scratch, sub);
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
    free(buf);
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
    free(bp);

    char *out_dir = under_scratch("create/out");
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);
    RUN_PAKKA_OK(&r, "-x", "-C", out_dir, built);
    proc_result_free(&r);

    EXPECT_EQ(fs_diff_tree(src, out_dir), 0);

    free(src);
    free(src_d);
    free(atxt);
    free(btxt);
    free(built);
    free(out_dir);
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
    free(bp);

    free(src);
    free(xtxt);
    free(built);
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

    free(src);
    free(keep);
    free(remove);
    free(pak);
    free(out_dir);
    free(out_keep);
    free(out_remove);
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

    free(src);
    free(ptxt);
    free(real_pak);
    free(disguised);
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

    RUN_TEST(test_create_builds_valid_zip);
    RUN_TEST(test_create_uppercase_pk4_extension);
    RUN_TEST(test_delete_rebuild_produces_valid_pk4);
    RUN_TEST(test_pack_magic_in_pk4_opens_as_pak);

    free(g_scratch);
    return t_summary();
}
