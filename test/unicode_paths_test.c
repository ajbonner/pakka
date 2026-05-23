/* unicode_paths_test — non-ASCII path handling.
 *
 * Tests pakka's UTF-8 ↔ UTF-16 conversion at the Windows syscall
 * boundary (wmain wrapper, ZIP GPBF bit 11, CP437 fallback, invalid-
 * UTF-8 extract substitution) plus the POSIX path where the host
 * treats UTF-8 bytes opaquely. Both POSIX and Windows correctness
 * matter — pakka's wmain receives wchar_t* argv on Windows; this
 * binary passes UTF-8 byte sequences through proc.c, which converts
 * to UTF-16 via MultiByteToWideChar(CP_UTF8, ...) before CreateProcessW. */

#include "fs.h"
#include "proc.h"
#include "test_macros.h"
#include "zip_build.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAK_HEADER_SIZE 12
#define PAK_NAME_FIELD  56
#define PAK_DIR_ENTRY   64

/* UTF-8 bytes for "Тест.txt" (Cyrillic "Test.txt"):
 *   Т U+0422 → D0 A2
 *   е U+0435 → D0 B5
 *   с U+0441 → D1 81
 *   т U+0442 → D1 82
 */
static const char CYR_UTF8[] = "\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82.txt";
/* The UTF-8 prefix bytes alone (no .txt suffix), for grep-style assertions. */
static const unsigned char CYR_UTF8_PREFIX[] = {0xD0, 0xA2, 0xD0, 0xB5, 0xD1, 0x81, 0xD1, 0x82};

/* CP1251 bytes for the same characters — NOT valid UTF-8: */
static const unsigned char CYR_CP1251_BYTES[] = {0xD2, 0xE5, 0xF1, 0xF2, '.', 't', 'x', 't'};

/* A second CP1251 4-byte name that also fails UTF-8 validation
 * ("АБВГ.txt"). Both legacy names sanitize to "____.txt" — used for
 * the post-sanitization collision test. */
static const unsigned char CYR_CP1251_BYTES_B[] = {0xC0, 0xC1, 0xC2, 0xC3, '.', 't', 'x', 't'};

static const char *g_pakka_path;
static char       *g_scratch;

static char *under_scratch(const char *sub) { return (char *)t_track(fs_join(g_scratch, sub)); }

