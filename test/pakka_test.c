/* pakka_test — Quake PAK format. Partial C peer of test/pakka.bats.
 *
 * Covers ~40 of pakka.bats's 65 cases — the ones that aren't POSIX-
 * symlink / chmod / shell-pipeline specific. The remaining cases stay
 * in pakka.bats during the migration window (those that need ln(1),
 * chmod a-w, or external commands like awk/find with specific output
 * shapes that don't add value to re-encode in C). */

#include "fs.h"
#include "proc.h"
#include "test_macros.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAK_HEADER_SIZE 12
#define PAK_NAME_FIELD  56
#define PAK_DIR_ENTRY   64

static const char *g_pakka_path;
static const char *g_pak0_path;    /* canonical pak0.pak from setup */
static char       *g_extracted;    /* setup_file: PAK0 extracted */
static char       *g_rebuilt;      /* setup_file: rebuilt.pak from extracted */
static char       *g_scratch;

static char *under_scratch(const char *sub) { return (char *)t_track(fs_join(g_scratch, sub)); }

static void put_u32_le(unsigned char *buf, size_t off, uint32_t v)
{
    buf[off + 0] = (unsigned char)(v & 0xFF);
    buf[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    buf[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    buf[off + 3] = (unsigned char)((v >> 24) & 0xFF);
}

/* Build a 64-byte PAK directory row for an entry with given name +
 * offset/length. name_bytes lets callers inject raw byte patterns
 * (control chars, backslashes) without C string-literal escaping. */
static int fill_pak_dir_row(unsigned char dir[PAK_DIR_ENTRY],
                            const void *name_bytes, size_t name_len,
                            uint32_t offset, uint32_t length)
{
    if (name_len > PAK_NAME_FIELD) return -1;
    memset(dir, 0, PAK_DIR_ENTRY);
    if (name_len > 0) memcpy(dir, name_bytes, name_len);
    put_u32_le(dir, PAK_NAME_FIELD, offset);
    put_u32_le(dir, PAK_NAME_FIELD + 4, length);
    return 0;
}

/* Variant accepting an explicit byte count so the caller can inject
 * names with embedded NULs / non-ASCII bytes that strlen would
 * mistruncate. */
static int write_pak_one_entry_bytes(const char *path,
                                     const void *name, size_t name_len)
{
    unsigned char header[PAK_HEADER_SIZE];
    memcpy(header, "PACK", 4);
    put_u32_le(header, 4, PAK_HEADER_SIZE);
    put_u32_le(header, 8, PAK_DIR_ENTRY);

    unsigned char dir[PAK_DIR_ENTRY];
    /* offset=76 (past directory), length=0 — pakka errors before
     * reading the (nonexistent) payload. */
    if (fill_pak_dir_row(dir, name, name_len, 76, 0) != 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        fwrite(dir, 1, sizeof(dir), f) != sizeof(dir)) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Minimal valid PAK with one zero-length entry. Mirrors bats's
 * write_pak_one_entry — 12-byte header + 64-byte dir entry, no payload. */
static int write_pak_one_entry(const char *path, const char *name)
{
    return write_pak_one_entry_bytes(path, name, strlen(name));
}

/* Two-entry PAK, both with zero-length payloads at offset=140. Used by
 * the normalized-collision and preflight-rejects tests. */
static int write_pak_two_entries(const char *path,
                                 const void *name_a, size_t alen,
                                 const void *name_b, size_t blen)
{
    unsigned char header[PAK_HEADER_SIZE];
    memcpy(header, "PACK", 4);
    put_u32_le(header, 4, PAK_HEADER_SIZE);
    put_u32_le(header, 8, PAK_DIR_ENTRY * 2);

    unsigned char dir_a[PAK_DIR_ENTRY], dir_b[PAK_DIR_ENTRY];
    if (fill_pak_dir_row(dir_a, name_a, alen, 140, 0) != 0) return -1;
    if (fill_pak_dir_row(dir_b, name_b, blen, 140, 0) != 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        fwrite(dir_a, 1, sizeof(dir_a), f) != sizeof(dir_a) ||
        fwrite(dir_b, 1, sizeof(dir_b), f) != sizeof(dir_b)) {
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

/* Count files (not dirs) in `path`, recursive. Used for "extracted N
 * files" assertions. */
static int count_files_recursive(const char *path);

#ifdef _WIN32
#include <windows.h>
static int count_files_win32(const char *path)
{
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char *child = fs_join(path, fd.cFileName);
        if (fs_is_dir(child)) {
            count += count_files_win32(child);
        } else if (fs_is_file(child)) {
            count++;
        }
        t_free(child);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
}
static int count_files_recursive(const char *path) { return count_files_win32(path); }
#else
#include <dirent.h>
static int count_files_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return 0;
    int            count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char *child = fs_join(path, e->d_name);
        if (fs_is_dir(child)) {
            count += count_files_recursive(child);
        } else if (fs_is_file(child)) {
            count++;
        }
        t_free(child);
    }
    closedir(d);
    return count;
}
#endif

static int files_equal(const char *a, const char *b)
{
    size_t         an = 0, bn = 0;
    unsigned char *ad = fs_read_file(a, &an);
    unsigned char *bd = fs_read_file(b, &bn);
    int            rc = (ad && bd && an == bn && memcmp(ad, bd, an) == 0) ? 0 : 1;
    t_free(ad);
    t_free(bd);
    return rc;
}

/* ---------- group A: PAK0 fixture-backed tests ---------- */

static void test_list_pak0_contains_339_entries(void)
{
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", g_pak0_path);
    EXPECT_EQ((long long)r.line_count, 339);
    proc_result_free(&r);
}

static void test_round_trip_content_identical(void)
{
    char *re_extracted = under_scratch("rt/re_extracted");
    EXPECT_EQ(fs_mkdir_p(re_extracted), 0);
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", re_extracted, g_rebuilt);
    proc_result_free(&r);
    EXPECT_EQ(fs_diff_tree(g_extracted, re_extracted), 0);
    t_free(re_extracted);
}

static void test_byte_delta_matches_orphan_size(void)
{
    /* id's pak0.pak ships with 432,312 bytes of internal gaps and a
     * 410,616-byte orphan progs.dat in a hole. pakka's pack is compact,
     * so the rebuilt file is exactly 410,616 bytes shorter. */
    size_t         orig_n = 0, new_n = 0;
    unsigned char *orig   = fs_read_file(g_pak0_path, &orig_n);
    unsigned char *neu    = fs_read_file(g_rebuilt, &new_n);
    EXPECT_NOT_NULL(orig);
    EXPECT_NOT_NULL(neu);
    EXPECT_EQ((long long)(orig_n - new_n), 410616);
    t_free(orig);
    t_free(neu);
}

static void test_extract_specific_files_only(void)
{
    char *out = under_scratch("extract_specific/out");
    EXPECT_EQ(fs_mkdir_p(out), 0);
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out, g_pak0_path, "default.cfg", "quake.rc");
    proc_result_free(&r);
    EXPECT_EQ(count_files_recursive(out), 2);

    char *out_default = fs_join(out, "default.cfg");
    char *src_default = fs_join(g_extracted, "default.cfg");
    EXPECT_EQ(files_equal(out_default, src_default), 0);
    t_free(out_default);
    t_free(src_default);
    t_free(out);
}

static void test_add_new_entry_round_trips(void)
{
    char *work    = under_scratch("add_new/work.pak");
    char *added   = under_scratch("add_new/added.txt");
    char *out     = under_scratch("add_new/out");
    char *out_add = NULL;
    EXPECT_EQ(fs_mkdir_p(under_scratch("add_new")), 0);
    EXPECT_EQ(copy_file(g_rebuilt, work), 0);
    EXPECT_EQ(fs_write_file(added, "pakka crud test payload\n", 24), 0);

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = under_scratch("add_new");
    RUN_PAKKA_OK_CWD(&r, &opts, "-a", "work.pak", "added.txt");
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "-l", work);
    EXPECT_STR_CONTAINS(r.stdout_buf, "added.txt ");
    proc_result_free(&r);

    EXPECT_EQ(fs_mkdir_p(out), 0);
    RUN_PAKKA_OK(&r, "-x", "-C", out, work, "added.txt");
    proc_result_free(&r);

    out_add = fs_join(out, "added.txt");
    EXPECT_EQ(files_equal(out_add, added), 0);
    t_free(out_add);
    t_free(work);
    t_free(added);
    t_free(out);
}

static void test_extract_missing_path_errors(void)
{
    char *out = under_scratch("missing/out");
    EXPECT_EQ(fs_mkdir_p(out), 0);
    const char   *argv[] = {g_pakka_path, "-x", "-C", out, g_pak0_path,
                            "no-such-file-in-pak", NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(out);
}

static void test_extract_duplicate_path_args_succeed(void)
{
    char *out = under_scratch("dup_arg/out");
    EXPECT_EQ(fs_mkdir_p(out), 0);
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out, g_pak0_path, "default.cfg", "default.cfg");
    proc_result_free(&r);
    char *got = fs_join(out, "default.cfg");
    char *src = fs_join(g_extracted, "default.cfg");
    EXPECT_TRUE(fs_is_file(got));
    EXPECT_EQ(files_equal(got, src), 0);
    t_free(got);
    t_free(src);
    t_free(out);
}

/* ---------- group B: magic / format error paths ---------- */

static void test_pak_with_only_magic_rejected(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("magic_only")), 0);
    char *pak = under_scratch("magic_only/truncated.pak");
    EXPECT_EQ(fs_write_file(pak, "PACK", 4), 0);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

