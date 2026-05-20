/* Opt-in DEFLATE backend: wraps the host system's zlib via
 * `deflateInit2` / `inflateInit2` with windowBits = -MAX_WBITS to
 * produce / consume raw RFC 1951 DEFLATE streams (no zlib wrapper),
 * the payload form ZIP LFH expects.
 *
 * Compiled when PAKKA_DEFLATE_BACKEND=zlib (Make) or
 * -DPAKKA_USE_ZLIB=ON (CMake). See src/deflate/deflate_iface.h for
 * the contract.
 *
 * Minimum zlib version: 1.2.0 (March 2003) — earlier releases lack
 * deflateBound(). Every CI host that runs the zlib backend (modern
 * glibc, plus the NetBSD/sparc legacy job iff its bundled zlib is
 * ≥ 1.2.0) clears that floor easily. */

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "pakka.h"
#include "deflate_iface.h"

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
    z_stream z;
    uLong bound;
    unsigned char *out;
    int rc;

    *out_buf = NULL;
    *out_len = 0;

    /* Empty input: same "not smaller" signal as the vendored backend.
     * zlib's deflate of zero bytes emits a 2-byte final-empty-block
     * marker which can never beat STORED. */
    if (src_len == 0) {
        return PAKKA_OK;
    }

    /* zlib uInt (unsigned int) caps the per-call avail_in / avail_out
     * fields. On modern hosts UINT_MAX matches or exceeds the ZIP u32
     * ceiling, so this guard is defensive. Bail to STORED fallback
     * rather than fail the add — same semantics as the vendored
     * backend's INT_MAX guard. */
    if (src_len > (size_t)UINT_MAX) {
        return PAKKA_OK;
    }

    memset(&z, 0, sizeof(z));
    rc = deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                      -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) {
        return df_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                           "deflate",
                           "deflateInit2 failed: rc=%d (%s)",
                           rc, z.msg ? z.msg : "no zlib message");
    }

    bound = deflateBound(&z, (uLong)src_len);
    if (bound == 0) {
        deflateEnd(&z);
        return df_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                           "deflate",
                           "deflateBound returned zero for input %zu bytes",
                           src_len);
    }
    if (bound > (uLong)UINT_MAX) {
        /* deflateBound's overhead pushed the upper-bound past uInt.
         * The compressed output may still fit in uint32 (and in the
         * ZIP non-ZIP64 csize field), but we can't size the output
         * buffer for it via zlib's uInt-typed avail_out. Treat as
         * the documented "fall back to STORED" signal, same as the
         * "not smaller" branch — matches the public docstring on
         * pakka_set_compression which promises STORED fallback for
         * the size-limit case. */
        deflateEnd(&z);
        return PAKKA_OK;
    }
    out = malloc((size_t)bound);
    if (out == NULL) {
        deflateEnd(&z);
        return df_err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                           "deflate",
                           "Cannot allocate DEFLATE output buffer "
                           "(%lu bytes)", (unsigned long)bound);
    }

    z.next_in   = (Bytef *)src;       /* cast away const; zlib doesn't mutate */
    z.avail_in  = (uInt)src_len;
    z.next_out  = (Bytef *)out;
    z.avail_out = (uInt)bound;

    rc = deflate(&z, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&z);
        free(out);
        return df_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                           "deflate",
                           "deflate(Z_FINISH) rc=%d (%s); produced=%lu, "
                           "consumed=%lu of %zu",
                           rc, z.msg ? z.msg : "no zlib message",
                           z.total_out, z.total_in, src_len);
    }
    deflateEnd(&z);

    if (z.total_out >= (uLong)src_len) {
        /* Didn't beat STORED. Signal fallback. */
        free(out);
        return PAKKA_OK;
    }

    *out_buf = out;
    *out_len = (size_t)z.total_out;
    return PAKKA_OK;
}

pakka_status_t pakka_deflate_inflate(const unsigned char *src,
                                     size_t src_len,
                                     unsigned char *out,
                                     size_t out_cap,
                                     size_t *out_len,
                                     size_t *in_consumed,
                                     pakka_error_t *err) {
    z_stream z;
    unsigned char scan_byte;
    Bytef *dest;
    uInt dest_cap;
    int rc;

    *out_len = 0;
    *in_consumed = 0;

    if (src_len > (size_t)UINT_MAX) {
        return df_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                           "inflate",
                           "DEFLATE compressed input %zu bytes exceeds "
                           "zlib uInt ceiling", src_len);
    }
    if (out_cap > (size_t)UINT_MAX) {
        return df_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                           "inflate",
                           "DEFLATE output capacity %zu bytes exceeds "
                           "zlib uInt ceiling", out_cap);
    }

    /* Scan-only mode: see deflate_iface.h. Point zlib at a one-byte
     * scratch. If the stream legitimately produces zero output, the
     * scratch is untouched and total_out is 0; if it would produce
     * any output, zlib returns Z_BUF_ERROR (output exhausted) and we
     * surface PAKKA_ERR_FORMAT. */
    if (out == NULL && out_cap == 0) {
        dest = (Bytef *)&scan_byte;
        dest_cap = 0;
    } else if (out == NULL) {
        return df_err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                           PAKKA_ERR_DOMAIN_NONE, 0, "inflate",
                           "DEFLATE inflate called with NULL out but "
                           "non-zero out_cap %zu", out_cap);
    } else {
        dest = (Bytef *)out;
        dest_cap = (uInt)out_cap;
    }

    memset(&z, 0, sizeof(z));
    rc = inflateInit2(&z, -MAX_WBITS);
    if (rc != Z_OK) {
        return df_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                           "inflate",
                           "inflateInit2 failed: rc=%d (%s)",
                           rc, z.msg ? z.msg : "no zlib message");
    }

    z.next_in   = (Bytef *)src;
    z.avail_in  = (uInt)src_len;
    z.next_out  = dest;
    z.avail_out = dest_cap;

    rc = inflate(&z, Z_FINISH);
    if (rc != Z_STREAM_END) {
        unsigned long t_in = z.total_in;
        unsigned long t_out = z.total_out;
        const char *msg = z.msg ? z.msg : "no zlib message";
        inflateEnd(&z);
        return df_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                           "inflate",
                           "inflate(Z_FINISH) rc=%d (%s); consumed=%lu/%zu, "
                           "produced=%lu/%zu",
                           rc, msg, t_in, src_len, t_out, out_cap);
    }

    *out_len     = (size_t)z.total_out;
    *in_consumed = (size_t)z.total_in;
    inflateEnd(&z);
    return PAKKA_OK;
}
