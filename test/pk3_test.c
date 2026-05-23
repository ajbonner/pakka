/* pk3_test — PK3 (Quake 3) ZIP container.
 *
 * End-to-end coverage of the PK3 reader/writer through the pakka CLI,
 * plus malformed-ZIP rejection cases via the parametric zip_build
 * helper. The c-api reach-through cases (max_decompressed cap,
 * commit-refuses-changed-source, open_entry_handle handles, rebuild
 * rollback in-memory offsets) call pakka_* directly via the linked
 * libpakka archive; same for the format-probe case. Fault-injection
 * tests pass PAKKA_INJECT_FAULT_AT through proc_run's env vector so
 * each spawned pakka process picks up the trigger fresh. */

#include "fs.h"
#include "pakka.h"
#include "proc.h"
#include "test_macros.h"
#include "zip_build.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_pakka_path;
static char       *g_scratch;

static char *under_scratch(const char *sub) { return (char *)t_track(fs_join(g_scratch, sub)); }

static void put_u16_le(unsigned char *buf, size_t off, uint16_t v)
{
    buf[off + 0] = (unsigned char)(v & 0xFF);
    buf[off + 1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_u32_le(unsigned char *buf, size_t off, uint32_t v)
{
    buf[off + 0] = (unsigned char)(v & 0xFF);
    buf[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    buf[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    buf[off + 3] = (unsigned char)((v >> 24) & 0xFF);
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

/* Bytes-search + first/nth CDR method probes live in test_support/zip_build.{h,c}. */

/* ---------- tests: pakka-driven ---------- */

static void test_empty_pk3_is_22_byte_eocd_only(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("empty")), 0);
    char *pak = under_scratch("empty/empty.pk3");

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-c", pak);
    proc_result_free(&r);

    size_t         n   = 0;
    unsigned char *buf = fs_read_file(pak, &n);
    EXPECT_EQ((long long)n, 22);
    EXPECT_MEM_EQ(buf, "PK\x05\x06", 4);
    t_free(buf);

    RUN_PAKKA_OK(&r, "-l", pak);
    proc_result_free(&r);
    t_free(pak);
}

static void test_add_duplicate_entry_refused(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("dup")), 0);
    char *pak = under_scratch("dup/dup.pk3");
    char *src = under_scratch("dup/a.txt");
    EXPECT_EQ(fs_write_file(src, "first\n", 6), 0);

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = under_scratch("dup");
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, "a.txt");
    proc_result_free(&r);

    /* Same name, second time — pakka -a must refuse. cwd=dup/ so the
     * relative "a.txt" arg resolves to the file we just rewrote. */
    EXPECT_EQ(fs_write_file(src, "second\n", 7), 0);
    const char *argv[]  = {g_pakka_path, "-a", pak, "a.txt", NULL};
    if (proc_run(argv, &opts, &r) != 0) {
        t_free(pak);
        t_free(src);
        FAIL("proc_run failed to launch pakka");
    }
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && (strstr(streams[s], "uplicate") || strstr(streams[s], "Duplicate"))) found = 1;
    }
    if (!found) {
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        proc_result_free(&r);
        t_free(pak);
        t_free(src);
        FAIL("expected 'duplicate' diagnostic on add of same entry");
    }
    proc_result_free(&r);
    t_free(pak);
    t_free(src);
}