static void test_pak_with_truncated_directory_rejected(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("trunc_dir")), 0);
    char *pak = under_scratch("trunc_dir/short_dir.pak");

    /* Header claims 1 entry (dirlength=64) at offset 12, but file ends
     * at byte 12. load_directory's fread should catch the short read. */
    unsigned char hdr[PAK_HEADER_SIZE];
    memcpy(hdr, "PACK", 4);
    put_u32_le(hdr, 4, PAK_HEADER_SIZE);
    put_u32_le(hdr, 8, PAK_DIR_ENTRY);
    EXPECT_EQ(fs_write_file(pak, hdr, sizeof(hdr)), 0);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

static void test_bad_pack_magic_rejected(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("badmagic")), 0);
    char         *pak = under_scratch("badmagic/badmagic.pak");
    unsigned char hdr[PAK_HEADER_SIZE];
    memcpy(hdr, "NOPE", 4);
    put_u32_le(hdr, 4, PAK_HEADER_SIZE);
    put_u32_le(hdr, 8, 0);
    EXPECT_EQ(fs_write_file(pak, hdr, sizeof(hdr)), 0);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

/* ---------- group C: per-entry-name policy (write_pak_one_entry) ---------- */

static void expect_refuses(const char *name, const char *scratch_sub)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch(scratch_sub)), 0);
    char *pak = fs_join(under_scratch(scratch_sub), "evil.pak");
    EXPECT_EQ(write_pak_one_entry(pak, name), 0);
    char *out = fs_join(under_scratch(scratch_sub), "out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    const char   *argv[] = {g_pakka_path, "-x", "-C", out, pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && strstr(streams[s], "Refusing to extract")) found = 1;
    }
    if (!found) {
        fprintf(stderr, "    name=%s stdout: %s stderr: %s\n", name,
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        proc_result_free(&r);
        t_free(pak);
        t_free(out);
        FAIL("expected 'Refusing to extract' diagnostic");
    }
    proc_result_free(&r);
    t_free(pak);
    t_free(out);
}

