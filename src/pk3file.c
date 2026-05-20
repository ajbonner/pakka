/* PK3 / PK4 / ZIP container support for pakka.
 *
 * The on-disk format is a constrained subset of PKWARE APPNOTE ZIP.
 * PK3 (Quake 3) and PK4
 * (Doom 3) are byte-identical and share this entire implementation;
 * the only difference is the filename extension and the pakka_format_t
 * label the caller sees back. This module handles the container layer;
 * DEFLATE decompression goes through the vendored `pakka_inflate`
 * (Mark Adler's puff) at src/vendor/puff/puff.c.
 *
 * Public pakka_* entry points live in src/pakfile.c and dispatch here
 * via the pakka_pk3_* helpers below when pakka_format_is_zip() is true
 * (i.e. format is PK3 or PK4). The pakka_pk3_ name prefix is historical.
 *
 * What's supported: STORED (method 0) and DEFLATE (method 8) entries.
 * Read works for both. Write produces STORED only. ZIP64, encryption,
 * data descriptors (general-purpose bit 3), and multi-disk archives
 * are refused.
 *
 * The error-fill helpers from pakfile.c (err_fill, err_set_entry) are
 * file-local to that TU. To avoid duplicating them, pk3file.c uses
 * file-local copies with the same signature — small enough that
 * factoring them out into common.c would cost more than it saves. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "common.h"
#include "pakka.h"
#include "vendor/puff/puff.h"

/* ------------------------------------------------------------------ */
/* Error-fill helpers (file-local copies of pakfile.c equivalents).   */
/* ------------------------------------------------------------------ */

