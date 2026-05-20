/*
 * libFuzzer harness: feed arbitrary bytes to pakka_open() and touch
 * every entry's metadata accessor. Targets the parse path —
 * directory / EOCD scan, per-entry CDR record validation, magic
 * dispatch, entry allocation — where ASan catches heap overflows,
 * out-of-bounds reads, and missed bounds checks. NOT a payload-read
 * fuzzer: the accessor calls only touch in-memory struct fields,
 * so a corrupt offset that passes open-time validation but blows up
 * at read time won't surface here. The roundtrip harness's case 0
 * exercises the streaming reader for that.
 *
 * Build:
 *   make fuzz-open
 * Run with the bundled corpus:
 *   build/fuzz/pakka_fuzz_open dev/fuzz/corpus_open -max_total_time=60
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pakka.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char path[] = "/tmp/pakka_fuzz_open_XXXXXX";
    int fd;
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s;
    size_t i, count;

    /* libFuzzer doesn't bound input by default; cap to a sane size
     * so a pathological 1 GiB input doesn't OOM the host. The real
     * 4 GiB cap is enforced inside pakka_open. */
    if (size > 16 * 1024 * 1024) return 0;

    fd = mkstemp(path);
    if (fd < 0) return 0;
    if (write(fd, data, size) != (ssize_t)size) {
        close(fd);
        unlink(path);
        return 0;
    }
    close(fd);

    s = pakka_open(path, PAKKA_OPEN_READ, &a, &err);
    if (s == PAKKA_OK) {
        count = pakka_entry_count(a);
        for (i = 0; i < count; i++) {
            const pakka_entry_t *e = NULL;
            if (pakka_entry_at(a, i, &e) == PAKKA_OK && e != NULL) {
                /* Touch every accessor — reads in-memory fields that
                 * the parser populated. Surfaces UBSan / ASan issues
                 * in the directory walk that built the entry node. */
                (void)pakka_entry_name(e);
                (void)pakka_entry_size(e);
                (void)pakka_entry_offset(e);
            }
        }
        pakka_close(a, &err);
    }
    unlink(path);
    return 0;
}