static void test_refuses_con(void)        { expect_refuses("CON", "con"); }
static void test_refuses_nul_txt(void)    { expect_refuses("NUL.txt", "nul_txt"); }
static void test_refuses_foo_com1(void)   { expect_refuses("foo/COM1", "foo_com1"); }
static void test_refuses_ads_colon(void)  { expect_refuses("file:stream", "ads"); }
static void test_refuses_trailing_dot(void)   { expect_refuses("foo.", "trail_dot"); }
static void test_refuses_trailing_space(void) { expect_refuses("foo ", "trail_space"); }

static void test_refuses_control_byte(void)
{
    /* "esc\x1binj" — printable name with embedded escape byte. */
    EXPECT_EQ(fs_mkdir_p(under_scratch("ctrlbyte")), 0);
    char         *pak  = fs_join(under_scratch("ctrlbyte"), "evil.pak");
    unsigned char name[] = {'e', 's', 'c', 0x1B, 'i', 'n', 'j'};
    EXPECT_EQ(write_pak_one_entry_bytes(pak, name, sizeof(name)), 0);
    char *out = fs_join(under_scratch("ctrlbyte"), "out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    const char   *argv[] = {g_pakka_path, "-x", "-C", out, pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
    t_free(out);
}

static void test_com10_allowed(void)
{
    /* COM10 is NOT a reserved Windows device (only COM0..COM9). pakka
     * must allow it. */
    EXPECT_EQ(fs_mkdir_p(under_scratch("com10")), 0);
    char *pak = fs_join(under_scratch("com10"), "safe.pak");
    EXPECT_EQ(write_pak_one_entry(pak, "COM10"), 0);
    char *out = fs_join(under_scratch("com10"), "out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out, pak);
    proc_result_free(&r);
    char *got = fs_join(out, "COM10");
    EXPECT_TRUE(fs_is_file(got));
    t_free(got);
    t_free(pak);
    t_free(out);
}

/* ---------- group D: path-traversal rejection ---------- */

static void test_refuses_dotdot_traversal(void)        { expect_refuses("../escape", "dotdot"); }
static void test_refuses_absolute_posix_path(void)     { expect_refuses("/etc/passwd", "absposix"); }
static void test_refuses_backslash_dotdot(void)        { expect_refuses("..\\escape", "bsdotdot"); }
static void test_refuses_leading_backslash(void)       { expect_refuses("\\etc\\passwd", "leadbs"); }
static void test_refuses_drive_letter(void)            { expect_refuses("C:\\evil", "drive"); }
static void test_refuses_unc_path(void)                { expect_refuses("\\\\server\\share\\evil", "unc"); }

static void test_refuses_empty_all_nul_name(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("emptyname")), 0);
    char         *pak    = fs_join(under_scratch("emptyname"), "evil.pak");
    unsigned char empty[1] = {0};
    EXPECT_EQ(write_pak_one_entry_bytes(pak, empty, 0), 0);
    char *out = fs_join(under_scratch("emptyname"), "out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    const char   *argv[] = {g_pakka_path, "-x", "-C", out, pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
    t_free(out);
}

static void test_legit_dotdot_substring_still_extracts(void)
{
    /* "foo..bar" contains ".." as a substring but no segment is "..",
     * so the safety check must allow it. */
    EXPECT_EQ(fs_mkdir_p(under_scratch("foo..bar_sub")), 0);
    char *pak = fs_join(under_scratch("foo..bar_sub"), "ok.pak");
    EXPECT_EQ(write_pak_one_entry(pak, "foo..bar"), 0);
    char *out = fs_join(under_scratch("foo..bar_sub"), "out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out, pak);
    proc_result_free(&r);
    char *got = fs_join(out, "foo..bar");
    EXPECT_TRUE(fs_is_file(got));
    t_free(got);
    t_free(pak);
    t_free(out);
}

/* ---------- group E: normalization-collision rejection ---------- */

static void test_refuses_case_fold_collision(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("case_collide")), 0);
    char *pak = fs_join(under_scratch("case_collide"), "case_collide.pak");
    EXPECT_EQ(write_pak_two_entries(pak, "Foo.txt", 7, "foo.txt", 7), 0);
    char *out = fs_join(under_scratch("case_collide"), "out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    const char   *argv[] = {g_pakka_path, "-x", "-C", out, pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && strstr(streams[s], "collide after normalization")) found = 1;
    }
    if (!found) {
        proc_result_free(&r);
        t_free(pak);
        t_free(out);
        FAIL("expected 'collide after normalization' diagnostic");
    }
    /* Neither variant should be materialized. */
    char *out_upper = fs_join(out, "Foo.txt");
    char *out_lower = fs_join(out, "foo.txt");
    EXPECT_FALSE(fs_is_file(out_upper));
    EXPECT_FALSE(fs_is_file(out_lower));
    proc_result_free(&r);
    t_free(out_upper);
    t_free(out_lower);
    t_free(pak);
    t_free(out);
}

