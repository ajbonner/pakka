/* Default DEFLATE backend: wraps the vendored sdefl (encoder) and
 * sinfl (decoder) single-header codecs.
 *
 * Compiled when PAKKA_DEFLATE_BACKEND != zlib (Make) or
 * PAKKA_USE_ZLIB is unset / OFF (CMake). See src/deflate/deflate_iface.h
 * for the contract.
 *
 * End-of-buffer safety: sinfl's patched pakka_sinfl_refill bounds its
 * bit-reads against the s->bitend pointer (a pakka-local addition;
 * see src/vendor/sinfl/sinfl.h and src/vendor/sinfl/VENDOR.md), so
 * malformed or tampered DEFLATE streams cannot drive bitptr past the
 * caller's input buffer. This wrapper passes the caller's `src` and
 * `src_len` directly to pakka_sinflate — no input padding is needed.
 *
 * Trailing-bytes detection caveat: sinfl's exported pakka_sinflate
 * returns only the number of output bytes written, not the number of
 * compressed bytes consumed. The vendored backend therefore reports
 * *in_consumed == src_len unconditionally on a successful decode and
 * does NOT detect a stream that ends before its LFH-declared csize.
 * The zlib backend reports the exact consumed-bytes count via
 * z.total_in and will surface this case. The CRC32 cross-check in
 * pakka_pk3_deep_verify_entry is the end-to-end integrity backstop
 * — any tampering of the compressed payload that survives sinfl
 * decode will produce decompressed bytes that fail CRC.
 *
 * Malformed-block tolerance caveat: sinfl returns a non-negative
 * byte count (success) on several malformed inputs (invalid block
 * type, stored-block LEN/NLEN mismatch, invalid Huffman code lengths)
 * — it just stops decoding and reports the partial output. The
 * `written != entry->length` check at the open_entry and deep_verify
 * call sites catches this as a length mismatch in every case where
 * the malformed block appears before the declared uncompressed size
 * is reached. CRC32 in deep-verify is the secondary backstop. The
 * zlib backend rejects these inputs with Z_DATA_ERROR directly. */

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pakka.h"
#include "deflate_iface.h"
#include "vendor/sdefl/sdefl.h"
#include "vendor/sinfl/sinfl.h"

/* File-local err-fill helper. Mirrors the convention in
 * src/pk3file.c and src/pakfile.c — both TUs have their own static
 * err_fill / err_set_entry pair rather than sharing one through
 * common.c. Three call sites per impl, keeping the helper local is
 * simpler than refactoring out a shared err_fill. */
static pakka_status_t df_err_fill(pakka_error_t *err,
                                  pakka_status_t status,
                                  pakka_error_domain_t domain,
                                  uint32_t system_code,
                                  const char *operation,
                                  const char *fmt, ...) {
    int written;
    size_t op_len;
    va_list args;

    if (err == NULL) {
        return status;
    }

    err->status = status;
    err->domain = domain;
    err->system_code = system_code;
    err->entry_name[0] = '\0';
    err->entry_name_truncated = 0;
    err->entry_index = (size_t)-1;
    err->offset = 0;
    err->length = 0;
    err->message_truncated = 0;

    if (operation == NULL) {
        err->operation[0] = '\0';
    } else {
        op_len = strlen(operation);
        if (op_len >= PAKKA_OPERATION_SIZE) {
            op_len = PAKKA_OPERATION_SIZE - 1;
        }
        memcpy(err->operation, operation, op_len);
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

pakka_status_t pakka_deflate_compress(const unsigned char *src,
                                      size_t src_len,
                                      unsigned char **out_buf,
                                      size_t *out_len,
                                      pakka_error_t *err) {
    struct pakka_sdefl *state;
    unsigned char *out;
    int bound, produced;

    *out_buf = NULL;
    *out_len = 0;

    /* Zero-length input: trivially incompressible (DEFLATE of empty
     * is itself a few bytes — a final-empty-block marker — and would
     * lose to STORED of zero bytes). Return the "not smaller" signal. */
    if (src_len == 0) {
        return PAKKA_OK;
    }

    /* sdefl takes `int` for both source size and output size. Inputs
     * larger than INT_MAX are signaled as "fall back to STORED" via
     * the (OK, NULL out) convention — see deflate_iface.h. */
    if (src_len > (size_t)INT_MAX) {
        return PAKKA_OK;
    }

    /* Heap-allocate the encoder state (~hundreds of KB of hash tables
     * + sequence buffer). On stack it would exceed Windows MSVC's
     * default 1 MB and risk NetBSD/sparc's 256 KB pthread stack. */
    state = malloc(sizeof(*state));
    if (state == NULL) {
        return df_err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                           "deflate",
                           "Cannot allocate DEFLATE encoder state (%zu bytes)",
                           sizeof(*state));
    }
    memset(state, 0, sizeof(*state));

    bound = pakka_sdefl_bound((int)src_len);
    if (bound <= 0) {
        free(state);
        return df_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                           "deflate",
                           "pakka_sdefl_bound returned non-positive value "
                           "%d for input %zu bytes", bound, src_len);
    }
    out = malloc((size_t)bound);
    if (out == NULL) {
        free(state);
        return df_err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                           "deflate",
                           "Cannot allocate DEFLATE output buffer "
                           "(%d bytes)", bound);
    }

    produced = pakka_sdeflate(state, out, src, (int)src_len,
                              PAKKA_SDEFL_LVL_DEF);
    free(state);

    if (produced <= 0 || (size_t)produced >= src_len) {
        /* Either encoder failed or didn't beat STORED. Both paths
         * surface the same "fall back" signal to the caller. */
        free(out);
        return PAKKA_OK;
    }

    *out_buf = out;
    *out_len = (size_t)produced;
    return PAKKA_OK;
}

