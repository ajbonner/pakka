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
