#include <stdarg.h>

#include "common.h"
#include "pakka.h"

/* Daikatana custom byte-codec decoder. Reference: yquake2/pakextract
 * (BSD-2-Clause) — the only published implementation of this codec.
 *
 * Opcode table (b is the next input byte):
 *   b <  64  : literal run of (b + 1) bytes from input
 *   b < 128  : (b - 62) zero bytes
 *   b < 192  : (b - 126) copies of the next input byte
 *   b < 254  : back-reference, length (b - 190), distance read as next
 *              input byte d, distance = d + 2, copy from
 *              produced - distance in OUTPUT
 *   b == 254 : invalid opcode (gap in the table)
 *   b == 255 : terminator (any trailing input is ignored)
 *
 * Termination accepts either an explicit 0xFF or clean input
 * exhaustion; what matters is that produced == out_len at the end.
 * Anything else (partial fill, OOB read/write, back-ref underflow,
 * invalid opcode) returns PAKKA_ERR_FORMAT and leaves the output
 * buffer's tail uninitialized — callers must treat partial output as
 * garbage. */

static pakka_status_t dk_err_fill(pakka_error_t *err,
                                  pakka_status_t status,
                                  const char *fmt, ...) {
    int written;
    va_list args;

    if (err == NULL) {
        return status;
    }

    err->status = status;
    err->domain = PAKKA_ERR_DOMAIN_NONE;
    err->system_code = 0;
    err->entry_name[0] = '\0';
    err->entry_name_truncated = 0;
    err->entry_index = (size_t)-1;
    err->offset = 0;
    err->length = 0;
    err->message_truncated = 0;

    {
        static const char op[] = "dk_inflate";
        size_t op_len = sizeof(op) - 1;
        if (op_len >= PAKKA_OPERATION_SIZE) {
            op_len = PAKKA_OPERATION_SIZE - 1;
        }
        memcpy(err->operation, op, op_len);
        err->operation[op_len] = '\0';
    }

    if (fmt == NULL) {
        err->message[0] = '\0';
    } else {
        va_start(args, fmt);
        written = vsnprintf(err->message, PAKKA_MESSAGE_SIZE, fmt, args);
        va_end(args);
        if (written < 0) {
            err->message[0] = '\0';
        } else if ((size_t)written >= PAKKA_MESSAGE_SIZE) {
            err->message_truncated = 1;
        }
    }

    return status;
}

pakka_status_t pakka_dk_inflate(const unsigned char *in, size_t in_len,
                                unsigned char *out, size_t out_len,
                                pakka_error_t *err) {
    size_t read = 0;
    size_t produced = 0;
    unsigned int opcode;
    size_t copy_len;
    size_t distance;
    size_t i;

    if ((in == NULL && in_len > 0) || (out == NULL && out_len > 0)) {
        return dk_err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                           "NULL buffer with non-zero length");
    }

    while (read < in_len) {
        opcode = in[read++];

        if (opcode < 64) {
            copy_len = (size_t)opcode + 1;
            if (copy_len > in_len - read) {
                return dk_err_fill(err, PAKKA_ERR_FORMAT,
                                   "literal run overruns input "
                                   "(want %zu, have %zu)",
                                   copy_len, in_len - read);
            }
            if (copy_len > out_len - produced) {
                return dk_err_fill(err, PAKKA_ERR_FORMAT,
                                   "literal run overruns output "
                                   "(want %zu, have %zu)",
                                   copy_len, out_len - produced);
            }
            memcpy(out + produced, in + read, copy_len);
            read += copy_len;
            produced += copy_len;
        } else if (opcode < 128) {
            copy_len = (size_t)opcode - 62;
            if (copy_len > out_len - produced) {
                return dk_err_fill(err, PAKKA_ERR_FORMAT,
                                   "zero run overruns output "
                                   "(want %zu, have %zu)",
                                   copy_len, out_len - produced);
            }
            memset(out + produced, 0, copy_len);
            produced += copy_len;
        } else if (opcode < 192) {
            if (read >= in_len) {
                return dk_err_fill(err, PAKKA_ERR_FORMAT,
                                   "byte-run missing data byte");
            }
            copy_len = (size_t)opcode - 126;
            if (copy_len > out_len - produced) {
                return dk_err_fill(err, PAKKA_ERR_FORMAT,
                                   "byte run overruns output "
                                   "(want %zu, have %zu)",
                                   copy_len, out_len - produced);
            }
            memset(out + produced, in[read], copy_len);
            read++;
            produced += copy_len;
        } else if (opcode < 254) {
            if (read >= in_len) {
                return dk_err_fill(err, PAKKA_ERR_FORMAT,
                                   "back-reference missing distance byte");
            }
            copy_len = (size_t)opcode - 190;
            distance = (size_t)in[read++] + 2;
            if (distance > produced) {
                return dk_err_fill(err, PAKKA_ERR_FORMAT,
                                   "back-reference underflow "
                                   "(distance %zu > produced %zu)",
                                   distance, produced);
            }
            if (copy_len > out_len - produced) {
                return dk_err_fill(err, PAKKA_ERR_FORMAT,
                                   "back-reference overruns output "
                                   "(want %zu, have %zu)",
                                   copy_len, out_len - produced);
            }
            /* Byte-by-byte so LZ-style overlap (copy_len > distance)
             * repeats the run correctly — memcpy/memmove would not. */
            for (i = 0; i < copy_len; i++) {
                out[produced] = out[produced - distance];
                produced++;
            }
        } else if (opcode == 254) {
            return dk_err_fill(err, PAKKA_ERR_FORMAT,
                               "invalid opcode 0xFE");
        } else {
            break;          /* 0xFF terminator; trailing input ignored */
        }
    }

    if (produced != out_len) {
        return dk_err_fill(err, PAKKA_ERR_FORMAT,
                           "decoder produced %zu of %zu expected bytes",
                           produced, out_len);
    }

    return PAKKA_OK;
}

