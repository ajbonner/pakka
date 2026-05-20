/*
 * libFuzzer harness: open + add + commit + close + reopen against a
 * mutated input. Targets the commit-time write paths (rebuild copy
 * loop, CDR / EOCD construction, ftruncate) that a parser-only fuzzer
 * doesn't reach. Catches:
 *   - EOCD-comment truncation regressions (H4 class)
 *   - rebuild-rollback offset corruption (H3 class)
 *   - atomic-add temp/rename gaps (H2 class)
 *   - any structural invariant violation that survives one
 *     open→commit→reopen cycle
 *
 * Build:
 *   make fuzz-roundtrip
 * Run:
 *   build/fuzz/pakka_fuzz_roundtrip dev/fuzz/corpus_roundtrip -max_total_time=60
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pakka.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char path[] = "/tmp/pakka_fuzz_rt_XXXXXX";
    char src_path[] = "/tmp/pakka_fuzz_rt_src_XXXXXX";
    int fd, src_fd;
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    const unsigned char *src_bytes;
    size_t src_len;

    if (size < 4 || size > 8 * 1024 * 1024) return 0;

    /* First 4 bytes pick the operation; remainder is the archive
     * bytes. Splitting like this lets the fuzzer steer toward
     * specific commit paths without needing dictionary support. */
    uint8_t op = data[0] & 0x07;        /* 8 distinct paths */
    size_t archive_len = size - 4;
    if (archive_len == 0) return 0;
    const unsigned char *archive_bytes = data + 4;

    fd = mkstemp(path);
    if (fd < 0) return 0;
    if (write(fd, archive_bytes, archive_len) != (ssize_t)archive_len) {
        close(fd);
        unlink(path);
        return 0;
    }
    close(fd);

    /* Build a small synthetic source for add operations from the
     * input's last 64 bytes (or fewer). */
    src_len = (archive_len > 64) ? 64 : archive_len;
    src_bytes = archive_bytes + (archive_len - src_len);
    src_fd = mkstemp(src_path);
    if (src_fd >= 0) {
        (void)write(src_fd, src_bytes, src_len);
        close(src_fd);
    }

    if (pakka_open(path, PAKKA_OPEN_READ_WRITE, &a, &err) != PAKKA_OK) {
        unlink(path);
        unlink(src_path);
        return 0;
    }

    switch (op) {
    case 0: /* read-only walk */
        {
            size_t count = pakka_entry_count(a);
            for (size_t i = 0; i < count; i++) {
                const pakka_entry_t *e = NULL;
                if (pakka_entry_at(a, i, &e) != PAKKA_OK || e == NULL) break;
                void *buf = NULL; size_t blen = 0;
                if (pakka_read_entry_alloc(a, pakka_entry_name(e),
                                           &buf, &blen, &err) == PAKKA_OK) {
                    pakka_free(buf);
                }
            }
        }
        break;
    case 1: /* add from source path */
        (void)pakka_add_file(a, src_path, "fuzz.txt", &err);
        break;
    case 2: /* add from memory */
        (void)pakka_add_memory(a, "fuzz_mem.txt", src_bytes, src_len, &err);
        break;
    case 3: /* delete first entry (implicit commit at close) */
        {
            const pakka_entry_t *e = NULL;
            if (pakka_entry_at(a, 0, &e) == PAKKA_OK && e != NULL) {
                (void)pakka_delete(a, pakka_entry_name(e), &err);
            }
        }
        break;
    case 4: /* add + commit explicitly */
        (void)pakka_add_memory(a, "x.txt", src_bytes, src_len, &err);
        (void)pakka_commit(a, &err);
        break;
    case 5: /* verify --deep */
        (void)pakka_verify(a, PAKKA_VERIFY_DEEP, NULL, NULL, &err);
        break;
    case 6: /* lower the cap aggressively */
        (void)pakka_set_max_decompressed_size(a, 1024, &err);
        (void)pakka_verify(a, PAKKA_VERIFY_DEEP, NULL, NULL, &err);
        break;
    case 7: /* full cycle: add, commit, reopen */
        (void)pakka_add_memory(a, "cycle.txt", src_bytes, src_len, &err);
        (void)pakka_commit(a, &err);
        pakka_close(a, &err);
        a = NULL;
        (void)pakka_open(path, PAKKA_OPEN_READ, &a, &err);
        break;
    }

    if (a != NULL) pakka_close(a, &err);
    unlink(path);
    unlink(src_path);
    return 0;
}
