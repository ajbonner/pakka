/* DK codec internals test. Reaches pakka_dk_inflate() and
 * pakka_dk_deflate() through src/common.h (the public surface
 * exposes neither). Decoder coverage: every opcode class plus the
 * bounds and termination contracts. Encoder coverage: worst-case
 * bound + round-trip integrity through the decoder. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static int failures = 0;

/* Decode `in` and verify the result equals `expected`. */
static void roundtrip(const char *label,
                      const unsigned char *in, size_t in_len,
                      const unsigned char *expected, size_t expected_len) {
    unsigned char *out;
    pakka_error_t err;
    pakka_status_t s;

    out = (expected_len > 0) ? calloc(1, expected_len) : NULL;
    s = pakka_dk_inflate(in, in_len, out, expected_len, &err);
    if (s != PAKKA_OK) {
        fprintf(stderr, "FAIL: %s: status=%d msg=%s\n",
                label, (int)s, err.message);
        failures++;
        free(out);
        return;
    }
    if (expected_len > 0 && memcmp(out, expected, expected_len) != 0) {
        fprintf(stderr, "FAIL: %s: byte mismatch\n", label);
        failures++;
    } else {
        printf("ok: %s\n", label);
    }
    free(out);
}

/* Decode `in` and verify that it fails with PAKKA_ERR_FORMAT. */
static void reject(const char *label,
                   const unsigned char *in, size_t in_len,
                   size_t out_len) {
    unsigned char *out = (out_len > 0) ? calloc(1, out_len) : NULL;
    pakka_error_t err;
    pakka_status_t s = pakka_dk_inflate(in, in_len, out, out_len, &err);
    if (s == PAKKA_OK) {
        fprintf(stderr, "FAIL: %s: expected error, got OK\n", label);
        failures++;
    } else if (s != PAKKA_ERR_FORMAT) {
        fprintf(stderr,
                "FAIL: %s: expected PAKKA_ERR_FORMAT, got status=%d (%s)\n",
                label, (int)s, err.message);
        failures++;
    } else {
        printf("ok: %s (rejected: %s)\n", label, err.message);
    }
    free(out);
}