static void test_delete_close_rebuild_produces_valid_pk3(void)
{
    char *src = under_scratch("del/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *keep_src   = fs_join(src, "keep.txt");
    char *remove_src = fs_join(src, "remove.txt");
    EXPECT_EQ(fs_write_file(keep_src, "keep\n", 5), 0);
    EXPECT_EQ(fs_write_file(remove_src, "remove\n", 7), 0);

    char *pak = under_scratch("del/del.pk3");

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
    t_free(keep_src);
    t_free(remove_src);
    t_free(pak);
    t_free(out_dir);
    t_free(out_keep);
    t_free(out_remove);
}

static void test_compress_encodes_deflate_on_compressible(void)
{
    char *src = under_scratch("compress_deflate/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *lorem = fs_join(src, "lorem.txt");

    /* 500 reps of a 45-char phrase — highly compressible. */
    char        line[]  = "The quick brown fox jumps over the lazy dog. ";
    size_t      line_n  = sizeof(line) - 1;
    size_t      total   = line_n * 500;
    char       *payload = (char *)malloc(total);
    EXPECT_NOT_NULL(payload);
    for (size_t i = 0; i < 500; i++) memcpy(payload + i * line_n, line, line_n);
    EXPECT_EQ(fs_write_file(lorem, payload, total), 0);
    t_free(payload);

    char *pak = under_scratch("compress_deflate/c.pk3");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", "--compress", pak, "lorem.txt");
    proc_result_free(&r);

    size_t         pn  = 0;
    unsigned char *pbuf = fs_read_file(pak, &pn);
    EXPECT_NOT_NULL(pbuf);
    EXPECT_EQ(zip_first_cdr_method(pbuf, pn), 8);
    t_free(pbuf);

    t_free(src);
    t_free(lorem);
    t_free(pak);
}

/* Build a buffer using a small LCG to ensure determinism + high entropy.
 * urandom would also work but introduces flake risk. The bats's
 * /dev/urandom test is platform-specific; we replicate the contract
 * (incompressible input → STORED fallback) with the LCG. */
static void fill_high_entropy(unsigned char *buf, size_t len, uint32_t seed)
{
    uint32_t r = seed;
    for (size_t i = 0; i < len; i++) {
        r      = r * 1103515245U + 12345U;
        buf[i] = (unsigned char)((r >> 16) & 0xFF);
    }
}

/* Build a mixed STORED + DEFLATE PK3 under `scratch_sub` using
 * `pakka -c --compress`, with the same shape the bats setup_file
 * produced via /usr/bin/zip:
 *
 *   hello.txt          — short text (STORED, too small for DEFLATE)
 *   binary.bin         — 11-byte binary blob with embedded NULs
 *   sub/nested.txt     — short text in a subdirectory
 *   lorem.txt          — repeating text large enough to pick DEFLATE
 *
 * Returns the absolute pak path (caller frees) on success, NULL on
 * any setup failure. The same scratch sub also leaves the source
 * tree at `<scratch_sub>/src` for diff_tree against the extract. */
static char *build_mixed_pk3_fixture(const char *scratch_sub)
{
    char *base = under_scratch(scratch_sub);
    char *src  = fs_join(base, "src");
    char *sub  = fs_join(src, "sub");
    if (fs_mkdir_p(sub) != 0) return NULL;

    char *hello = fs_join(src, "hello.txt");
    if (fs_write_file(hello, "hello pk3", 9) != 0) return NULL;
    t_free(hello);

    char *bin = fs_join(src, "binary.bin");
    static const unsigned char bin_bytes[] = {'b', 'i', 'n', 'a', 'r', 'y',
                                              0x00, 0x01, 0x02, 0x03,
                                              'd', 'a', 't', 'a'};
    if (fs_write_file(bin, bin_bytes, sizeof(bin_bytes)) != 0) return NULL;
    t_free(bin);

    char *nested = fs_join(sub, "nested.txt");
    if (fs_write_file(nested, "nested\n", 7) != 0) return NULL;
    t_free(nested);

    /* "The quick brown fox..." x 250 — comfortably above the DEFLATE
     * encoder's "worth it" threshold. */
    static const char line[] = "The quick brown fox jumps over the lazy dog. ";
    const size_t      line_n = sizeof(line) - 1;
    const size_t      total  = line_n * 250;
    char             *text   = (char *)malloc(total);
    if (!text) return NULL;
    for (size_t i = 0; i < 250; i++) memcpy(text + i * line_n, line, line_n);
    char *lorem = fs_join(src, "lorem.txt");
    int   rc    = fs_write_file(lorem, text, total);
    free(text);
    t_free(lorem);
    if (rc != 0) return NULL;

    char         *pak  = fs_join(base, "mixed.pk3");
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    /* Build with --compress; sub/ enters via recursive add. */
    const char   *argv[] = {g_pakka_path, "-c", "--compress", pak,
                            "hello.txt", "binary.bin", "lorem.txt", "sub", NULL};
    proc_result_t r;
    if (proc_run(argv, &opts, &r) != 0 || r.exit_code != 0) {
        proc_result_free(&r);
        return NULL;
    }
    proc_result_free(&r);

    t_free(sub);
    t_free(src);
    return pak;
}

static void test_compress_falls_back_to_stored_on_incompressible(void)
{
    char *src = under_scratch("compress_fallback/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *rnd = fs_join(src, "random.bin");

    unsigned char buf[4096];
    fill_high_entropy(buf, sizeof(buf), 0xDEADBEEFU);
    EXPECT_EQ(fs_write_file(rnd, buf, sizeof(buf)), 0);

    char *pak = under_scratch("compress_fallback/rb.pk3");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", "--compress", pak, "random.bin");
    proc_result_free(&r);

    size_t         pn   = 0;
    unsigned char *pbuf = fs_read_file(pak, &pn);
    EXPECT_NOT_NULL(pbuf);
    /* Method 0 = STORED — encoder's auto-fallback fired. */
    EXPECT_EQ(zip_first_cdr_method(pbuf, pn), 0);
    t_free(pbuf);

    t_free(src);
    t_free(rnd);
    t_free(pak);
}

static void test_compress_mixed_archive(void)
{
    char *src = under_scratch("compress_mixed/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *txt   = fs_join(src, "text.txt");
    char *noise = fs_join(src, "noise.bin");

    /* 2000 reps of "hello " → DEFLATE. 4 KiB of LCG noise → STORED. */
    char   line[] = "hello ";
    size_t line_n = sizeof(line) - 1;
    size_t total  = line_n * 2000;
    char  *text   = (char *)malloc(total);
    EXPECT_NOT_NULL(text);
    for (size_t i = 0; i < 2000; i++) memcpy(text + i * line_n, line, line_n);
    EXPECT_EQ(fs_write_file(txt, text, total), 0);
    t_free(text);

    unsigned char nbuf[4096];
    fill_high_entropy(nbuf, sizeof(nbuf), 0xC0FFEE);
    EXPECT_EQ(fs_write_file(noise, nbuf, sizeof(nbuf)), 0);

    char *pak = under_scratch("compress_mixed/m.pk3");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", "--compress", pak, "text.txt", "noise.bin");
    proc_result_free(&r);

    size_t         pn   = 0;
    unsigned char *pbuf = fs_read_file(pak, &pn);
    EXPECT_NOT_NULL(pbuf);
    /* Order in CDR follows the create argv order: text.txt first, noise.bin second. */
    EXPECT_EQ(zip_nth_cdr_method(pbuf, pn, 0), 8);
    EXPECT_EQ(zip_nth_cdr_method(pbuf, pn, 1), 0);
    t_free(pbuf);

    t_free(src);
    t_free(txt);
    t_free(noise);
    t_free(pak);
}

static void test_compress_round_trip_byte_identical(void)
{
    char *src = under_scratch("compress_rt/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *lorem = fs_join(src, "lorem.txt");
    char *rnd   = fs_join(src, "random.bin");

    char line[] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. ";
    size_t line_n = sizeof(line) - 1;
    size_t total  = line_n * 300;
    char  *text   = (char *)malloc(total);
    for (size_t i = 0; i < 300; i++) memcpy(text + i * line_n, line, line_n);
    EXPECT_EQ(fs_write_file(lorem, text, total), 0);
    t_free(text);

    unsigned char rbuf[4096];
    fill_high_entropy(rbuf, sizeof(rbuf), 0xCAFEBABE);
    EXPECT_EQ(fs_write_file(rnd, rbuf, sizeof(rbuf)), 0);

    char *pak = under_scratch("compress_rt/rt.pk3");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", "--compress", pak, "lorem.txt", "random.bin");
    proc_result_free(&r);

    char *out_dir = under_scratch("compress_rt/out");
    EXPECT_EQ(fs_mkdir_p(out_dir), 0);
    RUN_PAKKA_OK(&r, "-x", "-C", out_dir, pak);
    proc_result_free(&r);

    EXPECT_EQ(fs_diff_tree(src, out_dir), 0);

    t_free(src);
    t_free(lorem);
    t_free(rnd);
    t_free(pak);
    t_free(out_dir);
}

static void test_compress_verify_deep_accepts(void)
{
    char *src = under_scratch("compress_vd/src");
    EXPECT_EQ(fs_mkdir_p(src), 0);
    char *v = fs_join(src, "v.txt");

    char   line[] = "verify me ";
    size_t line_n = sizeof(line) - 1;
    size_t total  = line_n * 1000;
    char  *text   = (char *)malloc(total);
    for (size_t i = 0; i < 1000; i++) memcpy(text + i * line_n, line, line_n);
    EXPECT_EQ(fs_write_file(v, text, total), 0);
    t_free(text);

    char *pak = under_scratch("compress_vd/v.pk3");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", "--compress", pak, "v.txt");
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "--verify", "--deep", pak);
    proc_result_free(&r);

    t_free(src);
    t_free(v);
    t_free(pak);
}

static void test_compress_rejected_on_pak_target(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("compress_pak")), 0);
    char *target = under_scratch("compress_pak/x.pak");
    char *src    = under_scratch("compress_pak/p.txt");
    EXPECT_EQ(fs_write_file(src, "p\n", 2), 0);

    const char   *argv[] = {g_pakka_path, "-c", "--compress", target, src, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && (strstr(streams[s], "PK3") || strstr(streams[s], "DEFLATE"))) found = 1;
    }
    if (!found) {
        proc_result_free(&r);
        t_free(target);
        t_free(src);
        FAIL("expected 'PK3' or 'DEFLATE' in diagnostic on .pak target");
    }
    proc_result_free(&r);
    t_free(target);
    t_free(src);
}

/* ---------- tests: malformed-ZIP rejection ---------- */

static void test_multi_disk_spanning_marker_rejected(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("span")), 0);
    char         *pak = under_scratch("span/span.pk3");
    unsigned char span_marker[4] = {'P', 'K', 0x07, 0x08};
    EXPECT_EQ(fs_write_file(pak, span_marker, sizeof(span_marker)), 0);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