static void put_u32_le(unsigned char *buf, size_t off, uint32_t v)
{
    buf[off + 0] = (unsigned char)(v & 0xFF);
    buf[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    buf[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    buf[off + 3] = (unsigned char)((v >> 24) & 0xFF);
}

/* Minimal PAK with one zero-length entry whose name is the given raw
 * bytes. */
static int write_pak_one_entry_bytes(const char *path, const void *name,
                                     size_t name_len)
{
    if (name_len > PAK_NAME_FIELD) return -1;
    unsigned char header[PAK_HEADER_SIZE];
    memcpy(header, "PACK", 4);
    put_u32_le(header, 4, PAK_HEADER_SIZE);
    put_u32_le(header, 8, PAK_DIR_ENTRY);

    unsigned char dir[PAK_DIR_ENTRY] = {0};
    if (name_len > 0) memcpy(dir, name, name_len);
    put_u32_le(dir, PAK_NAME_FIELD, 76);
    put_u32_le(dir, PAK_NAME_FIELD + 4, 0);

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        fwrite(dir, 1, sizeof(dir), f) != sizeof(dir)) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Two-entry PAK with both entries' name fields holding raw bytes. */
static int write_pak_two_entries(const char *path,
                                 const void *name_a, size_t alen,
                                 const void *name_b, size_t blen)
{
    if (alen > PAK_NAME_FIELD || blen > PAK_NAME_FIELD) return -1;
    unsigned char header[PAK_HEADER_SIZE];
    memcpy(header, "PACK", 4);
    put_u32_le(header, 4, PAK_HEADER_SIZE);
    put_u32_le(header, 8, PAK_DIR_ENTRY * 2);

    unsigned char dir_a[PAK_DIR_ENTRY] = {0};
    unsigned char dir_b[PAK_DIR_ENTRY] = {0};
    if (alen) memcpy(dir_a, name_a, alen);
    if (blen) memcpy(dir_b, name_b, blen);
    put_u32_le(dir_a, PAK_NAME_FIELD, 140);
    put_u32_le(dir_b, PAK_NAME_FIELD, 140);

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

/* Find a byte sequence in a buffer. Returns offset or -1. */
static long find_bytes(const unsigned char *buf, size_t buf_len,
                       const unsigned char *needle, size_t needle_len)
{
    if (needle_len > buf_len) return -1;
    for (size_t i = 0; i <= buf_len - needle_len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) return (long)i;
    }
    return -1;
}

/* ---------- tests ---------- */

static void test_pak_add_list_round_trips_cyrillic_utf8(void)
{
    char *dir = under_scratch("pak_add_list");
    EXPECT_EQ(fs_mkdir_p(dir), 0);
    char *src = fs_join(dir, CYR_UTF8);
    EXPECT_EQ(fs_write_file(src, "hello", 5), 0);
    char *pak = fs_join(dir, "out.pak");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = dir;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, CYR_UTF8);
    proc_result_free(&r);

    RUN_PAKKA_OK(&r, "-l", pak);
    EXPECT_NOT_NULL(r.stdout_buf);
    /* The UTF-8 prefix bytes must appear verbatim in pakka -l output. */
    EXPECT_TRUE(find_bytes((const unsigned char *)r.stdout_buf, r.stdout_len,
                           CYR_UTF8_PREFIX, sizeof(CYR_UTF8_PREFIX)) >= 0);
    proc_result_free(&r);
    t_free(src);
    t_free(pak);
    t_free(dir);
}

static void test_pak_archive_header_stores_raw_utf8_bytes(void)
{
    char *dir = under_scratch("pak_hdr_utf8");
    EXPECT_EQ(fs_mkdir_p(dir), 0);
    char *src = fs_join(dir, CYR_UTF8);
    EXPECT_EQ(fs_write_file(src, "hello", 5), 0);
    char *pak = fs_join(dir, "out.pak");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = dir;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, CYR_UTF8);
    proc_result_free(&r);

    /* The UTF-8 byte sequence for "Тест" must appear in the pak's
     * directory area (we don't bother parsing diroffset — pakka writes
     * the directory contiguous after the single payload). */
    size_t         pn   = 0;
    unsigned char *pbuf = fs_read_file(pak, &pn);
    EXPECT_NOT_NULL(pbuf);
    EXPECT_TRUE(find_bytes(pbuf, pn, CYR_UTF8_PREFIX, sizeof(CYR_UTF8_PREFIX)) >= 0);
    t_free(pbuf);
    t_free(src);
    t_free(pak);
    t_free(dir);
}

static void test_pak_extract_round_trips_cyrillic_utf8(void)
{
    char *dir = under_scratch("pak_extract_utf8");
    EXPECT_EQ(fs_mkdir_p(dir), 0);
    char *src = fs_join(dir, CYR_UTF8);
    EXPECT_EQ(fs_write_file(src, "hello", 5), 0);
    char *pak = fs_join(dir, "out.pak");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = dir;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, CYR_UTF8);
    proc_result_free(&r);

    char *out = fs_join(dir, "extracted");
    EXPECT_EQ(fs_mkdir_p(out), 0);
    RUN_PAKKA_OK(&r, "-x", "-C", out, pak);
    proc_result_free(&r);

    char *extracted = fs_join(out, CYR_UTF8);
    EXPECT_TRUE(fs_is_file(extracted));
    size_t         n   = 0;
    unsigned char *buf = fs_read_file(extracted, &n);
    EXPECT_EQ((long long)n, 5);
    EXPECT_MEM_EQ(buf, "hello", 5);
    t_free(buf);
    t_free(extracted);
    t_free(out);
    t_free(src);
    t_free(pak);
    t_free(dir);
}