/* Daikatana byte-codec encoder. Greedy LZSS-style matcher over a
 * 256-byte window with three non-literal token classes (zero-run,
 * byte-RLE, back-reference) plus a literal-run fallback.
 *
 *  - Zero-run preference via a `zer * 2` tiebreak: a zero-run is a
 *    1-byte opcode (no payload), while byte-RLE and back-ref are
 *    2-byte opcodes (op + payload), so equal length favours zero-run.
 *  - Back-ref length is capped at the distance — no-overlap rule.
 *    Skipping LZ overlap keeps the encoder a single greedy pass.
 *  - Caps sit one byte inside the decoder envelope (65/65/63 there,
 *    64/64/62 here). Length-2 byte-RLE and back-ref break even on
 *    bytes but split surrounding literal runs, so they're skipped. */

/* Decoder envelope is 65 / 65 / 63 respectively. */
#define DK_MAX_ZERO_RUN   64
#define DK_MAX_BYTE_RUN   64
#define DK_MAX_BACK_REF   62
/* Back-ref distance is encoded as `next_byte + 2`, so range 2..257. */
#define DK_MAX_BACK_DIST 257
/* Literal opcode encodes (len - 1) in the low 6 bits — max 64. */
#define DK_MAX_LITERAL    64
#define DK_MIN_ZERO_RUN   2
#define DK_MIN_BYTE_RUN   3
#define DK_MIN_BACK_REF   3

static size_t dk_count_zero_run(const unsigned char *in, size_t in_len,
                                size_t i) {
    size_t len = 0;
    size_t max = in_len - i;
    if (max > DK_MAX_ZERO_RUN) max = DK_MAX_ZERO_RUN;
    while (len < max && in[i + len] == 0) len++;
    return len;
}

static size_t dk_count_byte_run(const unsigned char *in, size_t in_len,
                                size_t i) {
    unsigned char b;
    size_t len;
    size_t max;
    if (i >= in_len) return 0;
    b = in[i];
    len = 1;
    max = in_len - i;
    if (max > DK_MAX_BYTE_RUN) max = DK_MAX_BYTE_RUN;
    while (len < max && in[i + len] == b) len++;
    return len;
}

/* Longest match search over distances [2, 257]. Returns 0 below the
 * minimum profitable length. Match length capped at the distance
 * (no-overlap rule, see encoder comment above). */