static pakka_status_t pk3_err_fill(pakka_error_t *err,
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

static void pk3_err_set_entry(pakka_error_t *err,
                              const char *entry_name,
                              size_t entry_index,
                              uint64_t offset,
                              uint64_t length) {
    size_t n;

    if (err == NULL) {
        return;
    }

    err->entry_index = entry_index;
    err->offset = offset;
    err->length = length;

    if (entry_name == NULL) {
        err->entry_name[0] = '\0';
        err->entry_name_truncated = 0;
        return;
    }

    n = strlen(entry_name);
    if (n >= PAKKA_ENTRY_NAME_SIZE) {
        n = PAKKA_ENTRY_NAME_SIZE - 1;
        err->entry_name_truncated = 1;
    } else {
        err->entry_name_truncated = 0;
    }
    memcpy(err->entry_name, entry_name, n);
    err->entry_name[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Little-endian field readers / writers for ZIP records.             */
/* common.c has u32; ZIP records use u16 too. */
/* ------------------------------------------------------------------ */

static uint16_t pk3_read_u16(const unsigned char *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t pk3_read_u32(const unsigned char *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void pk3_put_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void pk3_put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* CRC32 (IEEE 802.3 polynomial, ZIP's choice).                       */
/* ------------------------------------------------------------------ */

static uint32_t pk3_crc32_table[256];
static int pk3_crc32_table_built = 0;

static void pk3_crc32_build_table(void) {
    uint32_t c;
    int i, j;
    for (i = 0; i < 256; i++) {
        c = (uint32_t)i;
        for (j = 0; j < 8; j++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        pk3_crc32_table[i] = c;
    }
    pk3_crc32_table_built = 1;
}

static uint32_t pk3_crc32_update(uint32_t crc,
                                 const unsigned char *buf, size_t len) {
    size_t i;
    if (!pk3_crc32_table_built) {
        pk3_crc32_build_table();
    }
    crc ^= 0xFFFFFFFFu;
    for (i = 0; i < len; i++) {
        crc = pk3_crc32_table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* EOCD scan + validation.                                            */
/* ------------------------------------------------------------------ */

/* Scan backwards from EOF for PK\005\006. Returns 0 on success and
 * sets *out_eocd_offset; returns non-zero on no-match or I/O error
 * (errno set). */
static int pk3_find_eocd(FILE *fp, uint64_t file_size,
                         uint64_t *out_eocd_offset) {
    unsigned char buf[PK3_EOCD_SCAN_MAX];
    uint64_t scan_start;
    size_t scan_len;
    size_t i;

    if (file_size < PK3_EOCD_SIZE) {
        errno = 0;
        return -1;
    }

    scan_len = (file_size > PK3_EOCD_SCAN_MAX)
        ? PK3_EOCD_SCAN_MAX
        : (size_t)file_size;
    scan_start = file_size - (uint64_t)scan_len;

    if (pakka_compat_fseek(fp, (int64_t)scan_start, SEEK_SET) != 0) {
        return -1;
    }
    if (fread(buf, 1, scan_len, fp) != scan_len) {
        return -1;
    }

    /* Scan from the latest possible EOCD start backwards. The "latest
     * valid" rule from the ZIP spec means we want the FIRST hit when
     * iterating EOF→start. */
    if (scan_len < PK3_EOCD_SIZE) {
        errno = 0;
        return -1;
    }
    for (i = scan_len - PK3_EOCD_SIZE + 1; i-- > 0; ) {
        if (memcmp(&buf[i], PK3_EOCD_SIGNATURE,
                   PAKFILE_SIGNATURE_LEN) == 0) {
            /* comment_length field at offset 20 in the EOCD record */
            uint16_t comment_len = pk3_read_u16(&buf[i + 20]);
            if ((uint64_t)i + PK3_EOCD_SIZE + comment_len == (uint64_t)scan_len) {
                *out_eocd_offset = scan_start + (uint64_t)i;
                return 0;
            }
        }
    }
    errno = 0;
    return -1;
}

/* Read + validate the 22-byte EOCD record at the given offset.
 * Refuses multi-disk archives, mismatched counts, ZIP64 sentinels,
 * and out-of-bounds CDR. */
static pakka_status_t pk3_validate_eocd(Pak_t *pak,
                                        uint64_t eocd_offset,
                                        uint32_t *out_cdr_offset,
                                        uint32_t *out_cdr_size,
                                        uint16_t *out_entry_count,
                                        pakka_error_t *err) {
    unsigned char eocd[PK3_EOCD_SIZE];
    uint16_t disk_num, disk_with_cdr, entries_on_disk, total_entries;
    uint32_t cdr_size, cdr_offset;
    int saved_errno;

    if (pakka_compat_fseek(pak->fp, (int64_t)eocd_offset, SEEK_SET) != 0) {
        saved_errno = errno;
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "open",
                            "Cannot seek to EOCD");
    }
    if (fread(eocd, 1, PK3_EOCD_SIZE, pak->fp) != PK3_EOCD_SIZE) {
        saved_errno = errno;
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "open",
                            "Cannot read EOCD");
    }

    disk_num         = pk3_read_u16(&eocd[4]);
    disk_with_cdr    = pk3_read_u16(&eocd[6]);
    entries_on_disk  = pk3_read_u16(&eocd[8]);
    total_entries    = pk3_read_u16(&eocd[10]);
    cdr_size         = pk3_read_u32(&eocd[12]);
    cdr_offset       = pk3_read_u32(&eocd[16]);

    if (disk_num != 0 || disk_with_cdr != 0) {
        return pk3_err_fill(err, PAKKA_ERR_UNSUPPORTED,
                            PAKKA_ERR_DOMAIN_NONE, 0, "open",
                            "Multi-disk ZIP archives are not supported");
    }
    if (entries_on_disk != total_entries) {
        return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open",
                            "EOCD entry counts disagree (%u != %u)",
                            (unsigned)entries_on_disk,
                            (unsigned)total_entries);
    }
    /* ZIP64 sentinels — refuse */
    if (total_entries == 0xFFFFu
        || cdr_size == 0xFFFFFFFFu
        || cdr_offset == 0xFFFFFFFFu) {
        return pk3_err_fill(err, PAKKA_ERR_UNSUPPORTED,
                            PAKKA_ERR_DOMAIN_NONE, 0, "open",
                            "ZIP64 archives are not supported");
    }
    /* CDR must fit before EOCD */
    if ((uint64_t)cdr_offset + (uint64_t)cdr_size > eocd_offset) {
        return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open",
                            "ZIP central directory extends past EOCD");
    }
    if ((uint32_t)total_entries > PAKFILE_MAX_ENTRIES) {
        return pk3_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open",
                            "ZIP has too many entries (%u, max %u)",
                            (unsigned)total_entries, PAKFILE_MAX_ENTRIES);
    }

    *out_cdr_offset = cdr_offset;
    *out_cdr_size = cdr_size;
    *out_entry_count = total_entries;
    return PAKKA_OK;
}

/* ------------------------------------------------------------------ */
/* CDR parse — read every entry into the in-memory list.              */
/* ------------------------------------------------------------------ */

static pakka_status_t pk3_validate_lfh(Pak_t *pak, Pakfileentry_t *entry,
                                       size_t entry_index,
                                       pakka_error_t *err) {
    unsigned char lfh[PK3_LFH_SIZE];
    uint16_t lfh_method, lfh_flags, lfh_name_len, lfh_extra_len;
    uint32_t lfh_crc, lfh_csize, lfh_usize;
    int saved_errno;

    if (pakka_compat_fseek(pak->fp, (int64_t)entry->pk3_lfh_offset, SEEK_SET) != 0) {
        saved_errno = errno;
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "open",
                            "Cannot seek to LFH for entry %s",
                            entry->filename);
    }
    if (fread(lfh, 1, PK3_LFH_SIZE, pak->fp) != PK3_LFH_SIZE) {
        saved_errno = errno;
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "open",
                            "Cannot read LFH for entry %s",
                            entry->filename);
    }
    if (memcmp(lfh, PK3_LFH_SIGNATURE, PAKFILE_SIGNATURE_LEN) != 0) {
        return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open", "Bad LFH signature for entry %s",
                            entry->filename);
    }
    lfh_flags     = pk3_read_u16(&lfh[6]);
    lfh_method    = pk3_read_u16(&lfh[8]);
    lfh_crc       = pk3_read_u32(&lfh[14]);
    lfh_csize     = pk3_read_u32(&lfh[18]);
    lfh_usize     = pk3_read_u32(&lfh[22]);
    lfh_name_len  = pk3_read_u16(&lfh[26]);
    lfh_extra_len = pk3_read_u16(&lfh[28]);

    /* Refuse encrypted (bit 0) and data descriptors (bit 3) — the latter
     * stores CRC/sizes after the payload, which we won't support
     * without streaming seek-less reads. */
    if (lfh_flags & 0x1u) {
        return pk3_err_fill(err, PAKKA_ERR_UNSUPPORTED,
                            PAKKA_ERR_DOMAIN_NONE, 0, "open",
                            "Encrypted ZIP entries are not supported (%s)",
                            entry->filename);
    }
    if (lfh_flags & 0x8u) {
        return pk3_err_fill(err, PAKKA_ERR_UNSUPPORTED,
                            PAKKA_ERR_DOMAIN_NONE, 0, "open",
                            "ZIP data descriptors are not supported (%s)",
                            entry->filename);
    }

    if (lfh_method != entry->pk3_method
        || lfh_crc != entry->pk3_crc32
        || lfh_csize != entry->pk3_compressed_size
        || lfh_usize != entry->length) {
        /* err_fill clears entry context, so call it FIRST then layer
         * entry-name + index + offset on top via err_set_entry. */
        pakka_status_t s = pk3_err_fill(err, PAKKA_ERR_FORMAT,
                            PAKKA_ERR_DOMAIN_NONE, 0,
                            "open",
                            "LFH disagrees with CDR for entry %s",
                            entry->filename);
        pk3_err_set_entry(err, entry->filename, entry_index,
                          entry->pk3_lfh_offset, entry->length);
        return s;
    }

    /* STORED entries must have compressed_size == uncompressed_size.
     * Otherwise the reader (which uses entry->length for STORED) would
     * disagree with the bounds check (which uses compressed_size). */
    if (entry->pk3_method == PK3_METHOD_STORED
        && entry->pk3_compressed_size != entry->length) {
        return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open",
                            "STORED entry %s has csize != usize "
                            "(%u != %u)",
                            entry->filename,
                            (unsigned)entry->pk3_compressed_size,
                            (unsigned)entry->length);
    }

    /* Payload starts after the LFH header + name + extra. Compute in
     * uint64_t and refuse u32 wrap before storing. */
    {
        uint64_t payload64 = (uint64_t)entry->pk3_lfh_offset
                           + (uint64_t)PK3_LFH_SIZE
                           + (uint64_t)lfh_name_len
                           + (uint64_t)lfh_extra_len;
        if (payload64 > 0xFFFFFFFFu) {
            return pk3_err_fill(err, PAKKA_ERR_FORMAT,
                                PAKKA_ERR_DOMAIN_NONE, 0, "open",
                                "ZIP LFH offset+name+extra overflows "
                                "u32 for entry %s", entry->filename);
        }
        entry->pk3_payload_offset = (uint32_t)payload64;
    }

    /* Payload must fit strictly before the central directory — a
     * malicious entry could otherwise point bytes into the CDR/EOCD
     * region and pass the looser EOF-only bound. */
    if ((uint64_t)entry->pk3_payload_offset
        + (uint64_t)entry->pk3_compressed_size > (uint64_t)pak->pk3_cdr_offset) {
        return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open",
                            "ZIP entry %s payload extends into CDR",
                            entry->filename);
    }

    return PAKKA_OK;
}