static void test_pk3_gpbf_bit11_set_for_cyrillic(void)
{
    /* pakka writes Cyrillic UTF-8 names with GP bit 11 (0x0800) set in
     * both LFH and CDR. The flags word is u16 LE at LFH+6 / CDR+8;
     * bit 11 corresponds to the high byte's 0x08. */
    char *dir = under_scratch("pk3_bit11_set");
    EXPECT_EQ(fs_mkdir_p(dir), 0);
    char *src = fs_join(dir, CYR_UTF8);
    EXPECT_EQ(fs_write_file(src, "hello", 5), 0);
    char *pak = fs_join(dir, "out.pk3");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = dir;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, CYR_UTF8);
    proc_result_free(&r);

    size_t         pn   = 0;
    unsigned char *pbuf = fs_read_file(pak, &pn);
    EXPECT_NOT_NULL(pbuf);
    EXPECT_TRUE(pn >= 30);
    /* LFH starts at offset 0 (pakka writes no pre-header data). */
    EXPECT_MEM_EQ(pbuf, "PK\x03\x04", 4);
    EXPECT_EQ((long long)pbuf[6], 0x00); /* low byte */
    EXPECT_EQ((long long)pbuf[7], 0x08); /* high byte: bit 11 set */

    /* Locate the CDR signature ("PK\x01\x02") and check GP flags at CDR+8. */
    const unsigned char cdr_sig[] = {'P', 'K', 0x01, 0x02};
    long cdr_off = find_bytes(pbuf, pn, cdr_sig, sizeof(cdr_sig));
    EXPECT_TRUE(cdr_off >= 0);
    EXPECT_EQ((long long)pbuf[cdr_off + 8],  0x00);
    EXPECT_EQ((long long)pbuf[cdr_off + 9],  0x08);
    t_free(pbuf);
    t_free(src);
    t_free(pak);
    t_free(dir);
}

static void test_pk3_gpbf_bit11_clear_for_ascii(void)
{
    char *dir = under_scratch("pk3_bit11_clear");
    EXPECT_EQ(fs_mkdir_p(dir), 0);
    char *src = fs_join(dir, "plain.txt");
    EXPECT_EQ(fs_write_file(src, "hello", 5), 0);
    char *pak = fs_join(dir, "out.pk3");

    proc_result_t r;
    proc_opts_t   opts = {0};
    opts.cwd           = dir;
    RUN_PAKKA_OK_CWD(&r, &opts, "-c", pak, "plain.txt");
    proc_result_free(&r);

    size_t         pn   = 0;
    unsigned char *pbuf = fs_read_file(pak, &pn);
    EXPECT_NOT_NULL(pbuf);
    EXPECT_TRUE(pn >= 8);
    /* GP flags word at LFH+6 must be entirely zero (no bit 11). */
    EXPECT_EQ((long long)pbuf[6], 0x00);
    EXPECT_EQ((long long)pbuf[7], 0x00);
    t_free(pbuf);
    t_free(src);
    t_free(pak);
    t_free(dir);
}

static void test_legacy_pak_extract_substitutes_invalid_utf8(void)
{
    /* Build a PAK whose entry name is raw CP1251 bytes — invalid UTF-8.
     * Extract should sanitize to "____.txt" (4 invalid bytes → 4 '_')
     * and warn rather than silently overwrite. */
    char *dir = under_scratch("legacy_sub");
    EXPECT_EQ(fs_mkdir_p(dir), 0);
    char *pak = fs_join(dir, "legacy.pak");
    EXPECT_EQ(write_pak_one_entry_bytes(pak, CYR_CP1251_BYTES,
                                        sizeof(CYR_CP1251_BYTES)), 0);

    char *out = fs_join(dir, "out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    proc_result_t r;
    /* Extract succeeds (warning, not error). */
    RUN_PAKKA_OK(&r, "-x", "-C", out, pak);
    /* The diagnostic must mention "not valid UTF-8". */
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && strstr(streams[s], "not valid UTF-8")) found = 1;
    }
    if (!found) {
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        proc_result_free(&r);
        t_free(pak);
        t_free(out);
        t_free(dir);
        FAIL("expected 'not valid UTF-8' diagnostic");
    }
    proc_result_free(&r);

    /* "____.txt" must exist. */
    char *substituted = fs_join(out, "____.txt");
    EXPECT_TRUE(fs_is_file(substituted));
    t_free(substituted);
    t_free(pak);
    t_free(out);
    t_free(dir);
}

