#include "common.h"

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