static pakka_status_t pk3_load_cdr(Pak_t *pak,
                                   uint32_t cdr_offset,
                                   uint32_t cdr_size,
                                   uint16_t entry_count,
                                   pakka_error_t *err) {
    Pakfileentry_t *tail = NULL;
    unsigned char *cdr;
    size_t pos = 0;
    int saved_errno;
    size_t i;
    uint32_t skipped_dirs = 0;

    if (entry_count == 0 && cdr_size == 0) {
        return PAKKA_OK;
    }
    /* Refuse "count says entries, size says none" and vice versa. */
    if ((entry_count == 0) != (cdr_size == 0)) {
        return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open",
                            "ZIP EOCD entry_count and cdr_size disagree "
                            "(entries=%u, cdr_size=%u)",
                            (unsigned)entry_count, (unsigned)cdr_size);
    }

    /* Cap CDR-size against a separate open-time constant. EOCD's
     * cdr_size is a u32 and an attacker can declare it inflated to
     * force a 4 GiB malloc here, before per-entry validation runs.
     * Not gated on pak->max_decompressed because that knob is set
     * earlier in this same call (no caller-visible window). See
     * PK3_MAX_CDR_SIZE in src/common.h for the rationale. */
    if ((uint64_t)cdr_size > PK3_MAX_CDR_SIZE) {
        return pk3_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open",
                            "ZIP CDR size (%u) exceeds open-time cap "
                            "(%" PRIu64 ")",
                            (unsigned)cdr_size,
                            (uint64_t)PK3_MAX_CDR_SIZE);
    }

    cdr = malloc(cdr_size);
    if (cdr == NULL) {
        return pk3_err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open", "Cannot allocate CDR buffer (%u bytes)",
                            (unsigned)cdr_size);
    }
    if (pakka_compat_fseek(pak->fp, (int64_t)cdr_offset, SEEK_SET) != 0) {
        saved_errno = errno;
        free(cdr);
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "open",
                            "Cannot seek to ZIP central directory");
    }
    if (fread(cdr, 1, cdr_size, pak->fp) != cdr_size) {
        saved_errno = errno;
        free(cdr);
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "open",
                            "Cannot read ZIP central directory");
    }

    for (i = 0; i < entry_count; i++) {
        Pakfileentry_t *entry;
        uint16_t method, flags, name_len, extra_len, comment_len;
        uint32_t crc, csize, usize, ext_attrs, lfh_offset;

        if (pos + PK3_CDR_SIZE > cdr_size) {
            free(cdr);
            return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE,
                                0, "open",
                                "ZIP CDR truncated at entry %zu", i);
        }
        if (memcmp(&cdr[pos], PK3_CDR_SIGNATURE,
                   PAKFILE_SIGNATURE_LEN) != 0) {
            free(cdr);
            return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE,
                                0, "open",
                                "Bad CDR signature at entry %zu", i);
        }

        flags        = pk3_read_u16(&cdr[pos + 8]);
        method       = pk3_read_u16(&cdr[pos + 10]);
        crc          = pk3_read_u32(&cdr[pos + 16]);
        csize        = pk3_read_u32(&cdr[pos + 20]);
        usize        = pk3_read_u32(&cdr[pos + 24]);
        name_len     = pk3_read_u16(&cdr[pos + 28]);
        extra_len    = pk3_read_u16(&cdr[pos + 30]);
        comment_len  = pk3_read_u16(&cdr[pos + 32]);
        ext_attrs    = pk3_read_u32(&cdr[pos + 38]);
        lfh_offset   = pk3_read_u32(&cdr[pos + 42]);

        if (pos + PK3_CDR_SIZE + name_len + extra_len + comment_len
            > cdr_size) {
            free(cdr);
            return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE,
                                0, "open",
                                "ZIP CDR entry %zu extends past CDR end",
                                i);
        }

        /* Refuse encrypted / data-descriptor flagged entries up-front;
         * LFH validation reasserts. */
        if (flags & 0x1u) {
            free(cdr);
            return pk3_err_fill(err, PAKKA_ERR_UNSUPPORTED,
                                PAKKA_ERR_DOMAIN_NONE, 0, "open",
                                "Encrypted ZIP entries are not supported");
        }
        if (flags & 0x8u) {
            free(cdr);
            return pk3_err_fill(err, PAKKA_ERR_UNSUPPORTED,
                                PAKKA_ERR_DOMAIN_NONE, 0, "open",
                                "ZIP data descriptors are not supported");
        }
        /* ZIP64 sentinels in per-entry fields */
        if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu
            || lfh_offset == 0xFFFFFFFFu) {
            free(cdr);
            return pk3_err_fill(err, PAKKA_ERR_UNSUPPORTED,
                                PAKKA_ERR_DOMAIN_NONE, 0, "open",
                                "ZIP64 per-entry fields are not supported");
        }
        if (method != PK3_METHOD_STORED && method != PK3_METHOD_DEFLATE) {
            free(cdr);
            return pk3_err_fill(err, PAKKA_ERR_UNSUPPORTED,
                                PAKKA_ERR_DOMAIN_NONE, 0, "open",
                                "ZIP compression method %u is not supported",
                                (unsigned)method);
        }
        if (name_len == 0) {
            free(cdr);
            return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE,
                                0, "open",
                                "ZIP entry %zu has empty filename", i);
        }
        if (name_len >= PAKKA_ENTRY_NAME_SIZE) {
            free(cdr);
            return pk3_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE,
                                0, "open",
                                "ZIP entry %zu name too long (%u bytes, max %u)",
                                i, (unsigned)name_len,
                                (unsigned)(PAKKA_ENTRY_NAME_SIZE - 1));
        }
        /* Refuse Unix symlink entries via external-attrs file-type bits */
        if (((ext_attrs >> 16) & PK3_UNIX_IFMT) == PK3_UNIX_IFLNK) {
            free(cdr);
            return pk3_err_fill(err, PAKKA_ERR_UNSAFE_NAME,
                                PAKKA_ERR_DOMAIN_NONE, 0, "open",
                                "ZIP entry %zu is a symlink", i);
        }

        /* Directory entries (name ends in '/', size == 0): silently
         * skipped. ZIP tools commonly emit them; rejecting would break
         * real-world PK3/PK4 archives. */
        if (cdr[pos + PK3_CDR_SIZE + name_len - 1] == '/'
            && usize == 0 && csize == 0) {
            skipped_dirs++;
            pos += PK3_CDR_SIZE + name_len + extra_len + comment_len;
            continue;
        }

        entry = calloc(1, sizeof(*entry));
        if (entry == NULL) {
            free(cdr);
            return pk3_err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE,
                                0, "open",
                                "Cannot allocate ZIP entry");
        }
        memcpy(entry->filename, &cdr[pos + PK3_CDR_SIZE], name_len);
        entry->filename[name_len] = '\0';
        entry->pk3_method = method;
        entry->pk3_crc32 = crc;
        entry->pk3_compressed_size = csize;
        entry->length = usize;
        entry->offset = lfh_offset;        /* placeholder; payload_offset is the real one */
        entry->pk3_lfh_offset = lfh_offset;

        if (pak->head == NULL) {
            pak->head = entry;
        } else {
            tail->next = entry;
        }
        tail = entry;

        pos += PK3_CDR_SIZE + name_len + extra_len + comment_len;
    }

    /* CDR must be exactly consumed — extra bytes are suspect. */
    if (pos != cdr_size) {
        free(cdr);
        return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open",
                            "ZIP CDR has %zu trailing bytes after %u "
                            "parsed entries",
                            cdr_size - pos, (unsigned)entry_count);
    }
    free(cdr);

    pak->num_entries = (uint32_t)(entry_count - skipped_dirs);

    /* Second pass: validate each LFH and cache pk3_payload_offset. */
    {
        Pakfileentry_t *e;
        size_t idx = 0;
        for (e = pak->head; e != NULL; e = e->next, idx++) {
            pakka_status_t s = pk3_validate_lfh(pak, e, idx, err);
            if (s != PAKKA_OK) return s;
            /* Sync the public offset accessor to payload_offset — that's
             * the byte the user typically wants to know about. */
            e->offset = e->pk3_payload_offset;
        }
    }
    return PAKKA_OK;
}

/* ------------------------------------------------------------------ */
/* Public entry points called from pakfile.c dispatch.                */
/* ------------------------------------------------------------------ */

