/*
 * libFuzzer harness: feed arbitrary byte streams to the Daikatana
 * custom-codec decoder. The codec has a hand-written opcode table
 * (literal-run / RLE / back-reference / terminator); a missed bounds
 * check would let a crafted stream overflow the output buffer or
 * read past the input. ASan is the oracle.
 *
 * Build:
 *   make fuzz-dk
 * Run:
 *   build/fuzz/pakka_fuzz_dk_inflate dev/fuzz/corpus_dk -max_total_time=60
 */

#include <stdint.h>
#include <stdlib.h>
#include "pakka.h"
/* pakka_dk_inflate is declared in src/common.h, not the public header
 * (it's an internal helper exposed only to in-tree TUs and the
 * dk_codec_test exerciser). The fuzz build adds -Isrc to FUZZ_CPPFLAGS
 * for this include — see the Makefile fuzz rules. */
#include "common.h"

/* Bounded output budget: the codec accepts a uint32 output size, but
 * we don't want libFuzzer's exploration to OOM the host. 1 MiB is
 * plenty to surface overrun + termination bugs. */
#define FUZZ_DK_OUT_CAP (1u << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    unsigned char *out;
    pakka_error_t err;

    if (size > FUZZ_DK_OUT_CAP) return 0;
    out = (unsigned char *)malloc(FUZZ_DK_OUT_CAP);
    if (out == NULL) return 0;

    /* Run the decoder with a fixed output budget. The caller's
     * out_len is also the expected uncompressed size; the codec
     * refuses a partial decode where produced != out_len. We try
     * three out_len values per input: the input size, half of it,
     * and the full budget. Different out_len choices exercise
     * different terminator / exact-length checks. */
    (void)pakka_dk_inflate(data, (uint32_t)size, out, (uint32_t)size, &err);
    (void)pakka_dk_inflate(data, (uint32_t)size, out, (uint32_t)(size / 2), &err);
    (void)pakka_dk_inflate(data, (uint32_t)size, out, FUZZ_DK_OUT_CAP, &err);

    free(out);
    return 0;
}
