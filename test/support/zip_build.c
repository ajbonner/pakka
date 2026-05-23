#include "zip_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u16_le(unsigned char *buf, size_t off, uint16_t v)
{
    buf[off + 0] = (unsigned char)(v & 0xFF);
    buf[off + 1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_u32_le(unsigned char *buf, size_t off, uint32_t v)
{
    buf[off + 0] = (unsigned char)(v & 0xFF);
    buf[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    buf[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    buf[off + 3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint16_t get_u16_le(const unsigned char *buf, size_t off)
{
    return (uint16_t)((uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8));
}

static uint32_t crc32_table[256];
static int      crc32_table_built;

static void build_crc32_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_built = 1;
}

uint32_t zip_crc32(const void *data, size_t len)
{
    if (!crc32_table_built) build_crc32_table();
    const unsigned char *p = (const unsigned char *)data;
    uint32_t             c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c = crc32_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

int zip_write_single(const char *path, const zip_single_t *p)
{
    uint32_t csize = p->use_csize_override ? p->csize_override : (uint32_t)p->payload_len;
    uint32_t usize = p->use_usize_override ? p->usize_override : (uint32_t)p->payload_len;
    uint32_t crc   = p->use_crc_override ? p->crc_override
                                         : zip_crc32(p->payload, p->payload_len);

    unsigned char lfh[30];
    memcpy(lfh, "PK\x03\x04", 4);
    put_u16_le(lfh, 4, 20);            /* version needed */
    put_u16_le(lfh, 6, p->gp_flags);
    put_u16_le(lfh, 8, p->method);
    put_u16_le(lfh, 10, 0);            /* mod time */
    put_u16_le(lfh, 12, 0x0021);       /* mod date — stable date so
                                          fixture bytes are reproducible
                                          across builds */
    put_u32_le(lfh, 14, crc);
    put_u32_le(lfh, 18, csize);
    put_u32_le(lfh, 22, usize);
    put_u16_le(lfh, 26, (uint16_t)p->name_len);
    put_u16_le(lfh, 28, 0);

    unsigned char cdr[46];
    memcpy(cdr, "PK\x01\x02", 4);
    put_u16_le(cdr, 4, 20);            /* version made by */
    put_u16_le(cdr, 6, 20);            /* version needed */
    put_u16_le(cdr, 8, p->gp_flags);
    put_u16_le(cdr, 10, p->method);
    put_u16_le(cdr, 12, 0);
    put_u16_le(cdr, 14, 0x0021);
    put_u32_le(cdr, 16, crc);
    put_u32_le(cdr, 20, csize);
    put_u32_le(cdr, 24, usize);
    put_u16_le(cdr, 28, (uint16_t)p->name_len);
    put_u16_le(cdr, 30, 0);
    put_u16_le(cdr, 32, 0);
    put_u16_le(cdr, 34, 0);
    put_u16_le(cdr, 36, 0);
    put_u32_le(cdr, 38, 0);
    put_u32_le(cdr, 42, 0);            /* LFH offset = 0 */

    uint32_t      lfh_total = (uint32_t)(sizeof(lfh) + p->name_len + p->payload_len);
    uint32_t      cdr_size  = (uint32_t)(sizeof(cdr) + p->name_len);
    unsigned char eocd[22];
    memcpy(eocd, "PK\x05\x06", 4);
    put_u16_le(eocd, 4, 0);
    put_u16_le(eocd, 6, 0);
    put_u16_le(eocd, 8, 1);
    put_u16_le(eocd, 10, 1);
    put_u32_le(eocd, 12, cdr_size);
    put_u32_le(eocd, 16, lfh_total);
    put_u16_le(eocd, 20, 0);

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(lfh, 1, sizeof(lfh), f) != sizeof(lfh)) goto fail;
    if (p->name_len > 0 && fwrite(p->name, 1, p->name_len, f) != p->name_len) goto fail;
    if (p->payload_len > 0 && fwrite(p->payload, 1, p->payload_len, f) != p->payload_len) goto fail;
    if (fwrite(cdr, 1, sizeof(cdr), f) != sizeof(cdr)) goto fail;
    if (p->name_len > 0 && fwrite(p->name, 1, p->name_len, f) != p->name_len) goto fail;
    if (fwrite(eocd, 1, sizeof(eocd), f) != sizeof(eocd)) goto fail;
    return fclose(f) == 0 ? 0 : -1;
fail:
    fclose(f);
    return -1;
}

int zip_first_cdr_method(const unsigned char *buf, size_t len)
{
    for (size_t i = 0; i + 12 <= len; i++) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4B &&
            buf[i + 2] == 0x01 && buf[i + 3] == 0x02) {
            return (int)get_u16_le(buf, i + 10);
        }
    }
    return -1;
}

int zip_nth_cdr_method(const unsigned char *buf, size_t len, int n)
{
    int seen = 0;
    for (size_t i = 0; i + 12 <= len; i++) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4B &&
            buf[i + 2] == 0x01 && buf[i + 3] == 0x02) {
            if (seen == n) {
                return (int)get_u16_le(buf, i + 10);
            }
            seen++;
        }
    }
    return -1;
}