pakka_status_t pakka_pk3_open_impl(Pak_t *pak, pakka_error_t *err) {
    uint64_t eocd_offset;
    uint32_t cdr_offset, cdr_size;
    uint16_t entry_count;
    pakka_status_t s;
    int saved_errno;

    /* pak->format is set by the caller in src/pakfile.c (PK3 or PK4
     * depending on the filename extension); we share the rest of the
     * open path between the two formats. */
    pak->max_decompressed = PK3_DEFAULT_MAX_DECOMPRESSED;

    if (pk3_find_eocd(pak->fp, pak->file_size, &eocd_offset) != 0) {
        saved_errno = errno;
        if (saved_errno != 0) {
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "open",
                                "Cannot scan for ZIP EOCD");
        }
        return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open",
                            "ZIP end-of-central-directory not found");
    }

    s = pk3_validate_eocd(pak, eocd_offset, &cdr_offset, &cdr_size,
                          &entry_count, err);
    if (s != PAKKA_OK) return s;

    pak->pk3_cdr_offset = cdr_offset;
    return pk3_load_cdr(pak, cdr_offset, cdr_size, entry_count, err);
}

/* ------------------------------------------------------------------ */
/* Reader path (extract / read_entry_alloc).                          */
/* ------------------------------------------------------------------ */

static pakka_status_t pk3_load_compressed(Pak_t *pak,
                                          Pakfileentry_t *entry,
                                          unsigned char **out_buf,
                                          pakka_error_t *err) {
    unsigned char *buf;
    int saved_errno;

    buf = malloc(entry->pk3_compressed_size);
    if (buf == NULL) {
        return pk3_err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open_entry",
                            "Cannot allocate compressed buffer (%u bytes)",
                            (unsigned)entry->pk3_compressed_size);
    }
    if (pakka_compat_fseek(pak->fp, (int64_t)entry->pk3_payload_offset, SEEK_SET) != 0) {
        saved_errno = errno;
        free(buf);
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "open_entry",
                            "Cannot seek to ZIP payload for %s",
                            entry->filename);
    }
    if (fread(buf, 1, entry->pk3_compressed_size, pak->fp)
        != entry->pk3_compressed_size) {
        saved_errno = errno;
        free(buf);
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "open_entry",
                            "Cannot read ZIP payload for %s",
                            entry->filename);
    }
    *out_buf = buf;
    return PAKKA_OK;
}

/* Allocate inflated buffer + read compressed payload + inflate.
 * Caller owns reader->inflated_buf; pakka_reader_close frees. */
pakka_status_t pakka_pk3_open_entry_impl(Pak_t *pak,
                                         struct pakka_reader *reader,
                                         Pakfileentry_t *entry,
                                         pakka_error_t *err) {
    unsigned char *compressed = NULL;
    unsigned long destlen, srclen;
    pakka_status_t s;
    int rc;

    reader->next_offset = entry->pk3_payload_offset;
    reader->remaining = entry->length;
    reader->inflated_buf = NULL;
    reader->inflated_len = 0;
    reader->inflated_cursor = 0;

    /* Enforce the decompressed-size cap on both sides. */
    if (pak->max_decompressed > 0) {
        if ((uint64_t)entry->length > pak->max_decompressed
            || (uint64_t)entry->pk3_compressed_size > pak->max_decompressed) {
            return pk3_err_fill(err, PAKKA_ERR_LIMIT,
                                PAKKA_ERR_DOMAIN_NONE, 0, "open_entry",
                                "ZIP entry %s exceeds max_decompressed_size "
                                "(uncompressed=%u, compressed=%u, cap=%"
                                PRIu64 ")",
                                entry->filename,
                                (unsigned)entry->length,
                                (unsigned)entry->pk3_compressed_size,
                                pak->max_decompressed);
        }
    }

    /* STORED entries: nothing to inflate, reader streams directly. */
    if (entry->pk3_method == PK3_METHOD_STORED) {
        return PAKKA_OK;
    }

    /* DEFLATE: load compressed payload, inflate to inflated_buf.
     * malloc(0) is implementation-defined; treat csize==0 explicitly. */
    if (entry->pk3_compressed_size == 0) {
        /* A DEFLATE entry must contain at least the end-of-block bit;
         * zero compressed bytes is malformed. */
        return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open_entry",
                            "ZIP DEFLATE entry %s has zero compressed bytes",
                            entry->filename);
    }
    s = pk3_load_compressed(pak, entry, &compressed, err);
    if (s != PAKKA_OK) return s;

    /* Inflate runs even when usize == 0 — a valid empty payload still
     * carries a deflate end-of-block marker that must parse cleanly.
     * puff with NIL dest is a "scan only" mode (see puff.h). */
    if (entry->length == 0) {
        destlen = 0;
        srclen = (unsigned long)entry->pk3_compressed_size;
        rc = pakka_inflate(NIL, &destlen, compressed, &srclen);
        free(compressed);
        if (rc != 0 || destlen != 0
            || srclen != (unsigned long)entry->pk3_compressed_size) {
            return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE,
                                0, "open_entry",
                                "ZIP DEFLATE decode failed for empty "
                                "entry %s (rc=%d, consumed=%lu/%u)",
                                entry->filename, rc, srclen,
                                (unsigned)entry->pk3_compressed_size);
        }
        return PAKKA_OK;
    }

    reader->inflated_buf = malloc(entry->length);
    if (reader->inflated_buf == NULL) {
        free(compressed);
        return pk3_err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open_entry",
                            "Cannot allocate inflated buffer (%u bytes)",
                            (unsigned)entry->length);
    }

    destlen = (unsigned long)entry->length;
    srclen = (unsigned long)entry->pk3_compressed_size;
    rc = pakka_inflate(reader->inflated_buf, &destlen, compressed, &srclen);
    free(compressed);
    /* Reject: decode error, produced size mismatch, OR consumed size
     * mismatch (trailing bytes inside the declared compressed payload
     * indicate either corruption or a malformed archive). */
    if (rc != 0
        || destlen != (unsigned long)entry->length
        || srclen != (unsigned long)entry->pk3_compressed_size) {
        free(reader->inflated_buf);
        reader->inflated_buf = NULL;
        return pk3_err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "open_entry",
                            "ZIP DEFLATE decode failed for %s "
                            "(rc=%d, got=%lu/%u, consumed=%lu/%u)",
                            entry->filename, rc,
                            destlen, (unsigned)entry->length,
                            srclen, (unsigned)entry->pk3_compressed_size);
    }
    reader->inflated_len = entry->length;
    return PAKKA_OK;
}

pakka_status_t pakka_pk3_reader_read_impl(struct pakka_reader *reader,
                                          void *buf, size_t len,
                                          size_t *nread,
                                          pakka_error_t *err) {
    Pak_t *pak = reader->archive;
    size_t want;
    int saved_errno;

    if (reader->remaining == 0) {
        if (nread != NULL) *nread = 0;
        return PAKKA_OK;
    }

    /* DEFLATE path: serve from inflated_buf. */
    if (reader->inflated_buf != NULL || reader->inflated_len > 0) {
        want = len;
        if ((uint64_t)want > reader->remaining) {
            want = (size_t)reader->remaining;
        }
        if (want > 0) {
            memcpy(buf, reader->inflated_buf + reader->inflated_cursor, want);
            reader->inflated_cursor += want;
            reader->remaining -= want;
        }
        if (nread != NULL) *nread = want;
        return PAKKA_OK;
    }

    /* STORED path: seek + fread, just like the PAK reader. */
    if (pakka_compat_fseek(pak->fp, (int64_t)reader->next_offset, SEEK_SET) != 0) {
        saved_errno = errno;
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "reader_read",
                            "Cannot seek to ZIP STORED payload");
    }
    want = len;
    if ((uint64_t)want > reader->remaining) {
        want = (size_t)reader->remaining;
    }
    if (want > 0) {
        size_t got = fread(buf, 1, want, pak->fp);
        if (got != want) {
            saved_errno = errno;
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "reader_read",
                                "Short read from ZIP STORED entry");
        }
        reader->next_offset += want;
        reader->remaining -= want;
    }
    if (nread != NULL) *nread = want;
    return PAKKA_OK;
}

