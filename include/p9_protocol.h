#ifndef P9_PROTOCOL_H
#define P9_PROTOCOL_H

#include <exec/types.h>

/*
 * 9P2000.L Protocol Constants and Types
 *
 * All wire fields are little-endian. The marshal/unmarshal helpers in
 * p9_marshal.c use byte-by-byte extraction which inherently converts
 * between host byte order and LE on big-endian PPC — no bswap needed.
 * Do NOT add __builtin_bswap calls; that would double-swap.
 */

#define P9_VERSION_STR  "9P2000.L"
#define P9_MSIZE        524288  /* Max message size (512KB) — fewer round-trips for large I/O */
#define P9_NOFID        0xFFFFFFFFUL
#define P9_NOTAG        0xFFFF
#define P9_MAXWELEM     16      /* Max walk elements per Twalk */

/* T-message type IDs. R-type = T-type + 1 */
#define P9_RLERROR      7
#define P9_TSTATFS      8
#define P9_RSTATFS      9
#define P9_TLOPEN       12
#define P9_RLOPEN       13
#define P9_TLCREATE     14
#define P9_RLCREATE     15
#define P9_TSYMLINK     16
#define P9_RSYMLINK     17
#define P9_TREADLINK    22
#define P9_RREADLINK    23
#define P9_TGETATTR     24
#define P9_RGETATTR     25
#define P9_TSETATTR     26
#define P9_RSETATTR     27
#define P9_TREADDIR     40
#define P9_RREADDIR     41
#define P9_TFSYNC       50
#define P9_RFSYNC       51
#define P9_TLINK        70
#define P9_RLINK        71
#define P9_TMKDIR       72
#define P9_RMKDIR       73
#define P9_TRENAMEAT    74
#define P9_RRENAMEAT    75
#define P9_TUNLINKAT    76
#define P9_RUNLINKAT    77
#define P9_TVERSION     100
#define P9_RVERSION     101
#define P9_TATTACH      104
#define P9_RATTACH      105
#define P9_TFLUSH       108
#define P9_RFLUSH       109
#define P9_TWALK        110
#define P9_RWALK        111
#define P9_TREAD        116
#define P9_RREAD        117
#define P9_TWRITE       118
#define P9_RWRITE       119
#define P9_TCLUNK       120
#define P9_RCLUNK       121

/* 9P2000.L getattr mask bits */
#define P9_GETATTR_MODE      0x00000001ULL
#define P9_GETATTR_NLINK     0x00000002ULL
#define P9_GETATTR_UID       0x00000004ULL
#define P9_GETATTR_GID       0x00000008ULL
#define P9_GETATTR_RDEV      0x00000010ULL
#define P9_GETATTR_ATIME     0x00000020ULL
#define P9_GETATTR_MTIME     0x00000040ULL
#define P9_GETATTR_CTIME     0x00000080ULL
#define P9_GETATTR_INO       0x00000100ULL
#define P9_GETATTR_SIZE      0x00000200ULL
#define P9_GETATTR_BLOCKS    0x00000400ULL
#define P9_GETATTR_BTIME     0x00000800ULL
#define P9_GETATTR_GEN       0x00001000ULL
#define P9_GETATTR_DATA_VERSION 0x00002000ULL
#define P9_GETATTR_BASIC     0x000007FFULL  /* All basic fields */
#define P9_GETATTR_ALL       0x00003FFFULL

/* 9P2000.L setattr valid bits */
#define P9_SETATTR_MODE      0x00000001UL
#define P9_SETATTR_UID       0x00000002UL
#define P9_SETATTR_GID       0x00000004UL
#define P9_SETATTR_SIZE      0x00000008UL
#define P9_SETATTR_ATIME     0x00000010UL
#define P9_SETATTR_MTIME     0x00000020UL
#define P9_SETATTR_CTIME     0x00000040UL
#define P9_SETATTR_ATIME_SET 0x00000080UL
#define P9_SETATTR_MTIME_SET 0x00000100UL

/* AT_REMOVEDIR for Tunlinkat */
#define P9_AT_REMOVEDIR      0x200

/* QID type bits */
#define P9_QTDIR             0x80
#define P9_QTAPPEND          0x40
#define P9_QTEXCL            0x20
#define P9_QTAUTH            0x08
#define P9_QTFILE            0x00

/* QID structure (13 bytes on wire) */
struct P9Qid {
    uint8  type;
    uint32 version;
    uint64 path;
};

/* Parsed Rgetattr response */
struct P9Stat {
    uint64 valid;
    struct P9Qid qid;
    uint32 mode;
    uint32 uid;
    uint32 gid;
    uint64 nlink;
    uint64 rdev;
    uint64 size;
    uint64 blksize;
    uint64 blocks;
    uint64 atime_sec;
    uint64 atime_nsec;
    uint64 mtime_sec;
    uint64 mtime_nsec;
    uint64 ctime_sec;
    uint64 ctime_nsec;
    uint64 btime_sec;
    uint64 btime_nsec;
    uint64 gen;
    uint64 data_version;
};

/* Tsetattr input */
struct P9Iattr {
    uint32 valid;
    uint32 mode;
    uint32 uid;
    uint32 gid;
    uint64 size;
    uint64 atime_sec;
    uint64 atime_nsec;
    uint64 mtime_sec;
    uint64 mtime_nsec;
};

/* Rstatfs response */
struct P9Statfs {
    uint32 type;
    uint32 bsize;
    uint64 blocks;
    uint64 bfree;
    uint64 bavail;
    uint64 files;
    uint64 ffree;
    uint64 fsid;
    uint32 namelen;
};

/*
 * Marshal helpers — write LE values into buffer at offset, advance offset.
 */
void p9_put_u8(uint8 *buf, uint32 *off, uint8 val);
void p9_put_u16(uint8 *buf, uint32 *off, uint16 val);
void p9_put_u32(uint8 *buf, uint32 *off, uint32 val);
void p9_put_u64(uint8 *buf, uint32 *off, uint64 val);
void p9_put_str(uint8 *buf, uint32 *off, const char *s);
void p9_put_header(uint8 *buf, uint32 *off, uint8 type, uint16 tag);
void p9_finalize(uint8 *buf, uint32 total_size);

/*
 * Unmarshal helpers — read LE values from buffer at offset, advance offset.
 */
uint8  p9_get_u8(const uint8 *buf, uint32 *off);
uint16 p9_get_u16(const uint8 *buf, uint32 *off);
uint32 p9_get_u32(const uint8 *buf, uint32 *off);
uint64 p9_get_u64(const uint8 *buf, uint32 *off);
void   p9_get_str(const uint8 *buf, uint32 *off, char *out, uint32 max);
void   p9_get_qid(const uint8 *buf, uint32 *off, struct P9Qid *qid);

#endif /* P9_PROTOCOL_H */
