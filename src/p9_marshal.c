#include "p9_protocol.h"
#include "string_utils.h"

/*
 * 9P wire format marshal/unmarshal helpers.
 *
 * All 9P fields are little-endian on the wire. Byte-by-byte extraction
 * inherently converts between host byte order and LE — no bswap needed.
 */

void p9_put_u8(uint8 *buf, uint32 *off, uint8 val)
{
    buf[*off] = val;
    *off += 1;
}

void p9_put_u16(uint8 *buf, uint32 *off, uint16 val)
{
    buf[*off]     = (uint8)(val);
    buf[*off + 1] = (uint8)(val >> 8);
    *off += 2;
}

void p9_put_u32(uint8 *buf, uint32 *off, uint32 val)
{
    buf[*off]     = (uint8)(val);
    buf[*off + 1] = (uint8)(val >> 8);
    buf[*off + 2] = (uint8)(val >> 16);
    buf[*off + 3] = (uint8)(val >> 24);
    *off += 4;
}

void p9_put_u64(uint8 *buf, uint32 *off, uint64 val)
{
    buf[*off]     = (uint8)(val);
    buf[*off + 1] = (uint8)(val >> 8);
    buf[*off + 2] = (uint8)(val >> 16);
    buf[*off + 3] = (uint8)(val >> 24);
    buf[*off + 4] = (uint8)(val >> 32);
    buf[*off + 5] = (uint8)(val >> 40);
    buf[*off + 6] = (uint8)(val >> 48);
    buf[*off + 7] = (uint8)(val >> 56);
    *off += 8;
}

void p9_put_str(uint8 *buf, uint32 *off, const char *s)
{
    uint16 len = (uint16)strlen(s);
    p9_put_u16(buf, off, len);
    memcpy(buf + *off, s, len);
    *off += len;
}

void p9_put_header(uint8 *buf, uint32 *off, uint8 type, uint16 tag)
{
    *off = 4;  /* Skip size[4] — will be patched by p9_finalize */
    p9_put_u8(buf, off, type);
    p9_put_u16(buf, off, tag);
}

void p9_finalize(uint8 *buf, uint32 total_size)
{
    uint32 off = 0;
    p9_put_u32(buf, &off, total_size);
}

uint8 p9_get_u8(const uint8 *buf, uint32 *off)
{
    uint8 val = buf[*off];
    *off += 1;
    return val;
}

uint16 p9_get_u16(const uint8 *buf, uint32 *off)
{
    uint16 val = (uint16)buf[*off] | ((uint16)buf[*off + 1] << 8);
    *off += 2;
    return val;
}

uint32 p9_get_u32(const uint8 *buf, uint32 *off)
{
    uint32 val = (uint32)buf[*off] |
                 ((uint32)buf[*off + 1] << 8) |
                 ((uint32)buf[*off + 2] << 16) |
                 ((uint32)buf[*off + 3] << 24);
    *off += 4;
    return val;
}

uint64 p9_get_u64(const uint8 *buf, uint32 *off)
{
    uint64 val = (uint64)buf[*off] |
                 ((uint64)buf[*off + 1] << 8) |
                 ((uint64)buf[*off + 2] << 16) |
                 ((uint64)buf[*off + 3] << 24) |
                 ((uint64)buf[*off + 4] << 32) |
                 ((uint64)buf[*off + 5] << 40) |
                 ((uint64)buf[*off + 6] << 48) |
                 ((uint64)buf[*off + 7] << 56);
    *off += 8;
    return val;
}

void p9_get_str(const uint8 *buf, uint32 *off, char *out, uint32 max)
{
    uint16 len = p9_get_u16(buf, off);
    uint32 copy = (len < max - 1) ? len : max - 1;
    memcpy(out, buf + *off, copy);
    out[copy] = '\0';
    *off += len;
}

void p9_get_qid(const uint8 *buf, uint32 *off, struct P9Qid *qid)
{
    qid->type    = p9_get_u8(buf, off);
    qid->version = p9_get_u32(buf, off);
    qid->path    = p9_get_u64(buf, off);
}