/* Called from pakka_reader_close. Frees the inflated buffer if any.
 * Caller frees the reader struct itself. */
void pakka_pk3_reader_close_impl(struct pakka_reader *reader) {
    if (reader != NULL && reader->inflated_buf != NULL) {
        free(reader->inflated_buf);
        reader->inflated_buf = NULL;
        reader->inflated_len = 0;
        reader->inflated_cursor = 0;
    }
}

/* Deep verify of a single ZIP entry (PK3/PK4): read the on-disk bytes,
 * decompress if DEFLATE, compute CRC32 of the uncompressed payload,
 * compare against the entry's CDR-recorded CRC32 and uncompressed
 * size. */
pakka_status_t pakka_pk3_deep_verify_entry(Pak_t *pak, Pakfileentry_t *entry,
                                           pakka_error_t *err) {
    unsigned char *compressed = NULL;
    unsigned char *inflated;
    unsigned long destlen, srclen;
    uint32_t crc;
    pakka_status_t s;
    int rc;

    /* Enforce the same decompressed-size cap that pakka_open_entry
     * applies. --verify --deep on a hostile archive can otherwise force
     * unbounded compressed and inflated allocations regardless of the
     * caller's pakka_set_max_decompressed_size choice. */
    if (pak->max_decompressed > 0) {
        if ((uint64_t)entry->length > pak->max_decompressed
            || (uint64_t)entry->pk3_compressed_size > pak->max_decompressed) {
            return pk3_err_fill(err, PAKKA_ERR_LIMIT,
                                PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                                "ZIP entry %s exceeds max_decompressed_size "
                                "(uncompressed=%u, compressed=%u, cap=%"
                                PRIu64 ")",
                                entry->filename,
                                (unsigned)entry->length,
                                (unsigned)entry->pk3_compressed_size,
                                pak->max_decompressed);
        }
    }

    /* Zero-byte entry: CRC32 of empty is 0; only validate CRC.
     * No on-disk bytes to read or decompress. */
    if (entry->length == 0 && entry->pk3_compressed_size == 0) {
        if (entry->pk3_crc32 != 0u) {
            return pk3_err_fill(err, PAKKA_ERR_FORMAT,
                                PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                                "ZIP entry %s has CRC32 0x%08x but is "
                                "zero bytes (expected CRC32 0)",
                                entry->filename, entry->pk3_crc32);
        }
        return PAKKA_OK;
    }

    s = pk3_load_compressed(pak, entry, &compressed, err);
    if (s != PAKKA_OK) return s;

    if (entry->pk3_method == PK3_METHOD_STORED) {
        /* Verified at open: STORED requires csize == usize. */
        crc = pk3_crc32_update(0, compressed,
                               (size_t)entry->pk3_compressed_size);
        free(compressed);
    } else {
        /* DEFLATE: inflate, then CRC the uncompressed bytes. */
        if (entry->length == 0) {
            /* puff(NIL, ...) scan-only mode validates the stream. */
            destlen = 0;
            srclen = (unsigned long)entry->pk3_compressed_size;
            rc = pakka_inflate(NIL, &destlen, compressed, &srclen);
            free(compressed);
            if (rc != 0 || destlen != 0
                || srclen != (unsigned long)entry->pk3_compressed_size) {
                return pk3_err_fill(err, PAKKA_ERR_FORMAT,
                                    PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                                    "ZIP DEFLATE decode failed for empty "
                                    "entry %s (rc=%d, consumed=%lu/%u)",
                                    entry->filename, rc, srclen,
                                    (unsigned)entry->pk3_compressed_size);
            }
            return (entry->pk3_crc32 == 0u)
                ? PAKKA_OK
                : pk3_err_fill(err, PAKKA_ERR_FORMAT,
                               PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                               "ZIP entry %s declares zero uncompressed "
                               "bytes but CRC32 = 0x%08x",
                               entry->filename, entry->pk3_crc32);
        }
        inflated = malloc(entry->length);
        if (inflated == NULL) {
            free(compressed);
            return pk3_err_fill(err, PAKKA_ERR_NOMEM,
                                PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                                "Cannot allocate inflate buffer (%u bytes)",
                                (unsigned)entry->length);
        }
        destlen = (unsigned long)entry->length;
        srclen = (unsigned long)entry->pk3_compressed_size;
        rc = pakka_inflate(inflated, &destlen, compressed, &srclen);
        free(compressed);
        if (rc != 0
            || destlen != (unsigned long)entry->length
            || srclen != (unsigned long)entry->pk3_compressed_size) {
            free(inflated);
            return pk3_err_fill(err, PAKKA_ERR_FORMAT,
                                PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                                "ZIP DEFLATE decode failed for %s "
                                "(rc=%d, got=%lu/%u, consumed=%lu/%u)",
                                entry->filename, rc,
                                destlen, (unsigned)entry->length,
                                srclen, (unsigned)entry->pk3_compressed_size);
        }
        crc = pk3_crc32_update(0, inflated, (size_t)entry->length);
        free(inflated);
    }

    if (crc != entry->pk3_crc32) {
        return pk3_err_fill(err, PAKKA_ERR_FORMAT,
                            PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                            "ZIP entry %s CRC32 mismatch "
                            "(computed 0x%08x, declared 0x%08x)",
                            entry->filename, crc, entry->pk3_crc32);
    }
    return PAKKA_OK;
}

/* ------------------------------------------------------------------ */
/* Write path: create / add / delete / commit. STORED only.           */
/* ------------------------------------------------------------------ */

/* Build a 22-byte EOCD record for the given central directory
 * offset/size and entry count. */
static void pk3_build_eocd(unsigned char eocd[PK3_EOCD_SIZE],
                           uint32_t cdr_offset, uint32_t cdr_size,
                           uint16_t entry_count) {
    eocd[0] = 'P'; eocd[1] = 'K'; eocd[2] = 5; eocd[3] = 6;
    pk3_put_u16(&eocd[4], 0);                /* disk number */
    pk3_put_u16(&eocd[6], 0);                /* disk with CDR start */
    pk3_put_u16(&eocd[8], entry_count);      /* entries on this disk */
    pk3_put_u16(&eocd[10], entry_count);     /* total entries */
    pk3_put_u32(&eocd[12], cdr_size);
    pk3_put_u32(&eocd[16], cdr_offset);
    pk3_put_u16(&eocd[20], 0);               /* comment length */
}

/* Build a 30-byte LFH for a STORED entry. extra and comment fields
 * are not written (zero-length). */
