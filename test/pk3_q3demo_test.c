/* pk3_q3demo_test — Q3 demo pak0.pk3 fixture. C peer of test/pk3_q3demo.bats.
 *
 * Gated on the Q3DEMO_PAK0_PK3 env var (set by the realpak-test-q3
 * Makefile target / fixtures.ps1 on Windows). If unset, every case
 * SKIPs so this binary is safe to include in `make test` /
 * `ctest`. The pakka_format() c-api case from the bats stays in
 * pk3_q3demo.bats because it needs an inline-cc harness linked
 * against libpakka headers — that flow doesn't translate cleanly
 * to a precompiled C test binary. */

#include "fs.h"
#include "proc.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_pakka_path;
static const char *g_q3demo_path;
static char       *g_scratch;

static char *under_scratch(const char *sub) { return (char *)t_track(fs_join(g_scratch, sub)); }

#define RUN_PAKKA_OK(out_result, ...) do {                                  \
    const char *_argv[] = { g_pakka_path, __VA_ARGS__, NULL };              \
    if (proc_run(_argv, NULL, (out_result)) != 0)                           \
        FAIL("proc_run failed to launch pakka");                            \
    if ((out_result)->exit_code != 0) {                                     \
        fprintf(stderr, "    pakka exit=%d\n    stderr: %s\n",              \
                (out_result)->exit_code,                                    \
                (out_result)->stderr_buf ? (out_result)->stderr_buf : "");  \
        FAIL("pakka exited non-zero");                                      \
    }                                                                       \
} while (0)

static int gated(void)
{
    if (!g_q3demo_path || !*g_q3demo_path || !fs_is_file(g_q3demo_path)) {
        return 1;
    }
    return 0;
}

static void test_lists_1274_entries(void)
{
    if (gated()) SKIP("Q3DEMO_PAK0_PK3 not set or fixture missing");
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", g_q3demo_path);
    EXPECT_EQ((long long)r.line_count, 1274);
    proc_result_free(&r);
}

static void test_tree_renders_with_recognizable_paths(void)
{
    if (gated()) SKIP("Q3DEMO_PAK0_PK3 not set or fixture missing");
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", "--tree", g_q3demo_path);
    EXPECT_STR_CONTAINS(r.stdout_buf, "models");
    EXPECT_STR_CONTAINS(r.stdout_buf, "sound");
    proc_result_free(&r);
}

static void test_structural_verify_succeeds_no_warnings(void)
{
    if (gated()) SKIP("Q3DEMO_PAK0_PK3 not set or fixture missing");
    proc_result_t r;
    RUN_PAKKA_OK(&r, "--verify", g_q3demo_path);
    if (r.stdout_buf && (strstr(r.stdout_buf, "ERROR") || strstr(r.stdout_buf, "WARNING"))) {
        proc_result_free(&r);
        FAIL("structural --verify surfaced ERROR/WARNING on Q3 demo");
    }
    proc_result_free(&r);
}

static void test_deep_verify_succeeds_no_warnings(void)
{
    if (gated()) SKIP("Q3DEMO_PAK0_PK3 not set or fixture missing");
    proc_result_t r;
    RUN_PAKKA_OK(&r, "--verify", "--deep", g_q3demo_path);
    if (r.stdout_buf && (strstr(r.stdout_buf, "ERROR") || strstr(r.stdout_buf, "WARNING"))) {
        proc_result_free(&r);
        FAIL("--verify --deep surfaced ERROR/WARNING on Q3 demo");
    }
    proc_result_free(&r);
}

static void test_extract_spot_checks_known_assets(void)
{
    if (gated()) SKIP("Q3DEMO_PAK0_PK3 not set or fixture missing");
    EXPECT_EQ(fs_mkdir_p(under_scratch("extract")), 0);
    char *out = under_scratch("extract/out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out, g_q3demo_path);
    proc_result_free(&r);

    /* vm/cgame.qvm: 236956 bytes (pinned by the wrapper SHA). */
    char  *qvm = fs_join(out, "vm/cgame.qvm");
    EXPECT_TRUE(fs_is_file(qvm));
    size_t qn  = 0;
    unsigned char *qbuf = fs_read_file(qvm, &qn);
    EXPECT_EQ((long long)qn, 236956);
    t_free(qbuf);
    t_free(qvm);

    /* scripts/base.shader: 2247 bytes. */
    char  *shader = fs_join(out, "scripts/base.shader");
    EXPECT_TRUE(fs_is_file(shader));
    size_t sn     = 0;
    unsigned char *sbuf = fs_read_file(shader, &sn);
    EXPECT_EQ((long long)sn, 2247);
    t_free(sbuf);
    t_free(shader);

    t_free(out);
}

int main(void)
{
    const char *pakka = getenv("PAKKA");
    if (!pakka || !*pakka) {
        fprintf(stderr, "pk3_q3demo_test: PAKKA env var not set\n");
        return 1;
    }
    g_pakka_path  = pakka;
    g_q3demo_path = getenv("Q3DEMO_PAK0_PK3");

    const char *scratch = getenv("Q3DEMO_TEST_SCRATCH");
    if (!scratch || !*scratch) scratch = "build/test/q3demo_scratch";
    fs_rmtree(scratch);
    if (fs_mkdir_p(scratch) != 0) {
        fprintf(stderr, "pk3_q3demo_test: mkdir(%s) failed\n", scratch);
        return 1;
    }
    g_scratch = strdup(scratch);

    RUN_TEST(test_lists_1274_entries);
    RUN_TEST(test_tree_renders_with_recognizable_paths);
    RUN_TEST(test_structural_verify_succeeds_no_warnings);
    RUN_TEST(test_deep_verify_succeeds_no_warnings);
    RUN_TEST(test_extract_spot_checks_known_assets);

    t_free(g_scratch);
    return t_summary();
}