static void test_zip64_sentinels_rejected(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("z64")), 0);
    char *pak = under_scratch("z64/z64.pk3");

    /* EOCD claiming 0xFFFF entries — ZIP64 sentinel pakka doesn't support. */
    unsigned char eocd[22];
    memcpy(eocd, "PK\x05\x06", 4);
    put_u16_le(eocd, 4, 0);
    put_u16_le(eocd, 6, 0);
    put_u16_le(eocd, 8, 0xFFFF);
    put_u16_le(eocd, 10, 0xFFFF);
    put_u32_le(eocd, 12, 0);
    put_u32_le(eocd, 16, 0);
    put_u16_le(eocd, 20, 0);
    EXPECT_EQ(fs_write_file(pak, eocd, sizeof(eocd)), 0);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

static int expect_pakka_rejects(const char *pak, const char *needle_a,
                                const char *needle_b /* may be NULL */)
{
    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    if (run_pakka_capture(&r, argv) != 0) return -1;
    int           rc      = -1;
    int           nonzero = (r.exit_code != 0);
    int           found_a = 0, found_b = (needle_b == NULL);
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && strstr(streams[s], needle_a)) found_a = 1;
        if (needle_b && streams[s] && strstr(streams[s], needle_b)) found_b = 1;
    }
    if (nonzero && found_a && found_b) {
        rc = 0;
    } else {
        fprintf(stderr, "    pakka exit=%d (expected non-zero=%d)\n", r.exit_code, nonzero);
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
    }
    proc_result_free(&r);
    return rc;
}