static void pk3_build_lfh(unsigned char lfh[PK3_LFH_SIZE],
                          uint32_t crc, uint32_t csize, uint32_t usize,
                          uint16_t name_len) {
    lfh[0] = 'P'; lfh[1] = 'K'; lfh[2] = 3; lfh[3] = 4;
    pk3_put_u16(&lfh[4], 20);                /* version needed: 2.0 */
    pk3_put_u16(&lfh[6], 0);                 /* general purpose flags */
    pk3_put_u16(&lfh[8], PK3_METHOD_STORED); /* compression method */
    pk3_put_u16(&lfh[10], 0);                /* DOS mod time */
    pk3_put_u16(&lfh[12], 0x0021);           /* DOS mod date: 1980-01-01 */
    pk3_put_u32(&lfh[14], crc);
    pk3_put_u32(&lfh[18], csize);
    pk3_put_u32(&lfh[22], usize);
    pk3_put_u16(&lfh[26], name_len);
    pk3_put_u16(&lfh[28], 0);                /* extra field length */
}

/* Build a 46-byte CDR entry header (variable-length name follows). */
static void pk3_build_cdr(unsigned char cdr[PK3_CDR_SIZE],
                          uint32_t crc, uint32_t csize, uint32_t usize,
                          uint16_t name_len, uint32_t lfh_offset) {
    cdr[0] = 'P'; cdr[1] = 'K'; cdr[2] = 1; cdr[3] = 2;
    pk3_put_u16(&cdr[4], 20);                /* version made by: 2.0 */
    pk3_put_u16(&cdr[6], 20);                /* version needed: 2.0 */
    pk3_put_u16(&cdr[8], 0);                 /* general purpose flags */
    pk3_put_u16(&cdr[10], PK3_METHOD_STORED);
    pk3_put_u16(&cdr[12], 0);                /* DOS mod time */
    pk3_put_u16(&cdr[14], 0x0021);           /* DOS mod date */
    pk3_put_u32(&cdr[16], crc);
    pk3_put_u32(&cdr[20], csize);
    pk3_put_u32(&cdr[24], usize);
    pk3_put_u16(&cdr[28], name_len);
    pk3_put_u16(&cdr[30], 0);                /* extra field length */
    pk3_put_u16(&cdr[32], 0);                /* comment length */
    pk3_put_u16(&cdr[34], 0);                /* disk number */
    pk3_put_u16(&cdr[36], 0);                /* internal attrs */
    pk3_put_u32(&cdr[38], 0);                /* external attrs */
    pk3_put_u32(&cdr[42], lfh_offset);
}

/* Write an empty EOCD at offset 0 of a freshly created PK3 or PK4. */
pakka_status_t pakka_pk3_create_impl(Pak_t *pak, pakka_error_t *err) {
    unsigned char eocd[PK3_EOCD_SIZE];
    int saved_errno;

    /* pak->format is set by the caller in src/pakfile.c (PK3 or PK4
     * depending on the requested format). */
    pak->max_decompressed = PK3_DEFAULT_MAX_DECOMPRESSED;
    pak->pk3_cdr_offset = 0;
    pak->num_entries = 0;
    pak->head = NULL;

    pk3_build_eocd(eocd, 0, 0, 0);
    if (fwrite(eocd, 1, PK3_EOCD_SIZE, pak->fp) != PK3_EOCD_SIZE) {
        saved_errno = errno;
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "create",
                            "Cannot write initial empty ZIP EOCD");
    }
    if (fflush(pak->fp) != 0) {
        saved_errno = errno;
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "create",
                            "Cannot flush initial empty ZIP");
    }
    return PAKKA_OK;
}

/* Preflight: allocate + fill an entry node, validate sizes, seek to
 * pk3_cdr_offset (overwriting any prior CDR+EOCD), and write
 * LFH + name. Does NOT yet link the entry or mark dirty — the caller
 * still has to stream the payload, and we want to roll back cleanly
 * if THAT fails. On success, *out_entry is the unlinked node; the
 * caller fills in pk3_cdr_offset and links it after payload succeeds.
 * On failure, no on-disk state changes that aren't restored. */
static pakka_status_t pk3_prepare_stored_entry(Pak_t *pak,
                                               const char *entry_name,
                                               size_t name_len,
                                               uint32_t crc,
                                               uint64_t payload_len,
                                               Pakfileentry_t **out_entry,
                                               pakka_error_t *err) {
    unsigned char lfh[PK3_LFH_SIZE];
    Pakfileentry_t *entry;
    uint32_t lfh_offset = pak->pk3_cdr_offset;
    uint64_t new_eof;
    int saved_errno;

    *out_entry = NULL;

    /* ZIP non-ZIP64 limit: every per-entry size field is u32. */
    if (payload_len > 0xFFFFFFFFu) {
        return pk3_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "add",
                            "ZIP entry %s payload (%" PRIu64 " bytes) "
                            "exceeds 4 GiB ZIP non-ZIP64 limit",
                            entry_name, payload_len);
    }
    new_eof = (uint64_t)lfh_offset + (uint64_t)PK3_LFH_SIZE
            + (uint64_t)name_len + payload_len;
    if (new_eof > 0xFFFFFFFFu) {
        return pk3_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "add",
                            "ZIP would grow past 4 GiB non-ZIP64 limit");
    }

    /* Allocate before any file mutation so an ENOMEM here doesn't
     * leave a half-written LFH on disk. */
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return pk3_err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                            "add", "Cannot allocate ZIP entry struct");
    }
    memcpy(entry->filename, entry_name, name_len);
    entry->filename[name_len] = '\0';
    entry->pk3_method = PK3_METHOD_STORED;
    entry->pk3_crc32 = crc;
    entry->pk3_compressed_size = (uint32_t)payload_len;
    entry->length = (uint32_t)payload_len;
    entry->pk3_lfh_offset = lfh_offset;
    entry->pk3_payload_offset = lfh_offset + PK3_LFH_SIZE + (uint32_t)name_len;
    entry->offset = entry->pk3_payload_offset;

    if (pakka_compat_fseek(pak->fp, (int64_t)lfh_offset, SEEK_SET) != 0) {
        saved_errno = errno;
        free(entry);
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "add",
                            "Cannot seek to ZIP CDR start for LFH write");
    }

    pk3_build_lfh(lfh, crc, (uint32_t)payload_len, (uint32_t)payload_len,
                  (uint16_t)name_len);
    if (fwrite(lfh, 1, PK3_LFH_SIZE, pak->fp) != PK3_LFH_SIZE
        || fwrite(entry_name, 1, name_len, pak->fp) != name_len) {
        saved_errno = errno;
        free(entry);
        /* Disk state is partial. The in-memory list isn't mutated
         * because we haven't linked entry; pak->pk3_cdr_offset still
         * points at lfh_offset, so a retry will overwrite the partial
         * LFH cleanly. dirty remains as it was, so pakka_close won't
         * commit a broken archive on top of this failure. */
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "add",
                            "Cannot write ZIP LFH for %s", entry_name);
    }

    *out_entry = entry;
    return PAKKA_OK;
}

/* Link a freshly-written entry into the archive's list and advance
 * pk3_cdr_offset. Called only after payload write has succeeded. */
static void pk3_commit_stored_entry(Pak_t *pak, Pakfileentry_t *entry) {
    Pakfileentry_t *tail;
    if (pak->head == NULL) {
        pak->head = entry;
    } else {
        for (tail = pak->head; tail->next != NULL; tail = tail->next) { }
        tail->next = entry;
    }
    pak->num_entries++;
    pak->pk3_cdr_offset = entry->pk3_payload_offset + entry->pk3_compressed_size;
    pak->dirty = 1;
}