static void test_list_invalid_utf8_renders_sanitized(void)
{
    /* pakka_fprint_sanitized must ?-substitute invalid UTF-8 byte runs
     * so the listing line is pure printable ASCII (no embedded escape
     * sequences that could corrupt a TTY). */
    char *dir = under_scratch("list_san");
    EXPECT_EQ(fs_mkdir_p(dir), 0);
    char *pak = fs_join(dir, "legacy.pak");
    EXPECT_EQ(write_pak_one_entry_bytes(pak, CYR_CP1251_BYTES,
                                        sizeof(CYR_CP1251_BYTES)), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", pak);
    EXPECT_NOT_NULL(r.stdout_buf);
    /* Every byte in stdout must be printable ASCII (0x20..0x7E) or
     * '\n' / '\r' / '\t'. No raw CP1251 high bytes (0xD2 etc.) must
     * appear — that's the whole point of sanitization. */
    for (size_t i = 0; i < r.stdout_len; i++) {
        unsigned char c = (unsigned char)r.stdout_buf[i];
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 0x20 || c > 0x7E) {
            fprintf(stderr, "    unsanitized byte 0x%02X at offset %zu\n", c, i);
            proc_result_free(&r);
            t_free(pak);
            t_free(dir);
            FAIL("pakka -l output contains non-printable / non-ASCII byte");
        }
    }
    proc_result_free(&r);
    t_free(pak);
    t_free(dir);
}

static void test_pk3_bit11_set_with_invalid_utf8_rejected(void)
{
    /* CP1251 bytes "Тест.txt" — definitely not valid UTF-8 — packaged
     * in a PK3 with GP bit 11 (0x0800) set, advertising UTF-8.
     * pakka must reject the open and emit a "UTF-8 flag" /
     * "not valid UTF-8" diagnostic. */
    EXPECT_EQ(fs_mkdir_p(under_scratch("pk3_bit11_bad")), 0);
    char *pak = fs_join(under_scratch("pk3_bit11_bad"), "bogus.pk3");

    zip_single_t p = {0};
    p.name         = CYR_CP1251_BYTES;
    p.name_len     = sizeof(CYR_CP1251_BYTES);
    p.payload      = NULL;
    p.payload_len  = 0;
    p.gp_flags     = 0x0800;
    EXPECT_EQ(zip_write_single(pak, &p), 0);

    const char   *argv[] = {g_pakka_path, "-l", pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && (strstr(streams[s], "UTF-8 flag") ||
                           strstr(streams[s], "not valid UTF-8"))) {
            found = 1;
        }
    }
    if (!found) {
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        proc_result_free(&r);
        t_free(pak);
        FAIL("expected 'UTF-8 flag' / 'not valid UTF-8' diagnostic");
    }
    proc_result_free(&r);
    t_free(pak);
}

static void test_pk3_bit11_clear_cp437_decodes_via_fallback(void)
{
    /* 0xE1 in CP437 is U+00DF (ß), which UTF-8-encodes as 0xC3 0x9F.
     * With GP bit 11 CLEAR, pakka decodes the name via the CP437
     * fallback table and renders it as UTF-8 in the listing output. */
    EXPECT_EQ(fs_mkdir_p(under_scratch("pk3_cp437")), 0);
    char *pak = fs_join(under_scratch("pk3_cp437"), "cp437.pk3");

    unsigned char name_byte = 0xE1;
    zip_single_t  p         = {0};
    p.name                  = &name_byte;
    p.name_len              = 1;
    p.payload               = NULL;
    p.payload_len           = 0;
    p.gp_flags              = 0x0000;
    EXPECT_EQ(zip_write_single(pak, &p), 0);

    proc_result_t r;
    RUN_PAKKA_OK(&r, "-l", pak);
    /* UTF-8 encoding of ß: 0xC3 0x9F. Must appear in the listing. */
    unsigned char eszett_utf8[] = {0xC3, 0x9F};
    EXPECT_TRUE(find_bytes((const unsigned char *)r.stdout_buf, r.stdout_len,
                           eszett_utf8, sizeof(eszett_utf8)) >= 0);
    proc_result_free(&r);
    t_free(pak);
}