static void test_open_rejects_dotdot_traversal_in_zip(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("zip_dotdot")), 0);
    char *pak = under_scratch("zip_dotdot/traversal.pk3");

    /* Extract path also exercises the policy; we use -x rather than -l
     * because the bats test does -x and asserts "nothing escaped". */
    const char  *name    = "../escape.txt";
    const char  *payload = "escape\n";
    zip_single_t p       = {0};
    p.name               = name;
    p.name_len           = strlen(name);
    p.payload            = payload;
    p.payload_len        = strlen(payload);
    EXPECT_EQ(zip_write_single(pak, &p), 0);

    char *out = under_scratch("zip_dotdot/out");
    EXPECT_EQ(fs_mkdir_p(out), 0);
    const char   *argv[] = {g_pakka_path, "-x", "-C", out, pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);

    /* Nothing should have escaped out of the destination tree. */
    char *escape = fs_join(under_scratch("zip_dotdot"), "escape.txt");
    EXPECT_FALSE(fs_is_file(escape));
    t_free(escape);
    t_free(pak);
    t_free(out);
}

static void test_open_rejects_embedded_nul_in_name(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("zip_nul")), 0);
    char *pak = under_scratch("zip_nul/embedded_nul.pk3");

    unsigned char name[] = {'g', 'o', 'o', 'd', 0, '.', '.', '/', 'e', 's', 'c'};
    const char   *payload = "x";
    zip_single_t  p       = {0};
    p.name                = name;
    p.name_len            = sizeof(name);
    p.payload             = payload;
    p.payload_len         = 1;
    EXPECT_EQ(zip_write_single(pak, &p), 0);

    EXPECT_EQ(expect_pakka_rejects(pak, "control byte", NULL), 0);
    t_free(pak);
}

static void test_open_rejects_zip_entry_with_reserved_name(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("zip_reserved")), 0);
    char *pak = under_scratch("zip_reserved/reserved.pk3");

    const char  *name    = "CON.txt";
    const char  *payload = "x";
    zip_single_t p       = {0};
    p.name               = name;
    p.name_len           = strlen(name);
    p.payload            = payload;
    p.payload_len        = 1;
    EXPECT_EQ(zip_write_single(pak, &p), 0);

    char *out = under_scratch("zip_reserved/out");
    EXPECT_EQ(fs_mkdir_p(out), 0);
    const char   *argv[] = {g_pakka_path, "-x", "-C", out, pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
    t_free(out);
}