static void test_refuses_slash_backslash_collision(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("sep_collide")), 0);
    char *pak = fs_join(under_scratch("sep_collide"), "sep_collide.pak");
    EXPECT_EQ(write_pak_two_entries(pak, "dir/file", 8, "dir\\file", 8), 0);
    char *out = fs_join(under_scratch("sep_collide"), "out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    const char   *argv[] = {g_pakka_path, "-x", "-C", out, pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
    t_free(out);
}

static void test_preflight_rejects_archive_on_later_unsafe(void)
{
    /* "safe/a" then "../escape". Pre-fix, the safe entry was written
     * before the second entry's validation fired. */
    EXPECT_EQ(fs_mkdir_p(under_scratch("preflight")), 0);
    char *pak = fs_join(under_scratch("preflight"), "mixed.pak");
    EXPECT_EQ(write_pak_two_entries(pak, "safe/a", 6, "../escape", 9), 0);
    char *out = fs_join(under_scratch("preflight"), "out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    const char   *argv[] = {g_pakka_path, "-x", "-C", out, pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);

    /* The earlier safe entry must not be on disk. */
    char *safe_a   = fs_join(out, "safe/a");
    char *safe_dir = fs_join(out, "safe");
    EXPECT_FALSE(fs_is_file(safe_a));
    EXPECT_FALSE(fs_is_dir(safe_dir));
    t_free(safe_a);
    t_free(safe_dir);
    t_free(pak);
    t_free(out);
}

/* ---------- group F: --tree rendering ---------- */

static void test_tree_rejected_with_non_list_mode(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("tree_reject/out")), 0);
    const char *argv[] = {g_pakka_path, "-x", "--tree", "-C",
                          under_scratch("tree_reject/out"), g_pak0_path, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && strstr(streams[s], "--tree may only be used with -l")) found = 1;
    }
    if (!found) {
        proc_result_free(&r);
        FAIL("expected '--tree may only be used with -l' diagnostic");
    }
    proc_result_free(&r);
}

