/*
 * Synthesize a PAK or PK3 archive with N entries for the bench harness.
 *
 * Used by dev/bench/run.sh to produce reproducible, sized-on-demand
 * inputs that exercise the asymptotic behavior of list / extract /
 * verify well past what real-world fixtures reach (Q3 demo tops out
 * around 1300 entries; the bench wants 10k and 50k).
 *
 * Shell-expanded `pakka -c <new.pak> path1 path2 ...` overflows ARG_MAX
 * at this scale (~256 KiB on most hosts), so we go directly through
 * libpakka: pakka_create + pakka_add_memory in a loop. Names are
 * deterministic ("bench/00000001.dat") so the on-disk archive is
 * byte-stable across runs — cache once, reuse forever.
 *
 * Usage:
 *   synth_tool <format> <count> <output_path>
 *
 *   format       pak | pk3
 *   count        positive integer (capped at PAKFILE_MAX_ENTRIES /
 *                ZIP non-ZIP64 65535 by libpakka)
 *   output_path  must not already exist (pakka_create refuses overwrite)
 *
 * Build:
 *   make bench   (or directly: make build/bench/synth_tool)
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pakka.h"

static void die(const char *fmt, const char *arg) {
    fprintf(stderr, "synth_tool: ");
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    exit(1);
}

static void die_err(const char *op, const pakka_error_t *err) {
    fprintf(stderr, "synth_tool: %s: %s\n", op, err->message);
    exit(1);
}

int main(int argc, char **argv) {
    /* Init to AUTO so clang's flow analysis sees a definite assignment
     * even though die() exits on the unknown-format path. AUTO is
     * never actually used — the dispatch below overwrites it or aborts. */
    pakka_format_t format = PAKKA_FORMAT_AUTO;
    long count_long;
    uint32_t count, i;
    const char *out_path;
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s;
    char name[32];
    /* 16-byte payload per entry: tiny enough that 50k entries stay
     * well under the 4 GiB PAK / ZIP non-ZIP64 ceiling, big enough that
     * extract benchmarks do measurable disk I/O. Content is the index
     * itself so two synth runs with the same N produce identical
     * archives (modulo CRC fields the format computes from this). */
    unsigned char payload[16] = {0};

    if (argc != 4) {
        fprintf(stderr,
                "Usage: %s <pak|pk3> <count> <output_path>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "pak") == 0) {
        format = PAKKA_FORMAT_PAK;
    } else if (strcmp(argv[1], "pk3") == 0) {
        format = PAKKA_FORMAT_PK3;
    } else {
        die("unknown format '%s' (expected pak or pk3)", argv[1]);
    }

    count_long = strtol(argv[2], NULL, 10);
    if (count_long <= 0 || count_long > 1000000) {
        die("count must be 1..1000000, got '%s'", argv[2]);
    }
    count = (uint32_t)count_long;
    out_path = argv[3];

    s = pakka_create(out_path, format, PAKKA_CREATE_DEFAULT, &a, &err);
    if (s != PAKKA_OK) die_err("pakka_create", &err);

    for (i = 0; i < count; i++) {
        snprintf(name, sizeof(name), "bench/%08" PRIu32 ".dat", i);
        memcpy(payload, &i, sizeof(i));
        s = pakka_add_memory(a, name, payload, sizeof(payload), &err);
        if (s != PAKKA_OK) {
            pakka_close(a, NULL);
            die_err("pakka_add_memory", &err);
        }
    }

    s = pakka_close(a, &err);
    if (s != PAKKA_OK) die_err("pakka_close", &err);

    printf("synth_tool: wrote %" PRIu32 " entries to %s\n", count, out_path);
    return 0;
}