static void test_open_rejects_encrypted_entry(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("zip_enc")), 0);
    char *pak = under_scratch("zip_enc/enc.pk3");

    const char  *name    = "a.txt";
    const char  *payload = "x";
    zip_single_t p       = {0};
    p.name               = name;
    p.name_len           = strlen(name);
    p.payload            = payload;
    p.payload_len        = 1;
    p.gp_flags           = 0x0001; /* encrypted */
    EXPECT_EQ(zip_write_single(pak, &p), 0);

    EXPECT_EQ(expect_pakka_rejects(pak, "Encrypted", NULL), 0);
    t_free(pak);
}

static void test_open_rejects_unsupported_method(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("zip_bz")), 0);
    char *pak = under_scratch("zip_bz/bz.pk3");

    const char  *name    = "a.txt";
    const char  *payload = "x";
    zip_single_t p       = {0};
    p.name               = name;
    p.name_len           = strlen(name);
    p.payload            = payload;
    p.payload_len        = 1;
    p.method             = 12; /* bzip2 — not supported */
    EXPECT_EQ(zip_write_single(pak, &p), 0);

    EXPECT_EQ(expect_pakka_rejects(pak, "method", NULL), 0);
    t_free(pak);
}

static void test_open_rejects_stored_csize_neq_usize(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("zip_mismatch")), 0);
    char *pak = under_scratch("zip_mismatch/stored_mismatch.pk3");

    const char  *name    = "a.txt";
    const char  *payload = "xxxxxxxxxx"; /* 10 'x's */
    zip_single_t p       = {0};
    p.name               = name;
    p.name_len           = strlen(name);
    p.payload            = payload;
    p.payload_len        = 10;
    /* method=0 (STORED), csize=10, usize=5 — mismatch. */
    p.csize_override     = 10;
    p.usize_override     = 5;
    p.use_csize_override = 1;
    p.use_usize_override = 1;
    EXPECT_EQ(zip_write_single(pak, &p), 0);

    EXPECT_EQ(expect_pakka_rejects(pak, "csize != usize", NULL), 0);
    t_free(pak);
}

static void test_open_rejects_lfh_payload_overlap_cdr(void)
{
    EXPECT_EQ(fs_mkdir_p(under_scratch("zip_overlap")), 0);
    char *pak = under_scratch("zip_overlap/overlap.pk3");

    /* LFH claims csize=100 but only 1 real byte exists before CDR. */
    const char  *name    = "a.txt";
    const char  *payload = "x";
    zip_single_t p       = {0};
    p.name               = name;
    p.name_len           = strlen(name);
    p.payload            = payload;
    p.payload_len        = 1;
    p.csize_override     = 100;
    p.usize_override     = 100;
    p.use_csize_override = 1;
    p.use_usize_override = 1;
    EXPECT_EQ(zip_write_single(pak, &p), 0);

    /* Pakka should reject with any of csize/CDR-overlap diagnostics. */
    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    proc_result_free(&r);
    t_free(pak);
}

/* ---------- group A: structural list/extract/create against mixed fixture ---------- */

static void test_mixed_list_enumerates_entries(void)
{
    char *pak = build_mixed_pk3_fixture("mixed_list");
    EXPECT_NOT_NULL(pak);
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", pak);
    EXPECT_STR_CONTAINS(r.stdout_buf, "hello.txt");
    EXPECT_STR_CONTAINS(r.stdout_buf, "sub/nested.txt");
    EXPECT_STR_CONTAINS(r.stdout_buf, "lorem.txt");
    proc_result_free(&r);
    t_free(pak);
}

static void test_mixed_list_tree_renders_hierarchy(void)
{
    char *pak = build_mixed_pk3_fixture("mixed_tree");
    EXPECT_NOT_NULL(pak);
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", "--tree", pak);
    EXPECT_STR_CONTAINS(r.stdout_buf, "sub");
    EXPECT_STR_CONTAINS(r.stdout_buf, "nested.txt");
    proc_result_free(&r);
    t_free(pak);
}

static void test_mixed_extract_round_trips_source_tree(void)
{
    char *pak = build_mixed_pk3_fixture("mixed_extract");
    EXPECT_NOT_NULL(pak);
    char *out = under_scratch("mixed_extract/out");
    EXPECT_EQ(fs_mkdir_p(out), 0);
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out, pak);
    proc_result_free(&r);

    char *src = under_scratch("mixed_extract/src");
    EXPECT_EQ(fs_diff_tree(src, out), 0);
    t_free(out);
    t_free(pak);
}