/* ---------- group G: CLI argument handling ---------- */

static void test_d_requires_at_least_one_path(void)
{
    char *work = under_scratch("cli_d/work.pak");
    EXPECT_EQ(fs_mkdir_p(under_scratch("cli_d")), 0);
    EXPECT_EQ(copy_file(g_rebuilt, work), 0);

    const char   *argv[] = {g_pakka_path, "-d", work, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && strstr(streams[s], "requires at least one path")) found = 1;
    }
    if (!found) {
        proc_result_free(&r);
        t_free(work);
        FAIL("expected 'requires at least one path' diagnostic");
    }
    proc_result_free(&r);
    t_free(work);
}

static void test_a_requires_at_least_one_path(void)
{
    char *work = under_scratch("cli_a/work.pak");
    EXPECT_EQ(fs_mkdir_p(under_scratch("cli_a")), 0);
    EXPECT_EQ(copy_file(g_rebuilt, work), 0);

    const char   *argv[] = {g_pakka_path, "-a", work, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(work);
}

static void test_rejects_empty_pakfile_name(void)
{
    const char   *argv[] = {g_pakka_path, "-l", "", NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
}

static void test_dash_V_prints_version_banner(void)
{
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-V");
    EXPECT_STR_CONTAINS(r.stdout_buf, "Pakka ");
    EXPECT_STR_CONTAINS(r.stdout_buf, "libpakka ");
    EXPECT_STR_CONTAINS(r.stdout_buf, "Supported formats");
    EXPECT_STR_CONTAINS(r.stdout_buf, "pak");
    EXPECT_STR_CONTAINS(r.stdout_buf, "sin");
    EXPECT_STR_CONTAINS(r.stdout_buf, "daikatana");
    EXPECT_STR_CONTAINS(r.stdout_buf, "pk3");
    EXPECT_STR_CONTAINS(r.stdout_buf, "pk4");
    proc_result_free(&r);
}

static void test_version_alias(void)
{
    proc_result_t r;
    RUN_PAKKA_OK(&r, "--version");
    EXPECT_STR_CONTAINS(r.stdout_buf, "libpakka ");
    proc_result_free(&r);
}

static void test_help_prints_banner(void)
{
    /* help() exits 1 (same as -h's "usage on no args" convention). */
    const char   *argv[] = {g_pakka_path, "--help", NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int has_modes  = 0;
    int has_vlong  = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && strstr(streams[s], "Operation Modes")) has_modes = 1;
        if (streams[s] && strstr(streams[s], "-V, --version")) has_vlong = 1;
    }
    EXPECT_TRUE(has_modes);
    EXPECT_TRUE(has_vlong);
    proc_result_free(&r);
}

static void test_extract_refuses_dash_C_regular_file(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("dashC")), 0);
    char *not_a_dir = fs_join(under_scratch("dashC"), "not-a-dir");
    EXPECT_EQ(fs_write_file(not_a_dir, "hi\n", 3), 0);

    const char   *argv[] = {g_pakka_path, "-x", "-C", not_a_dir, g_pak0_path, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && strstr(streams[s], "not a directory")) found = 1;
    }
    if (!found) {
        proc_result_free(&r);
        t_free(not_a_dir);
        FAIL("expected 'not a directory' diagnostic for -C regular file");
    }
    proc_result_free(&r);
    t_free(not_a_dir);
}