pakka_status_t pakka_pk3_add_file_impl(Pak_t *pak,
                                       const char *source_path,
                                       const char *entry_name,
                                       pakka_error_t *err) {
    FILE *src;
    unsigned char buf[PAKFILE_COPY_CHUNK];
    uint32_t crc = 0;
    size_t name_len = strlen(entry_name);
    uint64_t total = 0;
    size_t got;
    Pakfileentry_t *entry;
    pakka_status_t s;
    int saved_errno;

    if (name_len >= PAKKA_ENTRY_NAME_SIZE) {
        return pk3_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "add",
                            "ZIP entry name too long (%zu bytes, max %u)",
                            name_len, (unsigned)(PAKKA_ENTRY_NAME_SIZE - 1));
    }

    /* First pass: compute CRC32 and total size. total is uint64_t so
     * 32-bit hosts can't wrap before the 4 GiB cap check. */
    src = fopen(source_path, "rb");
    if (src == NULL) {
        saved_errno = errno;
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "add",
                            "Cannot open source %s", source_path);
    }
    while ((got = fread(buf, 1, sizeof(buf), src)) > 0) {
        crc = pk3_crc32_update(crc, buf, got);
        total += (uint64_t)got;
        if (total > 0xFFFFFFFFu) {
            fclose(src);
            return pk3_err_fill(err, PAKKA_ERR_LIMIT,
                                PAKKA_ERR_DOMAIN_NONE, 0, "add",
                                "Source file exceeds 4 GiB ZIP limit (%s)",
                                source_path);
        }
    }
    if (ferror(src)) {
        saved_errno = errno;
        fclose(src);
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "add",
                            "Read error on %s", source_path);
    }

    /* Two-phase write: prepare (allocate entry, write LFH+name),
     * stream payload, commit (link entry, advance pk3_cdr_offset). */
    s = pk3_prepare_stored_entry(pak, entry_name, name_len, crc, total,
                                 &entry, err);
    if (s != PAKKA_OK) {
        fclose(src);
        return s;
    }

    /* Second pass: stream payload from start of source. The prepare
     * helper left the file pointer at the payload offset. */
    if (pakka_compat_fseek(src, 0, SEEK_SET) != 0) {
        saved_errno = errno;
        free(entry);
        fclose(src);
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "add",
                            "Cannot rewind source %s", source_path);
    }
    while ((got = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, got, pak->fp) != got) {
            saved_errno = errno;
            free(entry);
            fclose(src);
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "add",
                                "Cannot stream payload to ZIP");
        }
    }
    if (ferror(src)) {
        saved_errno = errno;
        free(entry);
        fclose(src);
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "add",
                            "Read error on %s during payload streaming",
                            source_path);
    }
    fclose(src);

    pk3_commit_stored_entry(pak, entry);
    return PAKKA_OK;
}

pakka_status_t pakka_pk3_add_memory_impl(Pak_t *pak,
                                        const char *entry_name,
                                        const void *data, size_t len,
                                        pakka_error_t *err) {
    uint32_t crc;
    size_t name_len = strlen(entry_name);
    Pakfileentry_t *entry;
    pakka_status_t s;
    int saved_errno;

    if (name_len >= PAKKA_ENTRY_NAME_SIZE) {
        return pk3_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "add",
                            "ZIP entry name too long (%zu bytes, max %u)",
                            name_len, (unsigned)(PAKKA_ENTRY_NAME_SIZE - 1));
    }

    crc = (len > 0)
        ? pk3_crc32_update(0, (const unsigned char *)data, len)
        : 0;

    s = pk3_prepare_stored_entry(pak, entry_name, name_len, crc,
                                 (uint64_t)len, &entry, err);
    if (s != PAKKA_OK) return s;

    if (len > 0 && fwrite(data, 1, len, pak->fp) != len) {
        saved_errno = errno;
        free(entry);
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "add",
                            "Cannot write ZIP memory payload");
    }
    pk3_commit_stored_entry(pak, entry);
    return PAKKA_OK;
}

/* ------------------------------------------------------------------ */
/* Commit: rewrite CDR + EOCD at pak->pk3_cdr_offset.                 */
/* ------------------------------------------------------------------ */