pakka_status_t pakka_deflate_inflate(const unsigned char *src,
                                     size_t src_len,
                                     unsigned char *out,
                                     size_t out_cap,
                                     size_t *out_len,
                                     size_t *in_consumed,
                                     pakka_error_t *err) {
    unsigned char scan_byte;
    unsigned char *dest;
    int dest_cap;
    int produced;

    *out_len = 0;
    *in_consumed = 0;

    /* sinfl takes `int` for both sizes. Inputs larger than INT_MAX
     * are surfaced as PAKKA_ERR_LIMIT (caller treats as a fatal
     * format error, not a fallback signal — read path doesn't have
     * a STORED alternative). */
    if (src_len > (size_t)INT_MAX) {
        return df_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                           "inflate",
                           "DEFLATE compressed input %zu bytes exceeds "
                           "vendored backend's INT_MAX ceiling", src_len);
    }
    if (out_cap > (size_t)INT_MAX) {
        return df_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                           "inflate",
                           "DEFLATE output capacity %zu bytes exceeds "
                           "vendored backend's INT_MAX ceiling", out_cap);
    }

    /* Scan-only mode: caller wants to validate the stream decodes
     * but doesn't need the output bytes. pakka uses this to verify
     * empty DEFLATE entries (csize > 0, usize == 0) without
     * allocating a zero-byte buffer (malloc(0) is implementation-
     * defined). Point sinfl at a one-byte scratch — if the stream
     * legitimately produces zero output, produced will be 0; if it
     * produces > 0 bytes, the scratch byte gets the first one and
     * sinfl returns -2 (output overflow) on subsequent writes,
     * which surfaces as PAKKA_ERR_FORMAT for the caller. */
    if (out == NULL && out_cap == 0) {
        dest = &scan_byte;
        dest_cap = 0;
    } else if (out == NULL) {
        return df_err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                           PAKKA_ERR_DOMAIN_NONE, 0, "inflate",
                           "DEFLATE inflate called with NULL out but "
                           "non-zero out_cap %zu", out_cap);
    } else {
        dest = out;
        dest_cap = (int)out_cap;
    }

    /* sinfl bounds its own reads against s->bitend (set by pakka's
     * local patch to in + size). No padding needed — pass the
     * caller's buffer through directly. */
    produced = pakka_sinflate(dest, dest_cap, src, (int)src_len);

    if (produced < 0) {
        return df_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                           "inflate",
                           "DEFLATE decode failed (sinfl rc=%d, "
                           "input=%zu bytes, cap=%zu)",
                           produced, src_len, out_cap);
    }

    /* sinfl doesn't surface bytes-consumed; claim full consume on a
     * successful decode and accept the trailing-bytes detection gap
     * documented at the top of this file. */
    *out_len = (size_t)produced;
    *in_consumed = src_len;
    return PAKKA_OK;
}
