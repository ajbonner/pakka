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

/* Precomputed IEEE 802.3 CRC32 table (polynomial 0xEDB88320, reflected).
 * Inlined as a const so first use is allocation-free and concurrent
 * pakka_open / pakka_pk3_add_file calls can't race on a lazy build.
 * Generated once via:
 *   for i in 0..255: c = i; 8x: c = c&1 ? 0xEDB88320 ^ (c>>1) : c>>1 */
static const uint32_t pk3_crc32_table[PK3_CRC32_TABLE_SIZE] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau,
    0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
    0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u,
    0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
    0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu,
    0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu,
    0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
    0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u,
    0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u,
    0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u,
    0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u,
    0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
    0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au,
    0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u,
    0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu,
    0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
    0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu,
    0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u,
    0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
    0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u,
    0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u,
    0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u,
    0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
    0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au,
    0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
    0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u,
    0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu,
    0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
    0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu,
    0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u,
    0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u,
    0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
    0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u,
    0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
    0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u,
    0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au,
    0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u,
    0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
    0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu,
    0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu,
    0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u,
    0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
    0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u,
    0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u,
    0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
    0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u,
    0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

static uint32_t pk3_crc32_update(uint32_t crc,
                                 const unsigned char *buf, size_t len) {
    size_t i;
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
        /* Reject embedded NUL or control bytes across the FULL CDR
         * name length, not just the C-string view. Without this, a
         * crafted "good.txt\0../escape" would NUL-terminate to
         * "good.txt" for the in-memory list, but duplicate checks /
         * verify / extract would all see the truncated form while
         * the on-disk CDR record carries the full evil name. Cutoff
         * matches pakka_unsafe_entry_name + the CLI sanitizer: any
         * byte < 0x20 or == 0x7F is a control byte; high-bit bytes
         * (>= 0x80) are allowed since ZIP names may carry UTF-8 /
         * CP437. */
        {
            uint16_t k;
            for (k = 0; k < name_len; k++) {
                unsigned char b = cdr[pos + PK3_CDR_SIZE + k];
                if (b < 0x20 || b == 0x7F) {
                    pakka_entry_free(entry);
                    free(cdr);
                    return pk3_err_fill(err, PAKKA_ERR_FORMAT,
                                        PAKKA_ERR_DOMAIN_NONE, 0, "open",
                                        "ZIP entry %u name contains "
                                        "control byte 0x%02x at "
                                        "position %u",
                                        i, (unsigned)b, (unsigned)k);
                }
            }
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

    /* Pending (queued-add) entry: payload bytes aren't on pak->fp yet
     * — they live in pk3_pending_source (file path) or pk3_pending_data
     * (in-memory buffer). Reuse the inflated_buf machinery to serve
     * the bytes through pakka_reader_read; the buffer is freed by
     * pakka_reader_close just like DEFLATE inflated output. */
    if (entry->pk3_pending_source != NULL
        || entry->pk3_pending_data != NULL) {
        unsigned char *buf;
        uint32_t plen = entry->pk3_compressed_size;
        if (plen == 0) {
            /* Empty payload: leave inflated_buf NULL and report
             * zero remaining bytes; reader_read returns 0 immediately. */
            reader->remaining = 0;
            return PAKKA_OK;
        }
        buf = malloc(plen);
        if (buf == NULL) {
            return pk3_err_fill(err, PAKKA_ERR_NOMEM,
                                PAKKA_ERR_DOMAIN_NONE, 0, "open_entry",
                                "Cannot allocate pending-entry buffer "
                                "for %s (%u bytes)",
                                entry->filename, (unsigned)plen);
        }
        if (entry->pk3_pending_source != NULL) {
            /* Same source-side hardening commit applies — refuse to
             * follow a symlink or read from a non-regular file even
             * for in-process read-after-add. Distinguish stat failure
             * (PAKKA_ERR_IO with the underlying errno — source might
             * have been deleted or permission-revoked) from a found
             * symlink / non-regular file (PAKKA_ERR_UNSAFE_NAME). */
            {
                struct stat sb;
                if (pakka_compat_is_reparse_or_symlink(
                        entry->pk3_pending_source)) {
                    free(buf);
                    return pk3_err_fill(err, PAKKA_ERR_UNSAFE_NAME,
                                        PAKKA_ERR_DOMAIN_NONE, 0,
                                        "open_entry",
                                        "Pending source %s is now a "
                                        "symlink/reparse point",
                                        entry->pk3_pending_source);
                }
                if (pakka_compat_lstat(
                        entry->pk3_pending_source, &sb) != 0) {
                    int saved_errno = errno;
                    free(buf);
                    return pk3_err_fill(err, PAKKA_ERR_IO,
                                        PAKKA_ERR_DOMAIN_ERRNO,
                                        (uint32_t)saved_errno,
                                        "open_entry",
                                        "Cannot stat pending source %s",
                                        entry->pk3_pending_source);
                }
                if (!S_ISREG(sb.st_mode)) {
                    free(buf);
                    return pk3_err_fill(err, PAKKA_ERR_UNSAFE_NAME,
                                        PAKKA_ERR_DOMAIN_NONE, 0,
                                        "open_entry",
                                        "Pending source %s is no longer "
                                        "a regular file",
                                        entry->pk3_pending_source);
                }
            }
            {
                FILE *src = fopen(entry->pk3_pending_source, "rb");
                if (src == NULL) {
                    int saved_errno = errno;
                    free(buf);
                    return pk3_err_fill(err, PAKKA_ERR_IO,
                                        PAKKA_ERR_DOMAIN_ERRNO,
                                        (uint32_t)saved_errno, "open_entry",
                                        "Cannot reopen pending source %s",
                                        entry->pk3_pending_source);
                }
                if (fread(buf, 1, plen, src) != plen) {
                    int saved_errno = errno;
                    fclose(src);
                    free(buf);
                    return pk3_err_fill(err, PAKKA_ERR_IO,
                                        PAKKA_ERR_DOMAIN_ERRNO,
                                        (uint32_t)saved_errno, "open_entry",
                                        "Short read from pending source %s",
                                        entry->pk3_pending_source);
                }
                fclose(src);
            }
        } else {
            memcpy(buf, entry->pk3_pending_data, plen);
        }
        reader->inflated_buf = buf;
        reader->inflated_len = plen;
        reader->inflated_cursor = 0;
        return PAKKA_OK;
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

    /* Pending (queued-add) entry: payload isn't on pak->fp yet. The
     * CRC32 was computed at add time; with no on-disk bytes to
     * cross-check there's nothing meaningful for deep verify to do.
     * Skip cleanly — caller can re-run after commit. */
    if (entry->pk3_pending_source != NULL
        || entry->pk3_pending_data != NULL) {
        (void)pak;
        return PAKKA_OK;
    }

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

/* Allocate + fill an entry with ZIP STORED metadata, leaving on-disk
 * offsets as 0. Called from add_file / add_memory. The caller is
 * responsible for stashing the payload source (pk3_pending_source for
 * a file, pk3_pending_data for an in-memory buffer) and linking the
 * entry into pak->head. Sets dirty + needs_rebuild on the archive so
 * commit drives the temp-file rebuild that actually publishes the
 * bytes. Atomic-add semantics: pak->fp is never mutated until commit
 * succeeds and the temp file is renamed over the original. */
static pakka_status_t pk3_alloc_pending_entry(Pak_t *pak,
                                              const char *entry_name,
                                              size_t name_len,
                                              uint32_t crc,
                                              uint64_t payload_len,
                                              Pakfileentry_t **out_entry,
                                              pakka_error_t *err) {
    Pakfileentry_t *entry;
    *out_entry = NULL;
    (void)pak;

    /* ZIP non-ZIP64 limit: every per-entry size field is u32. */
    if (payload_len > 0xFFFFFFFFu) {
        return pk3_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "add",
                            "ZIP entry %s payload (%" PRIu64 " bytes) "
                            "exceeds 4 GiB ZIP non-ZIP64 limit",
                            entry_name, payload_len);
    }

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
    /* lfh / payload offsets are computed by the rebuild commit loop —
     * leave them 0 until then. Reads against a pending entry through
     * pak->fp would point at the wrong file, so pakka_open_entry must
     * also skip pending entries. */

    *out_entry = entry;
    return PAKKA_OK;
}

/* Link a queued entry into the in-memory list (appended at the tail
 * to preserve add order in the committed CDR), then mark the archive
 * dirty + needing rebuild. */
static void pk3_link_pending_entry(Pak_t *pak, Pakfileentry_t *entry) {
    Pakfileentry_t *tail;
    if (pak->head == NULL) {
        pak->head = entry;
    } else {
        for (tail = pak->head; tail->next != NULL; tail = tail->next) { }
        tail->next = entry;
    }
    pak->num_entries++;
    pak->dirty = 1;
    pak->needs_rebuild = 1;
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

    /* Scan the source to compute CRC32 and size up-front. ZIP non-
     * ZIP64 caps every per-entry size field at u32; total is uint64_t
     * so 32-bit hosts can't silently wrap before the cap check. The
     * payload bytes themselves are read again at commit time — atomic
     * add cost is one extra full read per source file. */
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
    fclose(src);

    s = pk3_alloc_pending_entry(pak, entry_name, name_len, crc, total,
                                &entry, err);
    if (s != PAKKA_OK) return s;

    entry->pk3_pending_source = pakka_compat_strdup(source_path);
    if (entry->pk3_pending_source == NULL) {
        pakka_entry_free(entry);
        return pk3_err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                            "add", "Cannot store pending source path");
    }
    pk3_link_pending_entry(pak, entry);
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

    if (name_len >= PAKKA_ENTRY_NAME_SIZE) {
        return pk3_err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                            "add",
                            "ZIP entry name too long (%zu bytes, max %u)",
                            name_len, (unsigned)(PAKKA_ENTRY_NAME_SIZE - 1));
    }

    crc = (len > 0)
        ? pk3_crc32_update(0, (const unsigned char *)data, len)
        : 0;

    s = pk3_alloc_pending_entry(pak, entry_name, name_len, crc,
                                (uint64_t)len, &entry, err);
    if (s != PAKKA_OK) return s;

    /* Take an owned copy of the caller's buffer. The caller is free
     * to mutate or free `data` after this returns; commit reads from
     * the copy. malloc(0) is implementation-defined, so allocate a
     * zero-sized payload via NULL — pk3_pending_data_len == 0 is the
     * marker for an empty pending entry. */
    if (len > 0) {
        entry->pk3_pending_data = malloc(len);
        if (entry->pk3_pending_data == NULL) {
            pakka_entry_free(entry);
            return pk3_err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                                "add", "Cannot copy memory payload");
        }
        memcpy(entry->pk3_pending_data, data, len);
        entry->pk3_pending_data_len = len;
    } else {
        /* Empty payload still needs the "pending" mark so commit's
         * rebuild loop knows not to fall through to the live-stream
         * branch. Use a non-NULL sentinel via a single zero byte. */
        entry->pk3_pending_data = malloc(1);
        if (entry->pk3_pending_data == NULL) {
            pakka_entry_free(entry);
            return pk3_err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                                "add", "Cannot tag empty pending entry");
        }
        ((char *)entry->pk3_pending_data)[0] = '\0';
        entry->pk3_pending_data_len = 0;
    }
    pk3_link_pending_entry(pak, entry);
    return PAKKA_OK;
}

