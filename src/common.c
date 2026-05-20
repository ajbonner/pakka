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
 * is_compressed). SiN widens the name field to 120 bytes. */
static const pakka_pak_geometry_t pakka_pak_geometry_rows[] = {
    /* PAK        */ { "PACK", 56,  64, 0 },
    /* SIN        */ { "SPAK", 120, 128, 0 },
    /* DAIKATANA  */ { "PACK", 56,  72, 1 }
};

const pakka_pak_geometry_t *pakka_pak_geometry(pakka_format_t fmt) {
    switch (fmt) {
        case PAKKA_FORMAT_PAK:       return &pakka_pak_geometry_rows[0];
        case PAKKA_FORMAT_SIN:       return &pakka_pak_geometry_rows[1];
        case PAKKA_FORMAT_DAIKATANA: return &pakka_pak_geometry_rows[2];
        default:                     return NULL;
    }
}