static void test_mixed_extract_selective_by_entry_name(void)
{
    char *pak = build_mixed_pk3_fixture("mixed_sel");
    EXPECT_NOT_NULL(pak);
    char *out = under_scratch("mixed_sel/out");
    EXPECT_EQ(fs_mkdir_p(out), 0);
    proc_result_t r;
    RUN_PAKKA_OK(&r, "-x", "-C", out, pak, "hello.txt");
    proc_result_free(&r);

    char *got    = fs_join(out, "hello.txt");
    char *not_bin   = fs_join(out, "binary.bin");
    char *not_sub   = fs_join(out, "sub");
    EXPECT_TRUE(fs_is_file(got));
    EXPECT_FALSE(fs_is_file(not_bin));
    EXPECT_FALSE(fs_is_dir(not_sub));
    t_free(got); t_free(not_bin); t_free(not_sub);
    t_free(out);
    t_free(pak);
}

static void test_create_pk3_builds_stored_round_trip(void)
{
    char *src = under_scratch("create_stored/src");
    char *d   = fs_join(src, "d");
    EXPECT_EQ(fs_mkdir_p(d), 0);
    char *a = fs_join(src, "a.txt");
    char *b = fs_join(d, "b.txt");
    EXPECT_EQ(fs_write_file(a, "a\n", 2), 0);
    EXPECT_EQ(fs_write_file(b, "b nested\n", 9), 0);

    char         *pak  = under_scratch("create_stored/built.pk3");
    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, "a.txt", "d");
    proc_result_free(&r);

    /* LFH signature PK\003\004 (entries were written, not just EOCD). */
    size_t         pn   = 0;
    unsigned char *pbuf = fs_read_file(pak, &pn);
    EXPECT_NOT_NULL(pbuf);
    EXPECT_TRUE(pn >= 4);
    EXPECT_MEM_EQ(pbuf, "PK\x03\x04", 4);
    /* STORED method on first entry. */
    EXPECT_EQ(zip_first_cdr_method(pbuf, pn), 0);
    t_free(pbuf);

    char *out = under_scratch("create_stored/out");
    EXPECT_EQ(fs_mkdir_p(out), 0);
    RUN_PAKKA_OK(&r, "-x", "-C", out, pak);
    proc_result_free(&r);
    EXPECT_EQ(fs_diff_tree(src, out), 0);

    t_free(a); t_free(b); t_free(d); t_free(src); t_free(pak); t_free(out);
}

static void test_format_returns_pk3_in_process(void)
{
    char *pak = build_mixed_pk3_fixture("format_pk3");
    EXPECT_NOT_NULL(pak);
    pakka_archive_t *a   = NULL;
    pakka_error_t    err = {0};
    pakka_status_t   s   = pakka_open(pak, PAKKA_OPEN_READ, &a, &err);
    EXPECT_EQ(s, PAKKA_OK);
    EXPECT_EQ((int)pakka_format(a), (int)PAKKA_FORMAT_PK3);
    /* Sanity-check the count too: hello.txt + binary.bin + sub/nested.txt + lorem.txt = 4. */
    EXPECT_EQ((long long)pakka_entry_count(a), 4);
    pakka_close(a, NULL);
    t_free(pak);
}

/* ---------- group B: in-process c-api harness (max_decompressed cap,
 *           commit-refuses-changed source, open_entry_handle) ---------- */

static void test_capi_max_decompressed_cap_refuses_oversize(void)
{
    char *pak = build_mixed_pk3_fixture("capi_cap_open");
    EXPECT_NOT_NULL(pak);
    pakka_archive_t *a   = NULL;
    pakka_error_t    err = {0};
    EXPECT_EQ(pakka_open(pak, PAKKA_OPEN_READ, &a, &err), PAKKA_OK);
    EXPECT_EQ(pakka_set_max_decompressed_size(a, 100, &err), PAKKA_OK);

    pakka_reader_t *r = NULL;
    /* lorem.txt is 11.25 KB after DEFLATE — comfortably above the
     * 100-byte cap. */
    EXPECT_EQ(pakka_open_entry(a, "lorem.txt", &r, &err), PAKKA_ERR_LIMIT);
    EXPECT_NULL(r);
    pakka_close(a, NULL);
    t_free(pak);
}

static int g_verify_saw_limit;
static void verify_limit_cb(void *userdata, pakka_report_severity_t sev,
                            pakka_status_t status, const char *entry_name,
                            const char *message)
{
    (void)userdata; (void)sev; (void)entry_name; (void)message;
    if (status == PAKKA_ERR_LIMIT) g_verify_saw_limit = 1;
}

static void test_capi_verify_deep_respects_max_decompressed_cap(void)
{
    char *pak = build_mixed_pk3_fixture("capi_cap_verify");
    EXPECT_NOT_NULL(pak);
    pakka_archive_t *a   = NULL;
    pakka_error_t    err = {0};
    EXPECT_EQ(pakka_open(pak, PAKKA_OPEN_READ, &a, &err), PAKKA_OK);
    EXPECT_EQ(pakka_set_max_decompressed_size(a, 100, &err), PAKKA_OK);

    g_verify_saw_limit = 0;
    pakka_status_t s = pakka_verify(a, PAKKA_VERIFY_DEEP, verify_limit_cb, NULL, &err);
    EXPECT_EQ(s, PAKKA_ERR_LIMIT);
    EXPECT_TRUE(g_verify_saw_limit);
    pakka_close(a, NULL);
    t_free(pak);
}