/* ---------- group H: --as ---------- */

static void test_as_stores_virtual_entry_name(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("as_one")), 0);
    char *src = fs_join(under_scratch("as_one"), "real.txt");
    char *pak = fs_join(under_scratch("as_one"), "out.pak");
    EXPECT_EQ(fs_write_file(src, "as-payload\n", 11), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak, "--as", "virtual/name.txt", src);
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "-l", pak);
    EXPECT_STR_CONTAINS(r.stdout_buf, "virtual/name.txt");
    proc_result_free(&r);
    t_free(src);
    t_free(pak);
}

static void test_as_rejects_outside_a_c(void)
{
    /* --as is only valid with -a or -c. -x + --as must error. */
    EXPECT_EQ(fs_mkdir_p(under_scratch("as_outside/out")), 0);
    const char *argv[] = {g_pakka_path, "-x", "--as", "x", "y", "-C",
                          under_scratch("as_outside/out"),
                          g_pak0_path, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
}

/* ---------- group I: verify ---------- */

static void test_verify_succeeds_on_pak0(void)
{
    proc_result_t r;
    RUN_PAKKA_OK(&r, "--verify", g_pak0_path);
    proc_result_free(&r);
}

static void test_verify_fails_on_duplicate_exact_names(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("verify_dup")), 0);
    char *pak = fs_join(under_scratch("verify_dup"), "dup.pak");
    EXPECT_EQ(write_pak_two_entries(pak, "foo.txt", 7, "foo.txt", 7), 0);

    const char   *argv[] = {g_pakka_path, "--verify", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

static void test_verify_rejects_normalized_collision(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("verify_norm")), 0);
    char *pak = fs_join(under_scratch("verify_norm"), "norm.pak");
    EXPECT_EQ(write_pak_two_entries(pak, "Foo.txt", 7, "foo.txt", 7), 0);

    const char   *argv[] = {g_pakka_path, "--verify", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

/* ---------- group J: bounds checks ---------- */

static void write_pak_header_only(const char *path, uint32_t diroffset,
                                  uint32_t dirlength)
{
    unsigned char hdr[PAK_HEADER_SIZE];
    memcpy(hdr, "PACK", 4);
    put_u32_le(hdr, 4, diroffset);
    put_u32_le(hdr, 8, dirlength);
    fs_write_file(path, hdr, sizeof(hdr));
}

static void test_diroffset_past_eof_rejected(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("diroff_eof")), 0);
    char *pak = fs_join(under_scratch("diroff_eof"), "p.pak");
    write_pak_header_only(pak, 0xFFFF0000U, PAK_DIR_ENTRY);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

static void test_dirlength_wraps_past_eof_rejected(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("dirlen_eof")), 0);
    char *pak = fs_join(under_scratch("dirlen_eof"), "p.pak");
    write_pak_header_only(pak, PAK_HEADER_SIZE, 0xFFFFFF00U);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

static void test_entry_offset_inside_header_rejected(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("off_in_hdr")), 0);
    char         *pak = fs_join(under_scratch("off_in_hdr"), "p.pak");
    unsigned char hdr[PAK_HEADER_SIZE];
    memcpy(hdr, "PACK", 4);
    put_u32_le(hdr, 4, PAK_HEADER_SIZE);
    put_u32_le(hdr, 8, PAK_DIR_ENTRY);

    unsigned char dir[PAK_DIR_ENTRY];
    /* offset=4 sits inside the header — must be rejected. */
    EXPECT_EQ(fill_pak_dir_row(dir, "evil", 4, 4, 0), 0);

    FILE *f = fopen(pak, "wb");
    EXPECT_NOT_NULL(f);
    fwrite(hdr, 1, sizeof(hdr), f);
    fwrite(dir, 1, sizeof(dir), f);
    fclose(f);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