/* ------------------------------------------------------------------ */
/* Commit: rewrite CDR + EOCD at pak->pk3_cdr_offset.                 */
/* ------------------------------------------------------------------ */

/* Restore the per-entry offset triplet plus pk3_cdr_offset from a
 * snapshot taken before the rebuild copy loop began. Called on any
 * pre-rename failure so the in-memory archive layout still matches
 * the unchanged on-disk file. Iterates by list order — the snapshot
 * was taken in list order and the list isn't reshuffled during the
 * copy. */
static void pk3_rebuild_rollback_offsets(Pak_t *pak,
                                         const uint32_t *old_lfh,
                                         const uint32_t *old_payload,
                                         const uint32_t *old_offset_field,
                                         uint32_t old_cdr_offset) {
    Pakfileentry_t *e;
    size_t i = 0;
    for (e = pak->head; e != NULL; e = e->next) {
        e->pk3_lfh_offset = old_lfh[i];
        e->pk3_payload_offset = old_payload[i];
        e->offset = old_offset_field[i];
        i++;
    }
    pak->pk3_cdr_offset = old_cdr_offset;
}

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
     * tmp → new_pakpath rename. Mirrors the PAK rebuild flow.
     *
     * Rollback semantics: in-memory entry offsets are snapshotted
     * before the copy loop. Any failure BEFORE the rename succeeds
     * restores the snapshot so the caller (and a subsequent open)
     * still sees the pre-commit layout. After the rename succeeds the
     * temp IS the canonical file, so post-publish failures (reopen,
     * post-rebuild seek) keep the new offsets — restoring would lie
     * about what's on disk. */
    if (pak->needs_rebuild) {
        FILE *tmpfp;
        char tmp_path[OS_PATH_MAX];
        unsigned char copy_buf[PAKFILE_COPY_CHUNK];
        uint32_t cur_offset;
        uint32_t *old_lfh = NULL;
        uint32_t *old_payload = NULL;
        uint32_t *old_offset_field = NULL;
        uint32_t old_cdr_offset = pak->pk3_cdr_offset;
        size_t n_entries = 0;
        size_t i;
        const char *dest_for_rename = pak->is_new ? pak->tmp_pakpath
                                                  : pak->cur_pakpath;

        for (e = pak->head; e != NULL; e = e->next) {
            n_entries++;
        }
        if (n_entries > 0) {
            old_lfh = malloc(sizeof(*old_lfh) * n_entries);
            old_payload = malloc(sizeof(*old_payload) * n_entries);
            old_offset_field = malloc(sizeof(*old_offset_field)
                                      * n_entries);
            if (old_lfh == NULL || old_payload == NULL
                || old_offset_field == NULL) {
                free(old_lfh);
                free(old_payload);
                free(old_offset_field);
                return pk3_err_fill(err, PAKKA_ERR_NOMEM,
                                    PAKKA_ERR_DOMAIN_NONE, 0, "commit",
                                    "Cannot allocate ZIP rebuild "
                                    "rollback buffers");
            }
            i = 0;
            for (e = pak->head; e != NULL; e = e->next) {
                old_lfh[i] = e->pk3_lfh_offset;
                old_payload[i] = e->pk3_payload_offset;
                old_offset_field[i] = e->offset;
                i++;
            }
        }

        if (! (tmpfp = pakka_compat_mkstemp_open(dest_for_rename,
                                              "pakkaXXXXXX",
                                              tmp_path,
                                              sizeof(tmp_path)))) {
            saved_errno = errno;
            free(old_lfh);
            free(old_payload);
            free(old_offset_field);
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot create ZIP rebuild temp file");
        }

        cur_offset = 0;
        for (e = pak->head; e != NULL; e = e->next) {
            unsigned char lfh[PK3_LFH_SIZE];
            uint64_t remaining;
            uint64_t per_entry_extent;

            /* Build a fresh LFH at the new offset; sizes/CRC are
             * verbatim from the entry. Name length comes from
             * strlen(filename). */
            uint16_t name_len = (uint16_t)strlen(e->filename);

            /* Per-entry size check against the u32 cap. cur_offset is
             * u32; without this guard a sequence of pending adds whose
             * lfh + name + payload exceeds 4 GiB would silently wrap
             * and emit overlapping LFH offsets. */
            per_entry_extent = (uint64_t)PK3_LFH_SIZE
                             + (uint64_t)name_len
                             + (uint64_t)e->pk3_compressed_size;
            if ((uint64_t)cur_offset + per_entry_extent > 0xFFFFFFFFu) {
                pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                             old_offset_field,
                                             old_cdr_offset);
                fclose(tmpfp);
                remove(tmp_path);
                free(old_lfh);
                free(old_payload);
                free(old_offset_field);
                return pk3_err_fill(err, PAKKA_ERR_LIMIT,
                                    PAKKA_ERR_DOMAIN_NONE, 0, "commit",
                                    "ZIP payload section would push past "
                                    "4 GiB non-ZIP64 limit at entry %s",
                                    e->filename);
            }

            pk3_build_lfh(lfh, e->pk3_crc32, e->pk3_compressed_size,
                          e->length, name_len);
            if (fwrite(lfh, 1, PK3_LFH_SIZE, tmpfp) != PK3_LFH_SIZE
                || fwrite(e->filename, 1, name_len, tmpfp) != name_len) {
                saved_errno = errno;
                pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                             old_offset_field,
                                             old_cdr_offset);
                fclose(tmpfp);
                remove(tmp_path);
                free(old_lfh);
                free(old_payload);
                free(old_offset_field);
                return pk3_err_fill(err, PAKKA_ERR_IO,
                                    PAKKA_ERR_DOMAIN_ERRNO,
                                    (uint32_t)saved_errno, "commit",
                                    "Cannot write rebuild LFH for %s",
                                    e->filename);
            }

            /* Stream payload bytes. Three sources, in priority order:
             *   1. pk3_pending_source != NULL — queued file add, open
             *      the source path and stream from it
             *   2. pk3_pending_data != NULL — queued memory add, copy
             *      from the buffer (length lives in pk3_pending_data_len;
             *      a 1-byte sentinel buffer represents a 0-byte payload)
             *   3. neither set — entry from the on-disk archive, stream
             *      from pak->fp at its current pk3_payload_offset
             *
             * Cases 1 and 2 are how H2's atomic add works: pak->fp
             * isn't mutated until commit succeeds and the temp file is
             * renamed over the original. */
            if (e->pk3_pending_source != NULL) {
                /* Re-run the source-side hardening that ran at add
                 * time. The source path is caller-supplied and might
                 * have been swapped for a symlink between add and
                 * commit (TOCTOU). A clean failure here beats writing
                 * the target's bytes under the original entry name. */
                {
                    struct stat sb;
                    /* Same three-way split as open_entry: symlink ->
                     * UNSAFE_NAME, stat failure -> IO/ERRNO, non-regular
                     * -> UNSAFE_NAME. */
                    if (pakka_compat_is_reparse_or_symlink(
                            e->pk3_pending_source)) {
                        pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                     old_offset_field,
                                                     old_cdr_offset);
                        fclose(tmpfp);
                        remove(tmp_path);
                        free(old_lfh);
                        free(old_payload);
                        free(old_offset_field);
                        return pk3_err_fill(err, PAKKA_ERR_UNSAFE_NAME,
                                            PAKKA_ERR_DOMAIN_NONE, 0,
                                            "commit",
                                            "Pending source %s for %s "
                                            "is now a symlink/reparse "
                                            "point",
                                            e->pk3_pending_source,
                                            e->filename);
                    }
                    if (pakka_compat_lstat(e->pk3_pending_source,
                                           &sb) != 0) {
                        saved_errno = errno;
                        pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                     old_offset_field,
                                                     old_cdr_offset);
                        fclose(tmpfp);
                        remove(tmp_path);
                        free(old_lfh);
                        free(old_payload);
                        free(old_offset_field);
                        return pk3_err_fill(err, PAKKA_ERR_IO,
                                            PAKKA_ERR_DOMAIN_ERRNO,
                                            (uint32_t)saved_errno,
                                            "commit",
                                            "Cannot stat pending source "
                                            "%s for %s",
                                            e->pk3_pending_source,
                                            e->filename);
                    }
                    if (!S_ISREG(sb.st_mode)) {
                        pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                     old_offset_field,
                                                     old_cdr_offset);
                        fclose(tmpfp);
                        remove(tmp_path);
                        free(old_lfh);
                        free(old_payload);
                        free(old_offset_field);
                        return pk3_err_fill(err, PAKKA_ERR_UNSAFE_NAME,
                                            PAKKA_ERR_DOMAIN_NONE, 0,
                                            "commit",
                                            "Pending source %s for %s "
                                            "is no longer a regular "
                                            "file",
                                            e->pk3_pending_source,
                                            e->filename);
                    }
                    /* Reject a source whose total size differs from
                     * what add saw. Catches the grow-with-same-prefix
                     * case the CRC alone misses: a source that grew
                     * keeps the same first N bytes (and CRC) but the
                     * archive would silently publish a truncated copy
                     * of the new file. */
                    if ((uint64_t)sb.st_size
                        != (uint64_t)e->pk3_compressed_size) {
                        pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                     old_offset_field,
                                                     old_cdr_offset);
                        fclose(tmpfp);
                        remove(tmp_path);
                        free(old_lfh);
                        free(old_payload);
                        free(old_offset_field);
                        return pk3_err_fill(err, PAKKA_ERR_FORMAT,
                                            PAKKA_ERR_DOMAIN_NONE, 0,
                                            "commit",
                                            "Pending source %s for %s "
                                            "changed size between add "
                                            "and commit (now %lld, was %u)",
                                            e->pk3_pending_source,
                                            e->filename,
                                            (long long)sb.st_size,
                                            (unsigned)e->pk3_compressed_size);
                    }
                }
                {
                    FILE *src = fopen(e->pk3_pending_source, "rb");
                    uint32_t recomputed_crc = 0;
                    if (src == NULL) {
                        saved_errno = errno;
                        pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                     old_offset_field,
                                                     old_cdr_offset);
                        fclose(tmpfp);
                        remove(tmp_path);
                        free(old_lfh);
                        free(old_payload);
                        free(old_offset_field);
                        return pk3_err_fill(err, PAKKA_ERR_IO,
                                            PAKKA_ERR_DOMAIN_ERRNO,
                                            (uint32_t)saved_errno, "commit",
                                            "Cannot reopen pending source %s "
                                            "for %s",
                                            e->pk3_pending_source,
                                            e->filename);
                    }
                    remaining = e->pk3_compressed_size;
                    while (remaining > 0) {
                        size_t want = remaining > sizeof(copy_buf)
                            ? sizeof(copy_buf) : (size_t)remaining;
                        size_t got = fread(copy_buf, 1, want, src);
                        if (got != want
                            || fwrite(copy_buf, 1, got, tmpfp) != got) {
                            saved_errno = errno;
                            fclose(src);
                            pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                         old_offset_field,
                                                         old_cdr_offset);
                            fclose(tmpfp);
                            remove(tmp_path);
                            free(old_lfh);
                            free(old_payload);
                            free(old_offset_field);
                            return pk3_err_fill(err, PAKKA_ERR_IO,
                                                PAKKA_ERR_DOMAIN_ERRNO,
                                                (uint32_t)saved_errno, "commit",
                                                "Stream from pending source "
                                                "%s failed for %s",
                                                e->pk3_pending_source,
                                                e->filename);
                        }
                        recomputed_crc = pk3_crc32_update(recomputed_crc,
                                                         copy_buf,
                                                         (size_t)got);
                        remaining -= (uint64_t)got;
                    }
                    fclose(src);
                    /* Refuse to publish stale-source bytes under the
                     * CRC we stored at add time. Catches a source file
                     * modified between add and commit — without this
                     * the archive committed valid bytes paired with a
                     * stale CRC, and downstream verifiers would flag
                     * it as corrupt. */
                    if (recomputed_crc != e->pk3_crc32) {
                        pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                     old_offset_field,
                                                     old_cdr_offset);
                        fclose(tmpfp);
                        remove(tmp_path);
                        free(old_lfh);
                        free(old_payload);
                        free(old_offset_field);
                        return pk3_err_fill(err, PAKKA_ERR_FORMAT,
                                            PAKKA_ERR_DOMAIN_NONE, 0,
                                            "commit",
                                            "Pending source %s for %s "
                                            "changed between add and "
                                            "commit (CRC 0x%08x vs 0x%08x)",
                                            e->pk3_pending_source,
                                            e->filename,
                                            recomputed_crc, e->pk3_crc32);
                    }
                }
            } else if (e->pk3_pending_data != NULL) {
                if (e->pk3_pending_data_len > 0
                    && fwrite(e->pk3_pending_data, 1,
                              e->pk3_pending_data_len, tmpfp)
                       != e->pk3_pending_data_len) {
                    saved_errno = errno;
                    pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                 old_offset_field,
                                                 old_cdr_offset);
                    fclose(tmpfp);
                    remove(tmp_path);
                    free(old_lfh);
                    free(old_payload);
                    free(old_offset_field);
                    return pk3_err_fill(err, PAKKA_ERR_IO,
                                        PAKKA_ERR_DOMAIN_ERRNO,
                                        (uint32_t)saved_errno, "commit",
                                        "Write pending data failed for %s",
                                        e->filename);
                }
            } else {
                if (pakka_compat_fseek(pak->fp, (int64_t)e->pk3_payload_offset, SEEK_SET) != 0) {
                    saved_errno = errno;
                    pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                 old_offset_field,
                                                 old_cdr_offset);
                    fclose(tmpfp);
                    remove(tmp_path);
                    free(old_lfh);
                    free(old_payload);
                    free(old_offset_field);
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
                        pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                     old_offset_field,
                                                     old_cdr_offset);
                        fclose(tmpfp);
                        remove(tmp_path);
                        free(old_lfh);
                        free(old_payload);
                        free(old_offset_field);
                        return pk3_err_fill(err, PAKKA_ERR_IO,
                                            PAKKA_ERR_DOMAIN_ERRNO,
                                            (uint32_t)saved_errno, "commit",
                                            "Short read on rebuild for %s",
                                            e->filename);
                    }
                    if (fwrite(copy_buf, 1, got, tmpfp) != got) {
                        saved_errno = errno;
                        pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                     old_offset_field,
                                                     old_cdr_offset);
                        fclose(tmpfp);
                        remove(tmp_path);
                        free(old_lfh);
                        free(old_payload);
                        free(old_offset_field);
                        return pk3_err_fill(err, PAKKA_ERR_IO,
                                            PAKKA_ERR_DOMAIN_ERRNO,
                                            (uint32_t)saved_errno, "commit",
                                            "Short write on rebuild for %s",
                                            e->filename);
                    }
                    remaining -= (uint64_t)got;
                }
            }

            /* Update the in-memory entry to point at the new file's
             * offsets. Snapshot in old_*[] still holds the pre-commit
             * values so a later pre-rename failure can restore them. */
            e->pk3_lfh_offset = cur_offset;
            e->pk3_payload_offset = cur_offset + PK3_LFH_SIZE
                                  + (uint32_t)name_len;
            e->offset = e->pk3_payload_offset;
            cur_offset = e->pk3_payload_offset + e->pk3_compressed_size;
        }

        pak->pk3_cdr_offset = cur_offset;

        /* Write CDR + EOCD into the temp BEFORE renaming. Pre-rename
         * placement means a failure here rolls back cleanly via the
         * existing rollback path (offsets restored, temp removed,
         * original archive untouched). The previous design wrote
         * CDR/EOCD into pak->fp AFTER the rename, leaving the on-disk
         * archive incomplete if any of the writes failed — a
         * disk-full event at the worst moment could publish a renamed
         * temp containing only LFH+payload bytes with no central
         * directory. */
        {
            uint64_t total = (uint64_t)pak->pk3_cdr_offset
                             + (uint64_t)PK3_EOCD_SIZE;
            for (e = pak->head; e != NULL; e = e->next) {
                total += (uint64_t)PK3_CDR_SIZE
                         + (uint64_t)strlen(e->filename);
                if (total > UINT32_MAX) {
                    pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                                 old_offset_field,
                                                 old_cdr_offset);
                    fclose(tmpfp);
                    remove(tmp_path);
                    free(old_lfh);
                    free(old_payload);
                    free(old_offset_field);
                    return pk3_err_fill(err, PAKKA_ERR_LIMIT,
                                        PAKKA_ERR_DOMAIN_NONE, 0, "commit",
                                        "ZIP CDR + EOCD would push file "
                                        "past 4 GiB");
                }
            }
        }
        for (e = pak->head; e != NULL; e = e->next) {
            uint16_t name_len = (uint16_t)strlen(e->filename);
            pk3_build_cdr(cdr, e->pk3_crc32, e->pk3_compressed_size,
                          e->length, name_len, e->pk3_lfh_offset);
            if (fwrite(cdr, 1, PK3_CDR_SIZE, tmpfp) != PK3_CDR_SIZE
                || fwrite(e->filename, 1, name_len, tmpfp) != name_len) {
                saved_errno = errno;
                pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                             old_offset_field,
                                             old_cdr_offset);
                fclose(tmpfp);
                remove(tmp_path);
                free(old_lfh);
                free(old_payload);
                free(old_offset_field);
                return pk3_err_fill(err, PAKKA_ERR_IO,
                                    PAKKA_ERR_DOMAIN_ERRNO,
                                    (uint32_t)saved_errno, "commit",
                                    "Cannot write rebuild CDR entry %s",
                                    e->filename);
            }
            cdr_size += PK3_CDR_SIZE + name_len;
            entry_count++;
        }
        pk3_build_eocd(eocd, pak->pk3_cdr_offset, cdr_size,
                       (uint16_t)entry_count);
        if (fwrite(eocd, 1, PK3_EOCD_SIZE, tmpfp) != PK3_EOCD_SIZE) {
            saved_errno = errno;
            pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                         old_offset_field, old_cdr_offset);
            fclose(tmpfp);
            remove(tmp_path);
            free(old_lfh);
            free(old_payload);
            free(old_offset_field);
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot write rebuild EOCD");
        }
        if (fflush(tmpfp) != 0) {
            saved_errno = errno;
            pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                         old_offset_field, old_cdr_offset);
            fclose(tmpfp);
            remove(tmp_path);
            free(old_lfh);
            free(old_payload);
            free(old_offset_field);
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot flush rebuild temp before rename");
        }

        /* Close BOTH file handles before renaming. Windows CRT fopen
         * omits FILE_SHARE_DELETE so MoveFileExA blocks while either
         * handle is live. Check fclose() errors before the rename so
         * buffered-write failures on either file surface here rather
         * than after we've published the temp. */
        /* fclose-with-fault: always close first, then let the fault
         * hook override the success code. Skipping the close on a
         * fault-injected branch would leak the handle. */
        {
            int close_rc = fclose(tmpfp);
            if (PAKKA_FAULT_CHECK("commit_fclose_tmp")) {
                close_rc = -1;
                errno = EIO;
            }
            if (close_rc != 0) {
                saved_errno = errno ? errno : EIO;
                pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                             old_offset_field,
                                             old_cdr_offset);
                remove(tmp_path);
                free(old_lfh);
                free(old_payload);
                free(old_offset_field);
                return pk3_err_fill(err, PAKKA_ERR_IO,
                                    PAKKA_ERR_DOMAIN_ERRNO,
                                    (uint32_t)saved_errno, "commit",
                                    "Cannot close ZIP rebuild temp "
                                    "before rename");
            }
        }
        {
            int close_rc = fclose(pak->fp);
            if (PAKKA_FAULT_CHECK("commit_fclose_orig")) {
                close_rc = -1;
                errno = EIO;
            }
            pak->fp = NULL;
            if (close_rc != 0) {
                saved_errno = errno ? errno : EIO;
                pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                             old_offset_field,
                                             old_cdr_offset);
                remove(tmp_path);
                /* Restore the original archive's handle so the caller
                 * keeps a readable archive after a failed commit —
                 * matches the rename-failure recovery path below. */
                pak->fp = fopen(pak->is_new ? pak->tmp_pakpath
                                            : pak->cur_pakpath, "r+b");
                free(old_lfh);
                free(old_payload);
                free(old_offset_field);
                return pk3_err_fill(err, PAKKA_ERR_IO,
                                    PAKKA_ERR_DOMAIN_ERRNO,
                                    (uint32_t)saved_errno, "commit",
                                    "Cannot close original ZIP "
                                    "before rename");
            }
        }
        {
            uint32_t win32_code = 0;
            int rc;
            /* For is_new: rename rebuild temp over the create temp
             * (pak->tmp_pakpath). pakka_close finishes the
             * tmp_pakpath → new_pakpath rename. For an existing
             * archive: rename rebuild temp over cur_pakpath. */
            if (PAKKA_FAULT_CHECK("commit_rename")) {
                rc = -1;
                errno = EIO;
            } else {
                rc = pak->is_new
                    ? pakka_compat_rename_replace(tmp_path,
                                                  pak->tmp_pakpath,
                                                  &win32_code)
                    : pakka_compat_rename_replace(tmp_path,
                                                  pak->cur_pakpath,
                                                  &win32_code);
            }
            if (rc != 0) {
                saved_errno = errno;
                pk3_rebuild_rollback_offsets(pak, old_lfh, old_payload,
                                             old_offset_field,
                                             old_cdr_offset);
                remove(tmp_path);
                /* Try to reopen the original so the handle is at
                 * least readable for a caller that wants to retry. */
                pak->fp = fopen(pak->is_new ? pak->tmp_pakpath
                                            : pak->cur_pakpath, "r+b");
                free(old_lfh);
                free(old_payload);
                free(old_offset_field);
                return pk3_err_fill(err, PAKKA_ERR_IO,
                                    win32_code ? PAKKA_ERR_DOMAIN_WIN32
                                               : PAKKA_ERR_DOMAIN_ERRNO,
                                    win32_code ? win32_code
                                               : (uint32_t)saved_errno,
                                    "commit",
                                    "Cannot rename ZIP rebuild temp");
            }
        }

        /* Past the rename: temp IS the canonical file. New offsets
         * stay installed even if reopen/seek below fail — restoring
         * would mismatch on-disk reality. Free the rollback buffers. */
        free(old_lfh);
        free(old_payload);
        free(old_offset_field);
        /* Queued adds have been consumed into the renamed file. Drop
         * the pending source/data on every entry so subsequent ops
         * (reads, a second commit on a new add) see them as live. */
        for (e = pak->head; e != NULL; e = e->next) {
            free(e->pk3_pending_source);
            e->pk3_pending_source = NULL;
            free(e->pk3_pending_data);
            e->pk3_pending_data = NULL;
            e->pk3_pending_data_len = 0;
        }
        /* CDR + EOCD are already in the renamed file. Reopen for future
         * reads only; no more writes needed. file_size matches what we
         * wrote: payload_section + cdr_size + EOCD. */
        pak->fp = fopen(pak->is_new ? pak->tmp_pakpath : pak->cur_pakpath,
                        "r+b");
        if (pak->fp == NULL) {
            saved_errno = errno;
            /* Archive is on disk but we have no handle. Leave dirty
             * cleared so pakka_close doesn't attempt another commit;
             * future reads will hit the no-fp error path. */
            pak->dirty = 0;
            pak->needs_rebuild = 0;
            return pk3_err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot reopen committed ZIP");
        }
        pak->file_size = (uint64_t)pak->pk3_cdr_offset
                       + (uint64_t)cdr_size
                       + (uint64_t)PK3_EOCD_SIZE;
        pak->dirty = 0;
        pak->needs_rebuild = 0;
        return PAKKA_OK;
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