static void test_legacy_pak_collision_after_sanitization_detected(void)
{
    /* Two CP1251 names that both sanitize to "____.txt" — different
     * raw bytes, same post-substitution form. The collision check must
     * fire (refuse the extract) rather than silently letting the
     * second entry overwrite the first. */
    char *dir = under_scratch("collision");
    EXPECT_EQ(fs_mkdir_p(dir), 0);
    char *pak = fs_join(dir, "legacy.pak");
    EXPECT_EQ(write_pak_two_entries(pak,
                                    CYR_CP1251_BYTES,   sizeof(CYR_CP1251_BYTES),
                                    CYR_CP1251_BYTES_B, sizeof(CYR_CP1251_BYTES_B)), 0);

    char *out = fs_join(dir, "out");
    EXPECT_EQ(fs_mkdir_p(out), 0);

    const char   *argv[] = {g_pakka_path, "-x", "-C", out, pak, NULL};
    proc_result_t r;
    EXPECT_EQ(run_pakka_capture(&r, argv), 0);
    EXPECT_NE(r.exit_code, 0);
    int found = 0;
    const char *streams[2] = {r.stdout_buf, r.stderr_buf};
    for (int s = 0; s < 2; s++) {
        if (streams[s] && strstr(streams[s], "collide")) found = 1;
    }
    if (!found) {
        fprintf(stderr, "    stdout: %s\n    stderr: %s\n",
                r.stdout_buf ? r.stdout_buf : "",
                r.stderr_buf ? r.stderr_buf : "");
        proc_result_free(&r);
        t_free(pak);
        t_free(out);
        t_free(dir);
        FAIL("expected 'collide' diagnostic on post-sanitization collision");
    }
    proc_result_free(&r);
    t_free(pak);
    t_free(out);
    t_free(dir);
}

int main(void)
{
    const char *pakka = getenv("PAKKA");
    if (!pakka || !*pakka) {
        fprintf(stderr, "unicode_paths_test: PAKKA env var not set\n");
        return 1;
    }
    g_pakka_path = pakka;

    const char *scratch = getenv("UNICODE_TEST_SCRATCH");
    if (!scratch || !*scratch) scratch = "build/test/unicode";
    fs_rmtree(scratch);
    if (fs_mkdir_p(scratch) != 0) {
        fprintf(stderr, "unicode_paths_test: mkdir(%s) failed\n", scratch);
        return 1;
    }
    g_scratch = strdup(scratch);

    RUN_TEST(test_pak_add_list_round_trips_cyrillic_utf8);
    RUN_TEST(test_pak_archive_header_stores_raw_utf8_bytes);
    RUN_TEST(test_pak_extract_round_trips_cyrillic_utf8);
    RUN_TEST(test_pk3_gpbf_bit11_set_for_cyrillic);
    RUN_TEST(test_pk3_gpbf_bit11_clear_for_ascii);
    RUN_TEST(test_legacy_pak_extract_substitutes_invalid_utf8);
    RUN_TEST(test_list_invalid_utf8_renders_sanitized);
    RUN_TEST(test_legacy_pak_collision_after_sanitization_detected);
    RUN_TEST(test_pk3_bit11_set_with_invalid_utf8_rejected);
    RUN_TEST(test_pk3_bit11_clear_cp437_decodes_via_fallback);

    t_free(g_scratch);
    return t_summary();
}