/* ---------- group K: zero-byte entry ---------- */

static void test_zero_byte_file_adds_and_extracts(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("zerobyte")), 0);
    char *src = fs_join(under_scratch("zerobyte"), "empty.dat");
    char *pak = fs_join(under_scratch("zerobyte"), "z.pak");
    char *out = fs_join(under_scratch("zerobyte"), "out");
    EXPECT_EQ(fs_write_file(src, "", 0), 0);
    EXPECT_EQ(fs_mkdir_p(out), 0);

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = under_scratch("zerobyte");
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, "empty.dat");
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "-x", "-C", out, pak);
    proc_result_free(&r);
    char *got = fs_join(out, "empty.dat");
    EXPECT_TRUE(fs_is_file(got));
    size_t         n   = 0;
    unsigned char *buf = fs_read_file(got, &n);
    EXPECT_EQ((long long)n, 0);
    t_free(buf);
    t_free(got);
    t_free(src);
    t_free(pak);
    t_free(out);
}

/* ---------- main ---------- */

static int setup_extracted_and_rebuilt(void)
{
    /* strdup detaches from the arena — t_test_end would otherwise free
     * these between tests since under_scratch is arena-tracked. */
    char *raw_e = under_scratch("setup/extracted");
    char *raw_r = under_scratch("setup/rebuilt.pak");
    if (!raw_e || !raw_r) return -1;
    g_extracted = strdup(raw_e);
    g_rebuilt   = strdup(raw_r);
    if (!g_extracted || !g_rebuilt) return -1;
    if (fs_mkdir_p(g_extracted) != 0) return -1;

    const char   *argv1[] = {g_pakka_path, "-x", "-C", g_extracted, g_pak0_path, NULL};
    proc_result_t r;
    if (proc_run(argv1, NULL, &r) != 0 || r.exit_code != 0) {
        fprintf(stderr, "setup: failed to extract pak0\n");
        proc_result_free(&r);
        return -1;
    }
    proc_result_free(&r);

    /* `pakka -c rebuilt.pak *` requires shell glob expansion. We can't
     * pass `*` as an argv literal — pakka doesn't glob itself. Build the
     * arg list by listing extracted's top-level entries. */
    char **entries = NULL;
    size_t cap = 16, n = 0;
    entries = (char **)calloc(cap, sizeof(char *));
    if (!entries) return -1;

#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s\\*", g_extracted);
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (n + 1 > cap) {
                cap *= 2;
                char **p = (char **)realloc(entries, cap * sizeof(char *));
                if (!p) { FindClose(h); return -1; }
                entries = p;
            }
            entries[n++] = strdup(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(g_extracted);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            if (n + 1 > cap) {
                cap *= 2;
                char **p = (char **)realloc(entries, cap * sizeof(char *));
                if (!p) { closedir(d); return -1; }
                entries = p;
            }
            entries[n++] = strdup(e->d_name);
        }
        closedir(d);
    }
#endif

    /* argv = [pakka, -c, rebuilt.pak, <entry1>, <entry2>, ..., NULL] */
    const char **argv2 = (const char **)calloc(n + 4, sizeof(char *));
    if (!argv2) return -1;
    argv2[0] = g_pakka_path;
    argv2[1] = "-c";
    argv2[2] = g_rebuilt;
    for (size_t i = 0; i < n; i++) argv2[3 + i] = entries[i];
    argv2[3 + n] = NULL;

    proc_opts_t opts = {0};
    opts.cwd         = g_extracted;
    int rc           = proc_run(argv2, &opts, &r);
    int exit_code    = r.exit_code;
    proc_result_free(&r);
    for (size_t i = 0; i < n; i++) t_free(entries[i]);
    t_free(entries);
    t_free(argv2);
    if (rc != 0 || exit_code != 0) return -1;
    return 0;
}