/* Build a single-entry PK3 (alpha.txt holding "ORIGINAL-BODY\n") with
 * pakka -c so the c-api commit-refuses cases have a target archive to
 * augment via pakka_add_file. Returns absolute pak path; caller frees. */
static char *build_single_entry_pk3(const char *scratch_sub)
{
    char *src = under_scratch(scratch_sub);
    if (fs_mkdir_p(src) != 0) return NULL;
    char *alpha = fs_join(src, "alpha.txt");
    if (fs_write_file(alpha, "ORIGINAL-BODY\n", 14) != 0) return NULL;

    char         *pak  = fs_join(src, "work.pk3");
    proc_opts_t   opts = {0};
    opts.cwd           = src;
    const char   *argv[] = {g_pakka_path, "-c", pak, "alpha.txt", NULL};
    proc_result_t r;
    if (proc_run(argv, &opts, &r) != 0 || r.exit_code != 0) {
        proc_result_free(&r);
        return NULL;
    }
    proc_result_free(&r);
    t_free(alpha);
    return pak;
}

static void test_capi_commit_refuses_changed_pending_source(void)
{
    char *pak = build_single_entry_pk3("capi_changed");
    EXPECT_NOT_NULL(pak);
    char *payload = under_scratch("capi_changed/payload.txt");
    EXPECT_EQ(fs_write_file(payload, "FIRST-BODY1\n", 12), 0);

    pakka_archive_t *a   = NULL;
    pakka_error_t    err = {0};
    EXPECT_EQ(pakka_open(pak, PAKKA_OPEN_READ_WRITE, &a, &err), PAKKA_OK);
    EXPECT_EQ(pakka_add_file(a, payload, "new.txt", &err), PAKKA_OK);

    /* Replace payload bytes in place (same length, different content)
     * before commit reads them back. */
    EXPECT_EQ(fs_write_file(payload, "SECOND-BODY\n", 12), 0);

    pakka_status_t s = pakka_commit(a, &err);
    pakka_close(a, NULL);
    EXPECT_EQ(s, PAKKA_ERR_FORMAT);
    t_free(pak);
}

static void test_capi_commit_refuses_grew_pending_source(void)
{
    char *pak = build_single_entry_pk3("capi_grew");
    EXPECT_NOT_NULL(pak);
    char *payload = under_scratch("capi_grew/payload.txt");
    EXPECT_EQ(fs_write_file(payload, "PREFIX-BODY1", 12), 0);

    pakka_archive_t *a   = NULL;
    pakka_error_t    err = {0};
    EXPECT_EQ(pakka_open(pak, PAKKA_OPEN_READ_WRITE, &a, &err), PAKKA_OK);
    EXPECT_EQ(pakka_add_file(a, payload, "new.txt", &err), PAKKA_OK);

    /* Append bytes — prefix CRC still matches, but the file grew past
     * the recorded length. */
    FILE *fp = fopen(payload, "ab");
    EXPECT_NOT_NULL(fp);
    fputs("-EXTRA", fp);
    fclose(fp);

    pakka_status_t s = pakka_commit(a, &err);
    pakka_close(a, NULL);
    EXPECT_EQ(s, PAKKA_ERR_FORMAT);
    t_free(pak);
}

static void test_capi_open_entry_handle_pending_respects_cap(void)
{
    /* Pending-entry reads must hit the same LIMIT check that the
     * name-keyed open_entry does. Without the cap the handle path
     * could load a multi-GiB payload from a pending source into the
     * inflated buffer. */
    EXPECT_EQ(fs_mkdir_p(under_scratch("handle_cap")), 0);
    char *big = under_scratch("handle_cap/big.txt");
    /* 4 KB — well above the 200-byte cap. */
    unsigned char *buf = (unsigned char *)malloc(4096);
    EXPECT_NOT_NULL(buf);
    memset(buf, 'x', 4096);
    EXPECT_EQ(fs_write_file(big, buf, 4096), 0);
    free(buf);

    char *pak = under_scratch("handle_cap/h.pk3");
    pakka_archive_t *a   = NULL;
    pakka_error_t    err = {0};
    EXPECT_EQ(pakka_create(pak, PAKKA_FORMAT_PK3, 0, &a, &err), PAKKA_OK);
    EXPECT_EQ(pakka_add_file(a, big, "big.txt", &err), PAKKA_OK);
    EXPECT_EQ(pakka_set_max_decompressed_size(a, 200, &err), PAKKA_OK);

    const pakka_entry_t *e = NULL;
    EXPECT_EQ(pakka_find_entry(a, "big.txt", &e), PAKKA_OK);
    EXPECT_NOT_NULL(e);

    pakka_reader_t *r = NULL;
    EXPECT_EQ(pakka_open_entry_handle(a, e, &r, &err), PAKKA_ERR_LIMIT);
    EXPECT_NULL(r);
    pakka_close(a, NULL);
}

