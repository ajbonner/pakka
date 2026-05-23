/* pak_goldsrc_test — GoldSrc PAK parity fixtures.
 *
 * Two independently-gated fixtures (Half-Life Uplink + Day One). Each
 * case SKIPs when its fixture env var is unset, so this binary is
 * safe to include in default `make test` / `ctest`. The format-probe
 * case calls pakka_open + pakka_format in-process via the linked
 * libpakka archive. */

#include "fs.h"
#include "pakka.h"
#include "proc.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_pakka_path;
static const char *g_uplink_path;
static const char *g_dayone_path;
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

static int uplink_gated(void)
{
    if (!g_uplink_path || !*g_uplink_path || !fs_is_file(g_uplink_path)) return 1;
    return 0;
}
static int dayone_gated(void)
{
    if (!g_dayone_path || !*g_dayone_path || !fs_is_file(g_dayone_path)) return 1;
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

/* Capture pakka -l output, normalize CRLF (already done by proc.c),
 * return a malloc'd copy of stdout. Caller frees. */
static char *capture_list(const char *pak)
{
    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    if (proc_run(argv, NULL, &r) != 0) return NULL;
    if (r.exit_code != 0) {
        proc_result_free(&r);
        return NULL;
    }
    char *copy = r.stdout_buf;
    r.stdout_buf = NULL; /* transfer ownership */
    proc_result_free(&r);
    return copy;
}

static char *capture_list_with_format(const char *pak, const char *fmt)
{
    const char   *argv[] = {g_pakka_path, "-l", "--format", fmt, pak, NULL};
    proc_result_t r;
    if (proc_run(argv, NULL, &r) != 0) return NULL;
    if (r.exit_code != 0) {
        proc_result_free(&r);
        return NULL;
    }
    char *copy = r.stdout_buf;
    r.stdout_buf = NULL;
    proc_result_free(&r);
    return copy;
}

/* ---------- uplink ---------- */

static void test_uplink_lists_1952_entries(void)
{
    if (uplink_gated()) SKIP("GOLDSRC_UPLINK_PAK0 not set or fixture missing");
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", g_uplink_path);
    EXPECT_EQ((long long)r.line_count, 1952);
    proc_result_free(&r);
}

static void test_uplink_format_aliases_match(void)
{
    if (uplink_gated()) SKIP("GOLDSRC_UPLINK_PAK0 not set or fixture missing");
    char *pak_out = capture_list_with_format(g_uplink_path, "pak");
    char *gs_out  = capture_list_with_format(g_uplink_path, "goldsrc");
    char *hl_out  = capture_list_with_format(g_uplink_path, "hl");
    EXPECT_NOT_NULL(pak_out);
    EXPECT_NOT_NULL(gs_out);
    EXPECT_NOT_NULL(hl_out);
    EXPECT_TRUE(strlen(pak_out) > 0);
    EXPECT_STREQ(pak_out, gs_out);
    EXPECT_STREQ(pak_out, hl_out);
    t_free(pak_out);
    t_free(gs_out);
    t_free(hl_out);
}

static void test_uplink_tree_renders(void)
{
    if (uplink_gated()) SKIP("GOLDSRC_UPLINK_PAK0 not set or fixture missing");
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", "--tree", g_uplink_path);
    EXPECT_STR_CONTAINS(r.stdout_buf, "maps");
    EXPECT_STR_CONTAINS(r.stdout_buf, "models");
    proc_result_free(&r);
}

static void test_uplink_structural_verify_passes(void)
{
    if (uplink_gated()) SKIP("GOLDSRC_UPLINK_PAK0 not set or fixture missing");
    proc_result_t r;
    RUN_PAKKA_OK(&r, "--verify", g_uplink_path);
    if (r.stdout_buf && (strstr(r.stdout_buf, "ERROR") || strstr(r.stdout_buf, "WARNING"))) {
        proc_result_free(&r);
        FAIL("structural --verify surfaced ERROR/WARNING on Uplink");
    }
    proc_result_free(&r);
}

static void test_uplink_full_extract_materializes_asset_families(void)
{
    if (uplink_gated()) SKIP("GOLDSRC_UPLINK_PAK0 not set or fixture missing");
    EXPECT_EQ(fs_mkdir_p(under_scratch("uplink")), 0);
    char *out = under_scratch("uplink/out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out, g_uplink_path);
    proc_result_free(&r);

    char *maps = fs_join(out, "maps");
    char *models = fs_join(out, "models");
    EXPECT_TRUE(fs_is_dir(maps));
    EXPECT_TRUE(fs_is_dir(models));
    t_free(maps);
    t_free(models);

    /* Uplink's tram-ride opener map t0a0.bsp = 3,003,032 bytes. */
    char  *bsp = fs_join(out, "maps/t0a0.bsp");
    EXPECT_TRUE(fs_is_file(bsp));
    size_t n   = 0;
    unsigned char *buf = fs_read_file(bsp, &n);
    EXPECT_EQ((long long)n, 3003032);
    t_free(buf);
    t_free(bsp);
    t_free(out);
}

static void test_uplink_format_returns_pak(void)
{
    if (uplink_gated()) SKIP("GOLDSRC_UPLINK_PAK0 not set or fixture missing");
    pakka_archive_t *a   = NULL;
    pakka_error_t    err = {0};
    pakka_status_t   s   = pakka_open(g_uplink_path, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK);
    EXPECT_EQ((int)pakka_format(a), (int)PAKKA_FORMAT_PAK);
    pakka_close(a, NULL);
}

static void test_uplink_delete_rebuild_drops_only_victim(void)
{
    if (uplink_gated()) SKIP("GOLDSRC_UPLINK_PAK0 not set or fixture missing");
    EXPECT_EQ(fs_mkdir_p(under_scratch("uplink_del")), 0);
    char *mut = under_scratch("uplink_del/uplink-mut.pak");
    EXPECT_EQ(copy_file(g_uplink_path, mut), 0);

    /* Snapshot the listing and identify the first entry (the delete victim). */
    char *before = capture_list(mut);
    EXPECT_NOT_NULL(before);

    char *nl = strchr(before, '\n');
    EXPECT_NOT_NULL(nl);
    /* First line shape: "<entry/name> (NNN bytes)" — victim is the
     * first whitespace-delimited token. */
    size_t first_len = (size_t)(nl - before);
    char  *first     = (char *)malloc(first_len + 1);
    memcpy(first, before, first_len);
    first[first_len] = '\0';
    char *space      = strchr(first, ' ');
    if (space) *space = '\0';
    char *victim     = strdup(first);
    t_free(first);
    EXPECT_NOT_NULL(victim);

    /* Count lines before. */
    size_t before_lines = 0;
    for (const char *p = before; *p; p++) if (*p == '\n') before_lines++;

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-d", mut, victim);
    proc_result_free(&r);

    char *after = capture_list(mut);
    EXPECT_NOT_NULL(after);
    size_t after_lines = 0;
    for (const char *p = after; *p; p++) if (*p == '\n') after_lines++;
    EXPECT_EQ((long long)(before_lines - after_lines), 1);

    /* Victim should no longer appear; every other line should match. */
    char victim_prefix[256];
    snprintf(victim_prefix, sizeof(victim_prefix), "%s ", victim);
    if (strstr(after, victim_prefix)) {
        t_free(before);
        t_free(after);
        t_free(victim);
        FAIL("victim still appears in listing after delete");
    }

    RUN_PAKKA_OK(&r, "--verify", mut);
    proc_result_free(&r);

    t_free(before);
    t_free(after);
    t_free(victim);
    t_free(mut);
}

/* ---------- dayone ---------- */

static void test_dayone_lists_2598_entries(void)
{
    if (dayone_gated()) SKIP("GOLDSRC_DAYONE_PAK0 not set or fixture missing");
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", g_dayone_path);
    EXPECT_EQ((long long)r.line_count, 2598);
    proc_result_free(&r);
}

static void test_dayone_format_alias_matches(void)
{
    if (dayone_gated()) SKIP("GOLDSRC_DAYONE_PAK0 not set or fixture missing");
    char *pak_out = capture_list_with_format(g_dayone_path, "pak");
    char *gs_out  = capture_list_with_format(g_dayone_path, "goldsrc");
    EXPECT_NOT_NULL(pak_out);
    EXPECT_NOT_NULL(gs_out);
    EXPECT_TRUE(strlen(pak_out) > 0);
    EXPECT_STREQ(pak_out, gs_out);
    t_free(pak_out);
    t_free(gs_out);
}

static void test_dayone_structural_verify_passes(void)
{
    if (dayone_gated()) SKIP("GOLDSRC_DAYONE_PAK0 not set or fixture missing");
    proc_result_t r;
    RUN_PAKKA_OK(&r, "--verify", g_dayone_path);
    if (r.stdout_buf && (strstr(r.stdout_buf, "ERROR") || strstr(r.stdout_buf, "WARNING"))) {
        proc_result_free(&r);
        FAIL("structural --verify surfaced ERROR/WARNING on Day One");
    }
    proc_result_free(&r);
}

static void test_dayone_extract_spot_checks_c0a0_bsp(void)
{
    if (dayone_gated()) SKIP("GOLDSRC_DAYONE_PAK0 not set or fixture missing");
    EXPECT_EQ(fs_mkdir_p(under_scratch("dayone")), 0);
    char *out = under_scratch("dayone/out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out, g_dayone_path);
    proc_result_free(&r);

    char *maps = fs_join(out, "maps");
    EXPECT_TRUE(fs_is_dir(maps));
    t_free(maps);

    /* c0a0.bsp = 2,232,604 bytes (Black Mesa monorail intro). */
    char  *bsp = fs_join(out, "maps/c0a0.bsp");
    EXPECT_TRUE(fs_is_file(bsp));
    size_t n   = 0;
    unsigned char *buf = fs_read_file(bsp, &n);
    EXPECT_EQ((long long)n, 2232604);
    t_free(buf);
    t_free(bsp);
    t_free(out);
}

int main(void)
{
    const char *pakka = getenv("PAKKA");
    if (!pakka || !*pakka) {
        fprintf(stderr, "pak_goldsrc_test: PAKKA env var not set\n");
        return 1;
    }
    g_pakka_path  = pakka;
    g_uplink_path = getenv("GOLDSRC_UPLINK_PAK0");
    g_dayone_path = getenv("GOLDSRC_DAYONE_PAK0");

    const char *scratch = getenv("GOLDSRC_TEST_SCRATCH");
    if (!scratch || !*scratch) scratch = "build/test/goldsrc_scratch";
    fs_rmtree(scratch);
    if (fs_mkdir_p(scratch) != 0) {
        fprintf(stderr, "pak_goldsrc_test: mkdir(%s) failed\n", scratch);
        return 1;
    }
    g_scratch = strdup(scratch);

    RUN_TEST(test_uplink_lists_1952_entries);
    RUN_TEST(test_uplink_format_aliases_match);
    RUN_TEST(test_uplink_tree_renders);
    RUN_TEST(test_uplink_structural_verify_passes);
    RUN_TEST(test_uplink_full_extract_materializes_asset_families);
    RUN_TEST(test_uplink_format_returns_pak);
    RUN_TEST(test_uplink_delete_rebuild_drops_only_victim);
    RUN_TEST(test_dayone_lists_2598_entries);
    RUN_TEST(test_dayone_format_alias_matches);
    RUN_TEST(test_dayone_structural_verify_passes);
    RUN_TEST(test_dayone_extract_spot_checks_c0a0_bsp);

    t_free(g_scratch);
    return t_summary();
}
