#ifndef PAKKA_TEST_ZIP_BUILD_H
#define PAKKA_TEST_ZIP_BUILD_H

#include <stddef.h>
#include <stdint.h>

/* Parametric single-entry ZIP writer. Every malformed-ZIP test case in
 * the test suite is shaped by overriding one of these fields:
 *   gp_flags       — gp bit 0 = encrypted, gp bit 11 = UTF-8 name.
 *   method         — 0 = STORED, 8 = DEFLATE, 12 = bzip2, …
 *   csize / usize  — compressed / uncompressed sizes; *_override=1
 *                    decouples them from payload_len for the
 *                    "csize != usize" and "LFH overlaps CDR" cases.
 *   crc            — *_override=1 sets a specific CRC (bad-CRC test);
 *                    otherwise zip_build computes it from the payload.
 *
 * Layout written: LFH(30) + name + payload + CDR(46) + name + EOCD(22).
 * Caller frees nothing — write_zip_single owns its FILE handle. */
typedef struct {
    const void *name;
    size_t      name_len;
    const void *payload;
    size_t      payload_len;
    uint16_t    gp_flags;
    uint16_t    method;
    uint32_t    csize_override;
    uint32_t    usize_override;
    uint32_t    crc_override;
    int         use_csize_override;
    int         use_usize_override;
    int         use_crc_override;
} zip_single_t;

int zip_write_single(const char *path, const zip_single_t *p);

/* Find the first CDR signature ("PK\\x01\\x02") in `buf` and read the
 * compression-method u16 LE at CDR+10. Returns -1 if no CDR found. */
int zip_first_cdr_method(const unsigned char *buf, size_t len);

/* Same, returning the n'th CDR's method (0-indexed). -1 if not found. */
int zip_nth_cdr_method(const unsigned char *buf, size_t len, int n);

/* IEEE 802.3 / 0xEDB88320 reflected CRC32. Exposed for tests that need
 * to compute their own CRCs (mainly bad-CRC overrides for verify --deep). */
uint32_t zip_crc32(const void *data, size_t len);

#endif
