/* large_file_test — 32-bit fseek/ftell ceiling regression.
 *
 * Synthesizes a sparse pak whose directory sits above LONG_MAX so the
 * wide-seek path actually fires on 32-bit hosts. Mirrors what
 * large_file.bats does with truncate(1) + dd, but without depending on
 * those binaries — the test does its own fseeko / _fseeki64 writes. */

#include "fs.h"
#include "proc.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <winioctl.h>
#endif

/* Both offsets above LONG_MAX (2,147,483,647) so the wide-seek path
 * exercises on 32-bit hosts. Bytes stored little-endian. */
#define LARGE_FILE_SIZE   2600000000LL
#define LARGE_DIR_OFFSET  2500000000LL

static const char *g_pakka_path;
static char       *g_large_pak;
static char       *g_out_dir;

/* Portable signed 64-bit seek. fseeko is POSIX; MSVC has _fseeki64. */
static int seek_to(FILE *f, long long off)
{
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

static int write_at(FILE *f, long long off, const void *data, size_t n)
{
    if (seek_to(f, off) != 0) {
        return -1;
    }
    return fwrite(data, 1, n, f) == n ? 0 : -1;
}

static int make_large_pak(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }

#ifdef _WIN32
    /* NTFS does not auto-sparse fwrite-past-EOF; the gap between
     * offset 12 and LARGE_DIR_OFFSET would otherwise be physically
     * zero-filled (≈2.5 GB of writes per run). FSCTL_SET_SPARSE marks
     * the file before any writes so the holes stay sparse. Best-effort
     * — the test still works if FSCTL_SET_SPARSE fails (e.g. on FAT),
     * it just burns disk. */
    {
        HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
        if (h != INVALID_HANDLE_VALUE) {
            DWORD ignored;
            DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                            &ignored, NULL);
        }
    }
#endif

    /* Header at offset 0: "PACK" + diroffset LE u32 + dirlength LE u32.
     * 2,500,000,000 = 0x95'02'F9'00 → LE bytes 00 F9 02 95.
     * 64 = 0x40 → LE bytes 40 00 00 00. */
    static const unsigned char header[] = {
        'P', 'A', 'C', 'K',
        0x00, 0xF9, 0x02, 0x95,
        0x40, 0x00, 0x00, 0x00,
    };
    if (write_at(f, 0, header, sizeof(header)) != 0) {
        fclose(f);
        return -1;
    }

    /* Payload "data" (4 bytes) at offset 12. */
    if (write_at(f, 12, "data", 4) != 0) {
        fclose(f);
        return -1;
    }

    /* 64-byte directory entry at LARGE_DIR_OFFSET:
     *   bytes  0..55  filename "tiny" + NUL padding (56-byte field)
     *   bytes 56..59  entry offset = 12 LE
     *   bytes 60..63  entry length = 4 LE */
    unsigned char dir[64] = {0};
    memcpy(dir, "tiny", 4);
    dir[56] = 0x0C; /* offset = 12 */
    dir[60] = 0x04; /* length = 4 */
    if (write_at(f, LARGE_DIR_OFFSET, dir, sizeof(dir)) != 0) {
        fclose(f);
        return -1;
    }

    /* Extend the file to LARGE_FILE_SIZE so pakka sees the same
     * trailing hole the bats fixture has. fwriting one byte at
     * (SIZE - 1) plus a sparse-friendly filesystem keeps the disk
     * footprint near zero on ext4 / APFS / NTFS. */
    if (seek_to(f, LARGE_FILE_SIZE - 1) != 0) {
        fclose(f);
        return -1;
    }
    if (fputc(0, f) == EOF) {
        fclose(f);
        return -1;
    }

    return fclose(f) == 0 ? 0 : -1;
}

static void test_lists_single_high_offset_entry(void)
{
    const char   *argv[] = {g_pakka_path, "-l", g_large_pak, NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, NULL, &r), 0);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ((long long)r.line_count, 1);
    EXPECT_STR_STARTS_WITH(r.lines[0], "tiny ");
    proc_result_free(&r);
}

static void test_extract_recovers_payload(void)
{
    EXPECT_EQ(fs_mkdir_p(g_out_dir), 0);

    const char *argv[] = {g_pakka_path, "-x", "-C", g_out_dir, g_large_pak, NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, NULL, &r), 0);
    EXPECT_EQ(r.exit_code, 0);

    char *tiny_path = fs_join(g_out_dir, "tiny");
    EXPECT_NOT_NULL(tiny_path);
    EXPECT_TRUE(fs_is_file(tiny_path));

    size_t         len = 0;
    unsigned char *buf = fs_read_file(tiny_path, &len);
    EXPECT_NOT_NULL(buf);
    EXPECT_EQ((long long)len, 4);
    EXPECT_MEM_EQ(buf, "data", 4);

    free(buf);
    free(tiny_path);
    proc_result_free(&r);
}

static void test_verify_passes(void)
{
    const char   *argv[] = {g_pakka_path, "--verify", g_large_pak, NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, NULL, &r), 0);
    EXPECT_EQ(r.exit_code, 0);
    proc_result_free(&r);
}

int main(void)
{
    const char *pakka = getenv("PAKKA");
    if (!pakka || !*pakka) {
        fprintf(stderr, "large_file_test: PAKKA env var not set\n");
        return 1;
    }
    g_pakka_path = pakka;

    const char *scratch = getenv("LARGE_FILE_SCRATCH");
    if (!scratch || !*scratch) {
        scratch = "build/test/large_file";
    }
    if (fs_mkdir_p(scratch) != 0) {
        fprintf(stderr, "large_file_test: mkdir(%s) failed\n", scratch);
        return 1;
    }
    g_large_pak = fs_join(scratch, "large.pak");
    g_out_dir   = fs_join(scratch, "out");

    fprintf(stdout, "==> synthesizing %s (sparse, %lld bytes)\n",
            g_large_pak, LARGE_FILE_SIZE);
    fflush(stdout);
    if (make_large_pak(g_large_pak) != 0) {
        fprintf(stderr, "large_file_test: make_large_pak(%s) failed\n", g_large_pak);
        return 1;
    }

    RUN_TEST(test_lists_single_high_offset_entry);
    RUN_TEST(test_extract_recovers_payload);
    RUN_TEST(test_verify_passes);

    free(g_large_pak);
    free(g_out_dir);
    return t_summary();
}
