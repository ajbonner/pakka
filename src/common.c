#include "common.h"
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* CRC32 (IEEE 802.3 polynomial, ZIP's choice).                       */
/* ------------------------------------------------------------------ */

/* Precomputed IEEE 802.3 CRC32 table (polynomial 0xEDB88320, reflected).
 * Inlined as a const so first use is allocation-free and concurrent
 * callers can't race on a lazy build. Generated once via:
 *   for i in 0..255: c = i; 8x: c = c&1 ? 0xEDB88320 ^ (c>>1) : c>>1
 * Shared by the ZIP path (LFH/CDR CRC fields) and the PAK atomic-staging
 * path (transient add-vs-commit content check); see pakka_crc32_update. */
static const uint32_t pakka_crc32_table[PAKKA_CRC32_TABLE_SIZE] = {
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

/* IEEE 802.3 CRC32 over buf, chained from a prior result (pass 0 to
 * start). Internal helper shared by src/pakfile.c and src/pk3file.c. */
uint32_t pakka_crc32_update(uint32_t crc,
                            const unsigned char *buf, size_t len) {
    size_t i;
    crc ^= 0xFFFFFFFFu;
    for (i = 0; i < len; i++) {
        crc = pakka_crc32_table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Free a single entry, including any pending-add bookkeeping (PAK
 * atomic staging or ZIP queued adds). Used by both src/pakfile.c
 * (destroy_pak, pakka_delete) and src/pk3file.c (add-path error paths)
 * so the cleanup of pending source/data lives in one place. */
void pakka_entry_free(Pakfileentry_t *e) {
    if (e == NULL) return;
    free(e->pending_source);
    free(e->pending_data);
    free(e);
}

/* VERSION is supplied as a -D flag by the build system (Makefile +
 * CMakeLists.txt). Reading it through a function rather than a macro
 * means downstream consumers of libpakka.a get the version compiled
 * into the library, not whatever their own translation unit sees. */
const char *pakka_version(void) {
    return VERSION;
}

/* The on-disk pak format is canonically little-endian (Quake 1/2
 * originated on x86). A raw fread of a uint32_t works only on LE
 * hosts; on big-endian (s390x, sparc, ppc) it interprets the bytes in
 * the wrong order and corrupts diroffset/dirlength/offset/length.
 * Anything that touches an on-disk u32 must go through these. */
int pakka_read_u32_le(FILE *fp, uint32_t *out) {
    unsigned char b[4];
    if (fread(b, 1, 4, fp) != 4) {
        return -1;
    }
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return 0;
}

int pakka_write_u32_le(FILE *fp, uint32_t value) {
    unsigned char b[4];
    b[0] = (unsigned char)(value & 0xFF);
    b[1] = (unsigned char)((value >> 8) & 0xFF);
    b[2] = (unsigned char)((value >> 16) & 0xFF);
    b[3] = (unsigned char)((value >> 24) & 0xFF);
    if (fwrite(b, 1, 4, fp) != 4) {
        return -1;
    }
    return 0;
}

/* PAK-class geometry rows. Quake (PAK) and Daikatana share the 56-byte
 * name field but Daikatana appends two u32s per entry (compressed_size +
 * is_compressed). SiN widens the name field to 120 bytes. WAD (Doom
 * 1/2 IWAD/PWAD) shares the 12-byte header shape but reorders the
 * directory entry (filepos + size + name[8]) and reorders the header
 * (numlumps-in-entries + infotableofs) — those divergences are handled
 * by pakka_format_is_wad branches at the five read/write sites, not by
 * extra fields here. The table captures only per-entry name + row
 * dimensions and per-format signature. */
static const pakka_pak_geometry_t pakka_pak_geometry_rows[] = {
    /* PAK        */ { "PACK", 56,  64, 0 },
    /* SIN        */ { "SPAK", 120, 128, 0 },
    /* DAIKATANA  */ { "PACK", 56,  72, 1 },
    /* IWAD       */ { "IWAD", 8,   16, 0 },
    /* PWAD       */ { "PWAD", 8,   16, 0 }
};

const pakka_pak_geometry_t *pakka_pak_geometry(pakka_format_t fmt) {
    switch (fmt) {
        case PAKKA_FORMAT_PAK:       return &pakka_pak_geometry_rows[0];
        case PAKKA_FORMAT_SIN:       return &pakka_pak_geometry_rows[1];
        case PAKKA_FORMAT_DAIKATANA: return &pakka_pak_geometry_rows[2];
        case PAKKA_FORMAT_IWAD:      return &pakka_pak_geometry_rows[3];
        case PAKKA_FORMAT_PWAD:      return &pakka_pak_geometry_rows[4];
        default:                     return NULL;
    }
}

/* Validate n bytes as UTF-8 per RFC 3629: rejects overlong encodings
 * (e.g. 0xC0 0x80 for NUL), surrogate halves (U+D800..U+DFFF), and
 * codepoints above U+10FFFF. Does not require a trailing NUL — caller
 * passes the explicit length. Used by:
 *   - pk3file.c read path: pick UTF-8 vs CP437 for legacy ZIP names
 *   - cli.c extract/list paths: detect legacy PAK/SiN/WAD names that
 *     cannot be handed to CreateFileW on Windows. */
int pakka_is_valid_utf8(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) {
            if (c < 0xC2) return 0;
            if (i + 1 >= n) return 0;
            if ((s[i+1] & 0xC0) != 0x80) return 0;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            uint32_t cp;
            if (i + 2 >= n) return 0;
            if ((s[i+1] & 0xC0) != 0x80) return 0;
            if ((s[i+2] & 0xC0) != 0x80) return 0;
            cp = ((uint32_t)(c & 0x0F) << 12)
               | ((uint32_t)(s[i+1] & 0x3F) << 6)
               |  (uint32_t)(s[i+2] & 0x3F);
            if (cp < 0x800) return 0;
            if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            uint32_t cp;
            if (c > 0xF4) return 0;
            if (i + 3 >= n) return 0;
            if ((s[i+1] & 0xC0) != 0x80) return 0;
            if ((s[i+2] & 0xC0) != 0x80) return 0;
            if ((s[i+3] & 0xC0) != 0x80) return 0;
            cp = ((uint32_t)(c & 0x07) << 18)
               | ((uint32_t)(s[i+1] & 0x3F) << 12)
               | ((uint32_t)(s[i+2] & 0x3F) << 6)
               |  (uint32_t)(s[i+3] & 0x3F);
            if (cp < 0x10000) return 0;
            if (cp > 0x10FFFF) return 0;
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

/* Length of a valid UTF-8 sequence starting at s[0], or 0 if s[0]
 * doesn't begin a valid sequence within the n remaining bytes. */
static size_t pakka_utf8_seq_len(const unsigned char *s, size_t n) {
    unsigned char c;
    if (n == 0) return 0;
    c = s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) {
        if (c < 0xC2) return 0;
        if (n < 2 || (s[1] & 0xC0) != 0x80) return 0;
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        uint32_t cp;
        if (n < 3) return 0;
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        cp = ((uint32_t)(c & 0x0F) << 12)
           | ((uint32_t)(s[1] & 0x3F) << 6)
           |  (uint32_t)(s[2] & 0x3F);
        if (cp < 0x800) return 0;
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        uint32_t cp;
        if (c > 0xF4) return 0;
        if (n < 4) return 0;
        if ((s[1] & 0xC0) != 0x80) return 0;
        if ((s[2] & 0xC0) != 0x80) return 0;
        if ((s[3] & 0xC0) != 0x80) return 0;
        cp = ((uint32_t)(c & 0x07) << 18)
           | ((uint32_t)(s[1] & 0x3F) << 12)
           | ((uint32_t)(s[2] & 0x3F) << 6)
           |  (uint32_t)(s[3] & 0x3F);
        if (cp < 0x10000) return 0;
        if (cp > 0x10FFFF) return 0;
        return 4;
    }
    return 0;
}

int pakka_utf8_substitute_invalid(const char *src, char *dst, size_t cap,
                                  char fill) {
    const unsigned char *s = (const unsigned char *)src;
    size_t i = 0, n = strlen(src);
    size_t out = 0;
    int substituted = 0;
    if (cap == 0) return 0;
    while (i < n) {
        size_t seq = pakka_utf8_seq_len(s + i, n - i);
        if (seq == 0) {
            /* Invalid byte at i. Substitute and advance by 1. */
            if (out + 1 >= cap) break;
            dst[out++] = fill;
            substituted = 1;
            i++;
        } else {
            if (out + seq >= cap) break;
            memcpy(dst + out, s + i, seq);
            out += seq;
            i += seq;
        }
    }
    dst[out < cap ? out : cap - 1] = '\0';
    return substituted;
}