static void test_capi_open_entry_handle_skips_name_lookup(void)
{
    char *pak = build_mixed_pk3_fixture("handle_skip");
    EXPECT_NOT_NULL(pak);
    pakka_archive_t *a   = NULL;
    pakka_error_t    err = {0};
    EXPECT_EQ(pakka_open(pak, PAKKA_OPEN_READ, &a, &err), PAKKA_OK);

    /* NULL-tolerance probe. */
    pakka_reader_t *r = NULL;
    EXPECT_EQ(pakka_open_entry_handle(NULL, NULL, &r, &err),
              PAKKA_ERR_INVALID_ARGUMENT);

    const pakka_entry_t *e = NULL;
    EXPECT_EQ(pakka_entry_at(a, 0, &e), PAKKA_OK);
    EXPECT_NOT_NULL(e);
    EXPECT_EQ(pakka_open_entry_handle(a, e, &r, &err), PAKKA_OK);

    char   small[64];
    size_t nread = 0;
    EXPECT_EQ(pakka_reader_read(r, small, sizeof(small), &nread, &err), PAKKA_OK);
    EXPECT_TRUE(nread > 0);
    pakka_reader_close(r);
    pakka_close(a, NULL);
    t_free(pak);
}

int main(void)
{
    const char *pakka = getenv("PAKKA");
    if (!pakka || !*pakka) {
        fprintf(stderr, "pk3_test: PAKKA env var not set\n");
        return 1;
    }
    g_pakka_path = pakka;

    const char *scratch = getenv("PK3_TEST_SCRATCH");
    if (!scratch || !*scratch) scratch = "build/test/pk3";
    fs_rmtree(scratch);
    if (fs_mkdir_p(scratch) != 0) {
        fprintf(stderr, "pk3_test: mkdir(%s) failed\n", scratch);
        return 1;
    }
    g_scratch = strdup(scratch);

    RUN_TEST(test_empty_pk3_is_22_byte_eocd_only);
    RUN_TEST(test_add_duplicate_entry_refused);
    RUN_TEST(test_delete_close_rebuild_produces_valid_pk3);
    RUN_TEST(test_compress_encodes_deflate_on_compressible);
    RUN_TEST(test_compress_falls_back_to_stored_on_incompressible);
    RUN_TEST(test_compress_mixed_archive);
    RUN_TEST(test_compress_round_trip_byte_identical);
    RUN_TEST(test_compress_verify_deep_accepts);
    RUN_TEST(test_compress_rejected_on_pak_target);

    RUN_TEST(test_multi_disk_spanning_marker_rejected);
    RUN_TEST(test_zip64_sentinels_rejected);
    RUN_TEST(test_open_rejects_dotdot_traversal_in_zip);
    RUN_TEST(test_open_rejects_embedded_nul_in_name);
    RUN_TEST(test_open_rejects_zip_entry_with_reserved_name);
    RUN_TEST(test_open_rejects_encrypted_entry);
    RUN_TEST(test_open_rejects_unsupported_method);
    RUN_TEST(test_open_rejects_stored_csize_neq_usize);
    RUN_TEST(test_open_rejects_lfh_payload_overlap_cdr);

    RUN_TEST(test_mixed_list_enumerates_entries);
    RUN_TEST(test_mixed_list_tree_renders_hierarchy);
    RUN_TEST(test_mixed_extract_round_trips_source_tree);
    RUN_TEST(test_mixed_extract_selective_by_entry_name);
    RUN_TEST(test_create_pk3_builds_stored_round_trip);
    RUN_TEST(test_format_returns_pk3_in_process);

    RUN_TEST(test_capi_max_decompressed_cap_refuses_oversize);
    RUN_TEST(test_capi_verify_deep_respects_max_decompressed_cap);
    RUN_TEST(test_capi_commit_refuses_changed_pending_source);
    RUN_TEST(test_capi_commit_refuses_grew_pending_source);
    RUN_TEST(test_capi_open_entry_handle_pending_respects_cap);
    RUN_TEST(test_capi_open_entry_handle_skips_name_lookup);

    t_free(g_scratch);
    return t_summary();
}