static size_t dk_find_match(const unsigned char *in, size_t in_len,
                            size_t i, size_t *out_dist) {
    size_t best_len = 0;
    size_t best_dist = 0;
    size_t d, max_d, remaining, len, per_d_cap;

    if (i < 2) return 0;
    max_d = (i < DK_MAX_BACK_DIST) ? i : DK_MAX_BACK_DIST;
    remaining = in_len - i;
    if (remaining < DK_MIN_BACK_REF) return 0;

    for (d = 2; d <= max_d; d++) {
        per_d_cap = (d < DK_MAX_BACK_REF) ? d : DK_MAX_BACK_REF;
        if (per_d_cap > remaining) per_d_cap = remaining;
        if (per_d_cap < DK_MIN_BACK_REF) continue;

        /* First-byte mismatch filter cuts the inner loop ~256x on
         * non-repetitive input. */
        if (in[i - d] != in[i]) continue;
        len = 1;
        while (len < per_d_cap && in[i + len] == in[i + len - d]) {
            len++;
        }
        if (len > best_len) {
            best_len = len;
            best_dist = d;
            if (best_len == DK_MAX_BACK_REF) break;
        }
    }

    if (best_len < DK_MIN_BACK_REF) return 0;
    *out_dist = best_dist;
    return best_len;
}

pakka_status_t pakka_dk_deflate(const unsigned char *in, size_t in_len,
                                unsigned char *out, size_t out_cap,
                                size_t *out_len, pakka_error_t *err) {
    size_t i = 0;
    size_t op = 0;
    size_t literal_start = 0;
    size_t literal_len = 0;
    uint64_t worst;

    if ((in == NULL && in_len > 0) || (out == NULL && out_cap > 0)) {
        return dk_err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                           "NULL buffer with non-zero length");
    }

    /* Worst case is literal-only: one op-byte per 64 input bytes,
     * plus the bytes, plus the terminator. Reject up front rather
     * than partial-write into an undersized buffer. */
    worst = (uint64_t)in_len + ((uint64_t)in_len + 63u) / 64u + 1u;
    if (worst > out_cap) {
        return dk_err_fill(err, PAKKA_ERR_LIMIT,
                           "output buffer too small for worst-case "
                           "encoding (need %llu, have %zu)",
                           (unsigned long long)worst, out_cap);
    }

    while (i < in_len) {
        size_t zlen = dk_count_zero_run(in, in_len, i);
        size_t rlen = dk_count_byte_run(in, in_len, i);
        size_t mdist = 0;
        size_t mlen = dk_find_match(in, in_len, i, &mdist);
        size_t z_eff = (zlen >= DK_MIN_ZERO_RUN) ? zlen : 0;
        size_t r_eff = (rlen >= DK_MIN_BYTE_RUN) ? rlen : 0;
        size_t m_eff = (mlen >= DK_MIN_BACK_REF) ? mlen : 0;
        int kind;
        size_t take;

        if (z_eff == 0 && r_eff == 0 && m_eff == 0) {
            kind = 0;
            take = 1;
        } else if (z_eff * 2 > r_eff && z_eff * 2 > m_eff) {
            kind = 1;
            take = z_eff;
        } else if (m_eff > r_eff) {
            kind = 3;
            take = m_eff;
        } else {
            kind = 2;
            take = r_eff;
        }

        if (kind == 0) {
            if (literal_len == 0) literal_start = i;
            literal_len++;
            i++;
            if (literal_len == DK_MAX_LITERAL) {
                out[op++] = (unsigned char)(literal_len - 1);
                memcpy(out + op, in + literal_start, literal_len);
                op += literal_len;
                literal_len = 0;
            }
        } else {
            if (literal_len > 0) {
                out[op++] = (unsigned char)(literal_len - 1);
                memcpy(out + op, in + literal_start, literal_len);
                op += literal_len;
                literal_len = 0;
            }
            if (kind == 1) {
                out[op++] = (unsigned char)(take + 62);
            } else if (kind == 2) {
                out[op++] = (unsigned char)(take + 126);
                out[op++] = in[i];
            } else {
                out[op++] = (unsigned char)(take + 190);
                out[op++] = (unsigned char)(mdist - 2);
            }
            i += take;
        }
    }

    if (literal_len > 0) {
        out[op++] = (unsigned char)(literal_len - 1);
        memcpy(out + op, in + literal_start, literal_len);
        op += literal_len;
    }
    out[op++] = 0xFFu;

    if (out_len != NULL) *out_len = op;
    return PAKKA_OK;
}