int main(void)
{
    const char *pakka = getenv("PAKKA");
    if (!pakka || !*pakka) {
        fprintf(stderr, "pakka_test: PAKKA env var not set\n");
        return 1;
    }
    g_pakka_path = pakka;

    g_pak0_path = getenv("PAK0");
    if (!g_pak0_path || !*g_pak0_path) {
        fprintf(stderr, "pakka_test: PAK0 env var not set\n");
        return 1;
    }
    if (!fs_is_file(g_pak0_path)) {
        fprintf(stderr, "pakka_test: PAK0=%s not found\n", g_pak0_path);
        return 1;
    }

    const char *scratch = getenv("PAKKA_TEST_SCRATCH");
    if (!scratch || !*scratch) scratch = "build/test/pakka";
    fs_rmtree(scratch);
    if (fs_mkdir_p(scratch) != 0) {
        fprintf(stderr, "pakka_test: mkdir(%s) failed\n", scratch);
        return 1;
    }
    g_scratch = strdup(scratch);

    fprintf(stdout, "==> setup: extract + rebuild pak0\n");
    fflush(stdout);
    if (setup_extracted_and_rebuilt() != 0) {
        fprintf(stderr, "pakka_test: setup failed\n");
        return 1;
    }

    RUN_TEST(test_list_pak0_contains_339_entries);
    RUN_TEST(test_round_trip_content_identical);
    RUN_TEST(test_byte_delta_matches_orphan_size);
    RUN_TEST(test_extract_specific_files_only);
    RUN_TEST(test_add_new_entry_round_trips);
    RUN_TEST(test_extract_missing_path_errors);
    RUN_TEST(test_extract_duplicate_path_args_succeed);

    RUN_TEST(test_pak_with_only_magic_rejected);
    RUN_TEST(test_pak_with_truncated_directory_rejected);
    RUN_TEST(test_bad_pack_magic_rejected);

    RUN_TEST(test_refuses_con);
    RUN_TEST(test_refuses_nul_txt);
    RUN_TEST(test_refuses_foo_com1);
    RUN_TEST(test_refuses_ads_colon);
    RUN_TEST(test_refuses_trailing_dot);
    RUN_TEST(test_refuses_trailing_space);
    RUN_TEST(test_refuses_control_byte);
    RUN_TEST(test_com10_allowed);

    RUN_TEST(test_refuses_dotdot_traversal);
    RUN_TEST(test_refuses_absolute_posix_path);
    RUN_TEST(test_refuses_backslash_dotdot);
    RUN_TEST(test_refuses_leading_backslash);
    RUN_TEST(test_refuses_drive_letter);
    RUN_TEST(test_refuses_unc_path);
    RUN_TEST(test_refuses_empty_all_nul_name);
    RUN_TEST(test_legit_dotdot_substring_still_extracts);

    RUN_TEST(test_refuses_case_fold_collision);
    RUN_TEST(test_refuses_slash_backslash_collision);
    RUN_TEST(test_preflight_rejects_archive_on_later_unsafe);

    RUN_TEST(test_tree_rejected_with_non_list_mode);

    RUN_TEST(test_d_requires_at_least_one_path);
    RUN_TEST(test_a_requires_at_least_one_path);
    RUN_TEST(test_rejects_empty_pakfile_name);
    RUN_TEST(test_dash_V_prints_version_banner);
    RUN_TEST(test_version_alias);
    RUN_TEST(test_help_prints_banner);
    RUN_TEST(test_extract_refuses_dash_C_regular_file);

    RUN_TEST(test_as_stores_virtual_entry_name);
    RUN_TEST(test_as_rejects_outside_a_c);

    RUN_TEST(test_verify_succeeds_on_pak0);
    RUN_TEST(test_verify_fails_on_duplicate_exact_names);
    RUN_TEST(test_verify_rejects_normalized_collision);

    RUN_TEST(test_diroffset_past_eof_rejected);
    RUN_TEST(test_dirlength_wraps_past_eof_rejected);
    RUN_TEST(test_entry_offset_inside_header_rejected);

    RUN_TEST(test_zero_byte_file_adds_and_extracts);

    t_free(g_extracted);
    t_free(g_rebuilt);
    t_free(g_scratch);
    return t_summary();
}
