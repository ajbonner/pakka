#include "common.h"
#include <stdlib.h>

/* Free a single entry, including any ZIP queued-add bookkeeping. Used
 * by both src/pakfile.c (destroy_pak, pakka_delete) and src/pk3file.c
 * (add-path error paths) so the cleanup of pending source/data lives
 * in one place. */
void pakka_entry_free(Pakfileentry_t *e) {
    if (e == NULL) return;
    free(e->pk3_pending_source);
    free(e->pk3_pending_data);
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