int main(void) {
    /* Opcode b < 64: literal run of (b+1) bytes. */
    {
        const unsigned char in[]       = { 0, 'A', 0xFF };          /* b=0 -> 1 byte */
        const unsigned char expected[] = { 'A' };
        roundtrip("opcode 0: single-byte literal",
                  in, sizeof(in), expected, sizeof(expected));
    }
    {
        const unsigned char in[]       = { 63,
            'a','b','c','d','e','f','g','h','i','j',
            'k','l','m','n','o','p','q','r','s','t',
            'u','v','w','x','y','z','A','B','C','D',
            'E','F','G','H','I','J','K','L','M','N',
            'O','P','Q','R','S','T','U','V','W','X',
            'Y','Z','0','1','2','3','4','5','6','7',
            '8','9','+','/',
            0xFF };
        unsigned char expected[64];
        memcpy(expected, in + 1, 64);
        roundtrip("opcode 63: 64-byte literal", in, sizeof(in), expected, 64);
    }

    /* Opcode b in [64, 128): zero-run of (b - 62) bytes. b=64 -> 2 zeros. */
    {
        const unsigned char in[]       = { 64, 0xFF };
        const unsigned char expected[] = { 0, 0 };
        roundtrip("opcode 64: 2 zero bytes", in, sizeof(in), expected, 2);
    }
    {
        const unsigned char in[]       = { 127, 0xFF };
        unsigned char expected[65] = {0};       /* b=127 -> 65 zeros */
        roundtrip("opcode 127: 65 zero bytes", in, sizeof(in), expected, 65);
    }

    /* Opcode b in [128, 192): byte-run of (b - 126) copies of next byte. */
    {
        const unsigned char in[]       = { 128, 'Q', 0xFF };
        const unsigned char expected[] = { 'Q', 'Q' };
        roundtrip("opcode 128: 2x next byte", in, sizeof(in), expected, 2);
    }

    /* Opcode b in [192, 254): back-reference of (b - 190) bytes from
     * (next_byte + 2) positions back in OUTPUT. */
    {
        /* Step 1: literal "ab". Step 2: back-ref of 2 bytes at
         * distance 2 — should reproduce "ab" so output is "abab". */
        const unsigned char in[]       = {
            1, 'a', 'b',                /* opcode 1 = literal of 2 */
            192, 0,                     /* opcode 192 = back-ref of 2;
                                           next byte=0 -> distance=2 */
            0xFF
        };
        const unsigned char expected[] = { 'a','b','a','b' };
        roundtrip("opcode 192: 2-byte back-ref at distance 2",
                  in, sizeof(in), expected, sizeof(expected));
    }
    {
        /* Minimum-distance back-ref (2). After literal "ab", a 3-byte
         * back-ref at distance 2 (d=0) walks the byte-by-byte copy
         * forward into freshly-written output, producing "aba" so the
         * total is "ababa". memcpy/memmove with overlap would corrupt
         * this — keep the byte-by-byte impl. */
        const unsigned char in[]       = {
            1, 'a', 'b',
            193, 0,
            0xFF
        };
        const unsigned char expected[] = { 'a','b','a','b','a' };
        roundtrip("opcode 193: 3-byte overlap back-ref at distance 2",
                  in, sizeof(in), expected, sizeof(expected));
    }

    /* Opcode 254: invalid. */
    {
        const unsigned char in[] = { 254, 0xFF };
        reject("opcode 254: invalid", in, sizeof(in), 0);
    }

    /* Termination via input exhaustion (no 0xFF). pakextract accepts
     * this so long as produced == out_len. */
    {
        const unsigned char in[]       = { 0, 'Z' };  /* literal 1, no terminator */
        const unsigned char expected[] = { 'Z' };
        roundtrip("input-exhaustion termination (no 0xFF)",
                  in, sizeof(in), expected, sizeof(expected));
    }

    /* Early terminator with output under-fill must reject. */
    {
        const unsigned char in[] = { 0xFF };
        reject("early terminator with unfilled output",
               in, sizeof(in), 5);
    }

    /* Literal run overruns input. */
    {
        const unsigned char in[] = { 5, 'a', 'b' };   /* opcode 5 -> need 6 literal bytes */
        reject("literal run overruns input", in, sizeof(in), 6);
    }

    /* Back-ref underflow: distance > produced. */
    {
        const unsigned char in[] = { 192, 10, 0xFF }; /* nothing produced yet, dist=12 */
        reject("back-ref distance exceeds produced", in, sizeof(in), 2);
    }

    /* Back-ref overruns output. */
    {
        const unsigned char in[] = {
            1, 'a', 'b',
            193, 0,             /* 3 bytes at dist 2 */
            0xFF
        };
        /* Output buffer is only 4 bytes; need 5. */
        reject("back-ref overruns output", in, sizeof(in), 4);
    }

    /* Zero-run on empty payload: opcode 64 wants 2 zero bytes; if out
     * buffer is 1, that's overrun. */
    {
        const unsigned char in[] = { 64, 0xFF };
        reject("zero-run overruns output", in, sizeof(in), 1);
    }

    /* Byte-run missing data byte. */
    {
        const unsigned char in[] = { 128 };
        reject("byte-run missing payload byte", in, sizeof(in), 2);
    }

    /* ----- Encoder coverage. Each case round-trips encoder output
     * through pakka_dk_inflate and memcmp's against the source. */
    {
        unsigned char src[256];
        unsigned char enc[2048];
        unsigned char dec[256];
        size_t enc_len = 0;
        pakka_error_t err;
        pakka_status_t s;
        size_t k;

        /* Encoder: empty input. Must emit exactly 0xFF terminator. */
        s = pakka_dk_deflate(NULL, 0, enc, sizeof(enc), &enc_len, &err);
        if (s != PAKKA_OK) {
            fprintf(stderr, "FAIL: encode empty: status=%d msg=%s\n",
                    (int)s, err.message);
            failures++;
        } else if (enc_len != 1 || enc[0] != 0xFFu) {
            fprintf(stderr, "FAIL: encode empty: expected [0xFF], got %zu bytes\n",
                    enc_len);
            failures++;
        } else {
            printf("ok: encode empty -> terminator only\n");
        }

        /* Encoder: single literal byte. */
        src[0] = 'Q';
        s = pakka_dk_deflate(src, 1, enc, sizeof(enc), &enc_len, &err);
        if (s != PAKKA_OK) {
            fprintf(stderr, "FAIL: encode 1-byte literal: %s\n", err.message);
            failures++;
        } else {
            s = pakka_dk_inflate(enc, enc_len, dec, 1, &err);
            if (s != PAKKA_OK || dec[0] != 'Q') {
                fprintf(stderr,
                        "FAIL: encode 1-byte literal: round-trip mismatch\n");
                failures++;
            } else {
                printf("ok: encode 1-byte literal round-trips\n");
            }
        }

        /* Encoder: 100 zero bytes — should pick zero-run tokens.
         * 100 = 64 + 36, so two zero-run tokens (64 then 36), plus
         * 0xFF terminator = 3 bytes encoded. */
        memset(src, 0, 100);
        s = pakka_dk_deflate(src, 100, enc, sizeof(enc), &enc_len, &err);
        if (s != PAKKA_OK) {
            fprintf(stderr, "FAIL: encode 100 zeros: %s\n", err.message);
            failures++;
        } else if (enc_len > 10) {
            fprintf(stderr,
                    "FAIL: encode 100 zeros: too large (%zu bytes; expected ~3)\n",
                    enc_len);
            failures++;
        } else {
            s = pakka_dk_inflate(enc, enc_len, dec, 100, &err);
            if (s != PAKKA_OK || memcmp(dec, src, 100) != 0) {
                fprintf(stderr, "FAIL: encode 100 zeros: round-trip mismatch\n");
                failures++;
            } else {
                printf("ok: encode 100 zeros (%zu encoded bytes)\n", enc_len);
            }
        }

        /* Encoder: 100 copies of 0x55 — should pick byte-RLE tokens. */
        memset(src, 0x55, 100);
        s = pakka_dk_deflate(src, 100, enc, sizeof(enc), &enc_len, &err);
        if (s != PAKKA_OK) {
            fprintf(stderr, "FAIL: encode 100x 0x55: %s\n", err.message);
            failures++;
        } else if (enc_len > 12) {
            fprintf(stderr,
                    "FAIL: encode 100x 0x55: too large (%zu bytes; expected ~5)\n",
                    enc_len);
            failures++;
        } else {
            s = pakka_dk_inflate(enc, enc_len, dec, 100, &err);
            if (s != PAKKA_OK || memcmp(dec, src, 100) != 0) {
                fprintf(stderr, "FAIL: encode 100x 0x55: round-trip mismatch\n");
                failures++;
            } else {
                printf("ok: encode 100x 0x55 (%zu encoded bytes)\n", enc_len);
            }
        }

        /* Alternating "ab" — exercises back-refs after the literal
         * prefix; no-overlap rule bounds each match by its distance. */
        for (k = 0; k < 64; k++) src[k] = (k & 1) ? 'b' : 'a';
        s = pakka_dk_deflate(src, 64, enc, sizeof(enc), &enc_len, &err);
        if (s != PAKKA_OK) {
            fprintf(stderr, "FAIL: encode alternating pattern: %s\n", err.message);
            failures++;
        } else {
            s = pakka_dk_inflate(enc, enc_len, dec, 64, &err);
            if (s != PAKKA_OK || memcmp(dec, src, 64) != 0) {
                fprintf(stderr,
                        "FAIL: encode alternating pattern: round-trip mismatch\n");
                failures++;
            } else {
                printf("ok: encode alternating ab pattern (%zu encoded bytes)\n",
                       enc_len);
            }
        }

        /* Encoder: gradient (mostly literals, no compression opportunity). */
        for (k = 0; k < 256; k++) src[k] = (unsigned char)k;
        s = pakka_dk_deflate(src, 256, enc, sizeof(enc), &enc_len, &err);
        if (s != PAKKA_OK) {
            fprintf(stderr, "FAIL: encode gradient: %s\n", err.message);
            failures++;
        } else {
            s = pakka_dk_inflate(enc, enc_len, dec, 256, &err);
            if (s != PAKKA_OK || memcmp(dec, src, 256) != 0) {
                fprintf(stderr, "FAIL: encode gradient: round-trip mismatch\n");
                failures++;
            } else {
                printf("ok: encode gradient (%zu encoded bytes for 256 input)\n",
                       enc_len);
            }
        }

        /* Encoder: too-small output buffer must reject with PAKKA_ERR_LIMIT
         * rather than scribble past the buffer or partial-write. Worst
         * case for 100 input bytes is 100 + ceil(100/64) + 1 = 103, so
         * passing 1 forces the bound check. */
        memset(src, 0, 100);
        s = pakka_dk_deflate(src, 100, enc, 1, &enc_len, &err);
        if (s != PAKKA_ERR_LIMIT) {
            fprintf(stderr,
                    "FAIL: undersized out_cap: expected PAKKA_ERR_LIMIT, got %d (%s)\n",
                    (int)s, err.message);
            failures++;
        } else {
            printf("ok: undersized out_cap rejected (%s)\n", err.message);
        }
    }

    /* Larger end-to-end round-trip on a 4 KiB buffer mixing zero runs,
     * byte runs, repeated patterns, and high-entropy literal data. The
     * intent is to exercise every encoder branch in one go. */
    {
        unsigned char src[4096];
        unsigned char enc[4096 + 4096 / 64 + 64];
        unsigned char dec[4096];
        size_t enc_len = 0;
        pakka_error_t err;
        pakka_status_t s;
        size_t k;

        /* Section 1: 512 zeros. */
        memset(src, 0, 512);
        /* Section 2: 512 copies of 0x42. */
        memset(src + 512, 0x42, 512);
        /* Section 3: 1024 bytes of repeating "abcd". */
        for (k = 0; k < 1024; k++) src[1024 + k] = (unsigned char)("abcd"[k & 3]);
        /* Section 4: 1024 bytes of mostly-random literal data (linear
         * congruential — deterministic so the test isn't flaky). */
        {
            uint32_t r = 0xDEADBEEFu;
            for (k = 0; k < 1024; k++) {
                r = r * 1103515245u + 12345u;
                src[2048 + k] = (unsigned char)(r >> 16);
            }
        }
        /* Section 5: 1024 zeros to flush out tail handling. */
        memset(src + 3072, 0, 1024);

        s = pakka_dk_deflate(src, 4096, enc, sizeof(enc), &enc_len, &err);
        if (s != PAKKA_OK) {
            fprintf(stderr, "FAIL: encode mixed 4 KiB: %s\n", err.message);
            failures++;
        } else {
            s = pakka_dk_inflate(enc, enc_len, dec, 4096, &err);
            if (s != PAKKA_OK || memcmp(dec, src, 4096) != 0) {
                fprintf(stderr,
                        "FAIL: encode mixed 4 KiB: round-trip mismatch (%s)\n",
                        err.message);
                failures++;
            } else if (enc_len >= 4096) {
                fprintf(stderr,
                        "FAIL: encode mixed 4 KiB: did not shrink (%zu >= 4096)\n",
                        enc_len);
                failures++;
            } else {
                printf("ok: encode mixed 4 KiB -> %zu bytes round-trip\n",
                       enc_len);
            }
        }
    }

    if (failures == 0) {
        printf("\nAll dk_codec_test scenarios passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d dk_codec_test failures.\n", failures);
    return 1;
}
