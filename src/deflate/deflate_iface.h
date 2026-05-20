/* Internal whole-buffer DEFLATE interface.
 *
 * Two backends implement these symbols, exactly one compiled per build:
 *   - src/deflate/deflate_vendored.c (default) wraps the vendored
 *     pakka_sdeflate (sdefl) + pakka_sinflate (sinfl) codecs.
 *   - src/deflate/deflate_zlib.c (opt-in via PAKKA_DEFLATE_BACKEND=zlib
 *     / -DPAKKA_USE_ZLIB=ON) wraps deflateInit2 / inflateInit2 with
 *     windowBits = -15 for raw RFC 1951 DEFLATE (no zlib header), the
 *     payload form ZIP LFH expects.
 *
 * Both exported symbols are pakka_*-prefixed so the symbol-audit gate
 * passes regardless of backend. zlib's own deflate / inflate symbols
 * appear only as undefined references (U flag in nm -g) in libpakka.a
 * — they resolve at link time against the consumer's -lz.
 *
 * The interface is whole-buffer (matches sinfl's and the previous
 * puff call sites): inputs and outputs are complete payloads, no
 * streaming state. ZIP read/write in pakka allocates the compressed
 * and inflated buffers up front anyway, so streaming would add
 * complexity for no win here. The 64 MiB pakka_set_max_decompressed_size
 * cap (default; PK3_DEFAULT_MAX_DECOMPRESSED in src/common.h) bounds
 * the peak buffer size on read; the write path is bounded by the
 * caller's source-file size and the ZIP non-ZIP64 u32 ceiling. */

#ifndef PAKKA_DEFLATE_IFACE_H
#define PAKKA_DEFLATE_IFACE_H

#include <stddef.h>
#include "pakka.h"

/* Encode src/src_len to a freshly-malloc'd output buffer returned
 * through *out_buf / *out_len. The caller takes ownership and must
 * `free(*out_buf)` once the bytes are consumed (single fwrite into
 * the ZIP payload).
 *
 * Returns PAKKA_OK with *out_buf set on a successful win.
 *
 * Returns PAKKA_OK with *out_buf == NULL when the encoded payload
 * was NOT smaller than src_len — the caller falls back to STORED
 * for that entry. Matches info-zip semantics; the auto-fallback is
 * documented in include/pakka.h on pakka_set_compression.
 *
 * The vendored backend additionally returns PAKKA_OK + *out_buf
 * NULL when src_len > INT_MAX (sdefl takes `int`). The zlib backend
 * accepts the full 4 GiB ZIP u32 range. Callers MUST therefore
 * treat the (NULL, OK) outcome uniformly as "fall back to STORED",
 * not as an error to propagate.
 *
 * Returns PAKKA_ERR_NOMEM on allocation failure (out_buf NULL).
 * Returns PAKKA_ERR_FORMAT if the backend's internal encoder
 * rejects the input — should not happen for valid inputs but is
 * surfaced for robustness. err is populated with operation="deflate"
 * and a descriptive message in every non-OK path. */
pakka_status_t pakka_deflate_compress(const unsigned char *src,
                                      size_t src_len,
                                      unsigned char **out_buf,
                                      size_t *out_len,
                                      pakka_error_t *err);

/* Decode src/src_len into out/out_cap. *out_len receives the actual
 * bytes written; *in_consumed receives the number of compressed
 * bytes consumed.
 *
 * Bytes-consumed reporting differs per backend:
 *   - zlib backend reports the exact consumed-bytes count via
 *     z.total_in. The pk3file.c call sites compare *in_consumed
 *     against src_len and reject mismatches as PAKKA_ERR_FORMAT
 *     (trailing bytes inside the declared LFH csize indicate
 *     corruption or a malformed archive).
 *   - vendored backend (sinfl) cannot surface consumed-bytes — the
 *     upstream pakka_sinflate returns only the output count. The
 *     wrapper therefore unconditionally reports *in_consumed ==
 *     src_len on a successful decode and silently accepts trailing
 *     garbage inside the LFH csize. See src/deflate/deflate_vendored.c
 *     header and src/vendor/sinfl/VENDOR.md for the full caveat.
 *     pakka_pk3_deep_verify_entry's CRC32 cross-check is the end-to-
 *     end integrity backstop on the vendored backend.
 *
 * out == NULL && out_cap == 0 is a scan-only mode: the stream is
 * decoded but no bytes are emitted. *out_len is still populated with
 * the number of bytes the stream WOULD have produced; the caller
 * uses this to validate an empty DEFLATE entry (csize > 0, usize ==
 * 0) without allocating a zero-byte output buffer (malloc(0) is
 * implementation-defined). If the stream legitimately produces zero
 * output (the empty end-of-block marker), the call returns PAKKA_OK
 * with *out_len == 0.
 *
 * Returns PAKKA_ERR_FORMAT on: malformed input, output size >
 * out_cap, or any backend-specific decode error (and, for zlib only,
 * trailing-bytes inside src). Returns PAKKA_ERR_LIMIT when src_len >
 * INT_MAX or out_cap > INT_MAX for the vendored backend (sinfl
 * takes `int`); the zlib backend accepts the full u32 range. err is
 * populated with operation="inflate" and a descriptive message in
 * every non-OK path. */
pakka_status_t pakka_deflate_inflate(const unsigned char *src,
                                     size_t src_len,
                                     unsigned char *out,
                                     size_t out_cap,
                                     size_t *out_len,
                                     size_t *in_consumed,
                                     pakka_error_t *err);

#endif /* PAKKA_DEFLATE_IFACE_H */