pakka_status_t pakka_pk3_commit_impl(Pak_t *pak, pakka_error_t *err) {
    Pakfileentry_t *e;
    unsigned char cdr[PK3_CDR_SIZE];
    unsigned char eocd[PK3_EOCD_SIZE];
    uint32_t cdr_size = 0;
    uint32_t entry_count = 0;
    int saved_errno;

    if (!pak->dirty) {
        return PAKKA_OK;
    }

    /* Refuse to commit > 64K entries — ZIP non-ZIP64 caps the EOCD
     * count fields at u16. Check BEFORE writing any CDR bytes so a
     * failed commit doesn't leave a partial CDR on disk. */
    if (pak->num_entries > 0xFFFFu) {
        return pk3_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "commit",
                            "ZIP entry count (%u) exceeds 64 K "
                            "non-ZIP64 limit",
                            (unsigned)pak->num_entries);
    }

    /* Rebuild path (any pakka_delete touched the list): copy retained
     * LFH+payload bytes verbatim into a temp file in CDR order, then
     * rename the temp over the original. Method/CRC/sizes are
     * preserved exactly — no re-inflate, no re-encode.
     *
     * For an is_new archive, the rebuild temp replaces pak->tmp_pakpath
     * (NOT pak->new_pakpath) — pakka_close still owns the final
     * tmp → new_pakpath rename. Mirrors the PAK rebuild flow. */
    if (pak->needs_rebuild) {
        FILE *tmpfp;
        char tmp_path[OS_PATH_MAX];
        unsigned char copy_buf[PAKFILE_COPY_CHUNK];
        uint32_t cur_offset;
        const char *dest_for_rename = pak->is_new ? pak->tmp_pakpath
                                                  : pak->cur_pakpath;

        if (! (tmpfp = pakka_compat_mkstemp_open(dest_for_rename,
                                              "pakkaXXXXXX",
                                              tmp_path,
                                              sizeof(tmp_path)))) {
            saved_errno = errno;
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot create ZIP rebuild temp file");
        }

        cur_offset = 0;
        for (e = pak->head; e != NULL; e = e->next) {
            unsigned char lfh[PK3_LFH_SIZE];
            uint64_t remaining;

            /* Build a fresh LFH at the new offset; sizes/CRC are
             * verbatim from the entry. Name length comes from
             * strlen(filename). */
            uint16_t name_len = (uint16_t)strlen(e->filename);
            pk3_build_lfh(lfh, e->pk3_crc32, e->pk3_compressed_size,
                          e->length, name_len);
            if (fwrite(lfh, 1, PK3_LFH_SIZE, tmpfp) != PK3_LFH_SIZE
                || fwrite(e->filename, 1, name_len, tmpfp) != name_len) {
                saved_errno = errno;
                fclose(tmpfp);
                remove(tmp_path);
                return pk3_err_fill(err, PAKKA_ERR_IO,
                                    PAKKA_ERR_DOMAIN_ERRNO,
                                    (uint32_t)saved_errno, "commit",
                                    "Cannot write rebuild LFH for %s",
                                    e->filename);
            }

            /* Stream payload bytes from the original file. */
            if (pakka_compat_fseek(pak->fp, (int64_t)e->pk3_payload_offset, SEEK_SET) != 0) {
                saved_errno = errno;
                fclose(tmpfp);
                remove(tmp_path);
                return pk3_err_fill(err, PAKKA_ERR_IO,
                                    PAKKA_ERR_DOMAIN_ERRNO,
                                    (uint32_t)saved_errno, "commit",
                                    "Cannot seek source for rebuild");
            }
            remaining = e->pk3_compressed_size;
            while (remaining > 0) {
                size_t want = remaining > sizeof(copy_buf)
                    ? sizeof(copy_buf) : (size_t)remaining;
                size_t got = fread(copy_buf, 1, want, pak->fp);
                if (got != want) {
                    saved_errno = errno;
                    fclose(tmpfp);
                    remove(tmp_path);
                    return pk3_err_fill(err, PAKKA_ERR_IO,
                                        PAKKA_ERR_DOMAIN_ERRNO,
                                        (uint32_t)saved_errno, "commit",
                                        "Short read on rebuild for %s",
                                        e->filename);
                }
                if (fwrite(copy_buf, 1, got, tmpfp) != got) {
                    saved_errno = errno;
                    fclose(tmpfp);
                    remove(tmp_path);
                    return pk3_err_fill(err, PAKKA_ERR_IO,
                                        PAKKA_ERR_DOMAIN_ERRNO,
                                        (uint32_t)saved_errno, "commit",
                                        "Short write on rebuild for %s",
                                        e->filename);
                }
                remaining -= (uint64_t)got;
            }

            /* Update the in-memory entry to point at the new file's
             * offsets — the old ones become invalid once we rename. */
            e->pk3_lfh_offset = cur_offset;
            e->pk3_payload_offset = cur_offset + PK3_LFH_SIZE
                                  + (uint32_t)name_len;
            e->offset = e->pk3_payload_offset;
            cur_offset = e->pk3_payload_offset + e->pk3_compressed_size;
        }

        pak->pk3_cdr_offset = cur_offset;

        /* Close BOTH file handles before renaming. Windows CRT fopen
         * omits FILE_SHARE_DELETE so MoveFileExA blocks while either
         * handle is live. Check fclose() errors before the rename so
         * buffered-write failures on either file surface here rather
         * than after we've published the temp. */
        if (fclose(tmpfp) != 0) {
            saved_errno = errno;
            remove(tmp_path);
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot close ZIP rebuild temp before rename");
        }
        if (fclose(pak->fp) != 0) {
            saved_errno = errno;
            remove(tmp_path);
            pak->fp = NULL;
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot close original ZIP before rename");
        }
        pak->fp = NULL;
        {
            uint32_t win32_code = 0;
            /* For is_new: rename rebuild temp over the create temp
             * (pak->tmp_pakpath). pakka_close finishes the
             * tmp_pakpath → new_pakpath rename. For an existing
             * archive: rename rebuild temp over cur_pakpath. */
            int rc = pak->is_new
                ? pakka_compat_rename_replace(tmp_path, pak->tmp_pakpath, &win32_code)
                : pakka_compat_rename_replace(tmp_path, pak->cur_pakpath, &win32_code);
            if (rc != 0) {
                saved_errno = errno;
                remove(tmp_path);
                return pk3_err_fill(err, PAKKA_ERR_IO,
                                    win32_code ? PAKKA_ERR_DOMAIN_WIN32
                                               : PAKKA_ERR_DOMAIN_ERRNO,
                                    win32_code ? win32_code
                                               : (uint32_t)saved_errno,
                                    "commit",
                                    "Cannot rename ZIP rebuild temp");
            }
        }
        /* Re-open the renamed file for further reads. */
        pak->fp = fopen(pak->is_new ? pak->tmp_pakpath : pak->cur_pakpath,
                        "r+b");
        if (pak->fp == NULL) {
            saved_errno = errno;
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot reopen committed ZIP");
        }
        /* Seek to where CDR will be written below. */
        if (pakka_compat_fseek(pak->fp, (int64_t)pak->pk3_cdr_offset, SEEK_SET) != 0) {
            saved_errno = errno;
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot seek to CDR start after rebuild");
        }
        pak->needs_rebuild = 0;
    } else {
        /* Add-only path: file already has fresh LFHs at the correct
         * offsets. Just seek to pk3_cdr_offset to overwrite any stale
         * CDR+EOCD. */
        if (pakka_compat_fseek(pak->fp, (int64_t)pak->pk3_cdr_offset, SEEK_SET) != 0) {
            saved_errno = errno;
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot seek to ZIP CDR start");
        }
    }

    /* Earlier LFH/payload placement checks bound each entry, but
     * commit appends CDR + 22-byte EOCD on top. Reject before any CDR
     * byte hits disk; a half-written CDR would leave the EOCD scanner
     * pointing at garbage. */
    {
        uint64_t total = (uint64_t)pak->pk3_cdr_offset
                         + (uint64_t)PK3_EOCD_SIZE;
        for (e = pak->head; e != NULL; e = e->next) {
            total += (uint64_t)PK3_CDR_SIZE
                     + (uint64_t)strlen(e->filename);
            if (total > UINT32_MAX) {
                return pk3_err_fill(err, PAKKA_ERR_LIMIT,
                                    PAKKA_ERR_DOMAIN_NONE, 0,
                                    "commit",
                                    "ZIP CDR + EOCD would push file "
                                    "past 4 GiB");
            }
        }
    }

    /* Write the central directory. */
    for (e = pak->head; e != NULL; e = e->next) {
        uint16_t name_len = (uint16_t)strlen(e->filename);
        pk3_build_cdr(cdr, e->pk3_crc32, e->pk3_compressed_size,
                      e->length, name_len, e->pk3_lfh_offset);
        if (fwrite(cdr, 1, PK3_CDR_SIZE, pak->fp) != PK3_CDR_SIZE
            || fwrite(e->filename, 1, name_len, pak->fp) != name_len) {
            saved_errno = errno;
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot write ZIP CDR entry %s",
                                e->filename);
        }
        cdr_size += PK3_CDR_SIZE + name_len;
        entry_count++;
    }
    /* Entry-count check ran up-front before any CDR bytes were written. */
    pk3_build_eocd(eocd, pak->pk3_cdr_offset, cdr_size,
                   (uint16_t)entry_count);
    if (fwrite(eocd, 1, PK3_EOCD_SIZE, pak->fp) != PK3_EOCD_SIZE) {
        saved_errno = errno;
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "commit",
                            "Cannot write ZIP EOCD");
    }
    if (fflush(pak->fp) != 0) {
        saved_errno = errno;
        return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "commit",
                            "Cannot flush ZIP commit");
    }

    /* Drop any bytes past the new EOCD. An EOCD comment from the
     * pre-commit archive (or a longer prior CDR after deletes) can
     * outlive the new EOCD record, and pk3_find_eocd scans for
     * EOCD+comment ending exactly at EOF — without truncation, reopen
     * would find the stale tail instead of the new EOCD. */
    {
        int64_t new_eof = (int64_t)pak->pk3_cdr_offset
                        + (int64_t)cdr_size
                        + (int64_t)PK3_EOCD_SIZE;
        if (pakka_compat_ftruncate(pak->fp, new_eof) != 0) {
            saved_errno = errno;
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot truncate ZIP after EOCD");
        }
        pak->file_size = (uint64_t)new_eof;
    }

    pak->dirty = 0;
    return PAKKA_OK;
}

