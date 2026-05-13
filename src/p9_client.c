#include "p9_client.h"
#include "virtio9p_handler.h"
#include "virtio/virtqueue.h"
#include "virtio/virtio_pci.h"
#include <proto/exec.h>
#include <exec/memory.h>
#include "string_utils.h"

/*
 * PPC data cache management for DMA buffers.
 *
 * dcbst (Data Cache Block Store): flush dirty cache line to RAM.
 *   Device can then read the latest CPU-written data.
 *   User-mode safe.
 *
 * dcbf (Data Cache Block Flush): flush dirty line to RAM AND invalidate.
 *   Next CPU read fetches fresh data from RAM (written by device).
 *   User-mode safe.  (dcbi is supervisor-only — privilege violation!)
 *
 * These replace per-transaction StartDMA/EndDMA calls, saving ~10
 * kernel round-trips.  Physical addresses are cached at startup.
 *
 * PPC cache line = 32 bytes on G3/G4.
 */
/* Forward declarations */
static int32 p9_check_error(const uint8 *rx_buf, uint32 rx_len);
static uint16 p9_next_tag(struct V9PHandler *h);

/* Minimum bytes required for a well-formed R-message header:
 * size[4] + type[1] + tag[2]. */
#define P9_RHDR_SIZE  7

/* Minimum Rlerror size: header + errno[4]. */
#define P9_RLERROR_SIZE  (P9_RHDR_SIZE + 4)

/* Guard against a truncated response.  If the server (or transport) hands
 * us fewer bytes than the caller is about to parse, return -EIO rather
 * than reading tail garbage out of the rx_buf. */
static inline int32 p9_require_size(uint32 rx_len, uint32 need)
{
    return rx_len < need ? -5 : 0;
}

#define CACHE_LINE 32

static inline void cache_flush(void *addr, uint32 len)
{
    uint8 *p = (uint8 *)((uint32)addr & ~(CACHE_LINE - 1));
    uint8 *end = (uint8 *)addr + len;
    while (p < end) {
        __asm__ volatile("dcbst 0,%0" : : "r"(p));
        p += CACHE_LINE;
    }
    __asm__ volatile("sync" ::: "memory");
}

static inline void cache_invalidate(void *addr, uint32 len)
{
    uint8 *p = (uint8 *)((uint32)addr & ~(CACHE_LINE - 1));
    uint8 *end = (uint8 *)addr + len;
    while (p < end) {
        __asm__ volatile("dcbf 0,%0" : : "r"(p) : "memory");
        p += CACHE_LINE;
    }
    __asm__ volatile("sync" ::: "memory");
}

/*
 * V9P_Transact — Send T-message in tx_buf, wait for R-message in rx_buf.
 *
 * Mode-agnostic: calls AddBuf/Kick/GetBuf which handle endianness internally.
 * Returns the number of bytes in the R-message, or 0 on error.
 *
 * Uses cached DMA physical addresses (resolved once at startup) with
 * PPC dcbst/dcbf for per-transaction cache coherency:
 *   - dcbst tx_buf: flush CPU writes to RAM so device sees our T-message
 *   - dcbf  rx_buf: flush + invalidate so CPU reads the device's
 *                    R-message from RAM on the next load
 *
 * The semaphore is removed since FBX runs a single-threaded event loop.
 */
uint32 V9P_Transact(struct V9PHandler *h, uint32 tx_size)
{
    struct virtqueue *vq = h->vq;
    struct PCIDevice *pciDev = h->pciDevice;

    /* P0-1: capture expected tag from the T-message we are about to
     * send.  Every R-message must carry the same tag.  Any response
     * whose tag does not match is a stale reply from a prior aborted
     * (Tflush'd) request; drop it and keep polling.  This is what
     * eliminates the "Error 1" desync seen by DOpus4. */
    uint16 expected_tag = (uint16)h->tx_buf[5] | ((uint16)h->tx_buf[6] << 8);

    /* Flush tx_buf from CPU cache to RAM so device sees it */
    cache_flush(h->tx_buf, tx_size);

    /* Invalidate rx_buf so CPU won't read stale cached data after device writes */
    cache_invalidate(h->rx_buf, h->msize);

    /* Build scatter-gather using cached physical addresses */
    struct vring_sg sg[2];
    sg[0].addr = h->tx_phys;
    sg[0].len  = tx_size;
    sg[1].addr = h->rx_phys;
    sg[1].len  = h->msize;

    void *cookie = (void *)h->tx_buf;

    int32 rc = VirtQueue_AddBuf(IExec, vq, sg, 1, 1, cookie);
    if (rc < 0)
        return 0;
    VirtQueue_Kick(IExec, vq, pciDev, h->iobase);

    /* Poll for completion.
     *
     * We cannot use IExec->Wait() here because FBX's FbxEventLoop also
     * calls Wait() with a broad signal mask that includes our IRQ bit.
     * If the ISR fires while FBX is in Wait(), FBX consumes it and our
     * Wait() blocks forever.
     *
     * Instead, poll the used ring directly.  QEMU processes VirtIO 9P
     * requests in microseconds, so the first poll usually succeeds.
     * A generous iteration count handles any latency.
     */
    uint32 written = 0;
    void *ret_cookie = NULL;
    uint32 poll_count = 0;
    /* Iteration-count timeout — wall-clock varies with CPU speed AND
     * how chatty the debug build is (each op's serial DPRINTFs eat
     * milliseconds).  100 M iterations = ~1 s on a 600 MHz G3, much
     * shorter on faster cores.  P2-7 will replace this with a proper
     * ReadEClock-based wall-clock timeout. */
    const uint32 MAX_POLLS = 100000000;

    while (poll_count < MAX_POLLS) {
        ret_cookie = VirtQueue_GetBuf(IExec, vq, &written);
        if (ret_cookie) {
            /* P0-1 — validate the response actually belongs to us before
             * exiting the poll loop.  Invalidate the header cache lines
             * so tag/size are read fresh from RAM. */
            cache_invalidate(h->rx_buf, 32);
            if (written >= 7) {
                uint16 got_tag = (uint16)h->rx_buf[5]
                               | ((uint16)h->rx_buf[6] << 8);
                uint32 got_size = ((uint32)h->rx_buf[0])
                                | ((uint32)h->rx_buf[1] << 8)
                                | ((uint32)h->rx_buf[2] << 16)
                                | ((uint32)h->rx_buf[3] << 24);
                if (got_tag == expected_tag && got_size == written)
                    break;  /* legitimate response — exit poll loop */
                DPRINTF("V9P_Transact: drop stale resp tag=%u "
                        "(expect %u) size=%lu rx_len=%lu\n",
                        (uint32)got_tag, (uint32)expected_tag,
                        got_size, written);
            } else {
                DPRINTF("V9P_Transact: drop short resp rx_len=%lu\n",
                        written);
            }
            /* stale or truncated — discard and keep polling */
            ret_cookie = NULL;
            written = 0;
        }
        __asm__ volatile("lwsync" ::: "memory");
        poll_count++;
    }

    /* Invalidate the full rx_buf so subsequent unmarshal reads are fresh
     * (we only invalidated the first cache line above for the tag check). */
    cache_invalidate(h->rx_buf, h->msize);

    if (!ret_cookie) {
        DPRINTF("V9P_Transact: timeout — attempting Tflush for tag\n");

        /* P0-2: build the Tflush in a *dedicated* buffer (h->flush_buf)
         * so we never overwrite the original T-message in tx_buf — the
         * device may still be reading it. */
        uint32 flush_off = 0;
        uint16 stalled_tag = expected_tag;  /* exactly the tag we sent */

        p9_put_header(h->flush_buf, &flush_off, P9_TFLUSH, p9_next_tag(h));
        p9_put_u16(h->flush_buf, &flush_off, stalled_tag);
        p9_finalize(h->flush_buf, flush_off);

        cache_flush(h->flush_buf, flush_off);
        cache_invalidate(h->rx_buf, h->msize);

        struct vring_sg fsg[2];
        fsg[0].addr = h->flush_phys;
        fsg[0].len  = flush_off;
        fsg[1].addr = h->rx_phys;
        fsg[1].len  = h->msize;

        cookie = (void *)h->flush_buf;
        rc = VirtQueue_AddBuf(IExec, vq, fsg, 1, 1, cookie);
        if (rc >= 0) {
            VirtQueue_Kick(IExec, vq, pciDev, h->iobase);

            /* Drain up to 2 responses (stalled R-message + Rflush). */
            uint32 drain;
            for (drain = 0; drain < 2; drain++) {
                poll_count = 0;
                while (poll_count < MAX_POLLS) {
                    ret_cookie = VirtQueue_GetBuf(IExec, vq, &written);
                    if (ret_cookie)
                        break;
                    __asm__ volatile("lwsync" ::: "memory");
                    poll_count++;
                }
                if (!ret_cookie)
                    break;
            }
            cache_invalidate(h->rx_buf, h->msize);
        }

        DPRINTF("V9P_Transact: flush complete, returning timeout\n");
        return 0;
    }

    return written;
}

/*
 * Check if the response is an Rlerror and extract the errno.
 * Returns 0 if not an error, or a negative errno.
 * Also returns -EIO if the response is shorter than a valid R-header
 * (or a valid Rlerror when the type field says so).
 */
static int32 p9_check_error(const uint8 *rx_buf, uint32 rx_len)
{
    if (rx_len < P9_RHDR_SIZE)
        return -5;  /* EIO: truncated R-message */

    uint32 off = 4;  /* Skip size[4] */
    uint8 type = p9_get_u8(rx_buf, &off);
    if (type == P9_RLERROR) {
        if (rx_len < P9_RLERROR_SIZE)
            return -5;  /* EIO: truncated Rlerror */
        off = 7;
        uint32 ecode = p9_get_u32(rx_buf, &off);
        return -(int32)ecode;
    }
    return 0;
}

static uint16 p9_next_tag(struct V9PHandler *h)
{
    uint16 tag = h->next_tag;
    h->next_tag++;
    if (h->next_tag == P9_NOTAG)
        h->next_tag = 0;
    return tag;
}

int32 P9_Version(struct V9PHandler *h)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TVERSION, P9_NOTAG);
    p9_put_u32(buf, &off, P9_MSIZE);
    p9_put_str(buf, &off, P9_VERSION_STR);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5; /* EIO */

    int32 err = p9_check_error(h->rx_buf, rx_len);
    if (err)
        return err;

    /* Rversion: size[4] type[1] tag[2] msize[4] version[s] — minimum
     * 7 + 4 + 2 = 13 bytes (empty version string). */
    err = p9_require_size(rx_len, 13);
    if (err)
        return err;

    uint32 roff = 7;
    h->msize = p9_get_u32(h->rx_buf, &roff);

    char ver[32];
    p9_get_str(h->rx_buf, &roff, ver, sizeof(ver));

    DPRINTF("P9_Version: msize=%lu version=%s\n", h->msize, ver);

    return 0;
}

int32 P9_Attach(struct V9PHandler *h, uint32 root_fid)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TATTACH, p9_next_tag(h));
    p9_put_u32(buf, &off, root_fid);  /* fid */
    p9_put_u32(buf, &off, P9_NOFID);  /* afid (no auth) */
    p9_put_str(buf, &off, "");         /* uname */
    p9_put_str(buf, &off, "");         /* aname */
    p9_put_u32(buf, &off, 0);          /* n_uname (root) */
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    int32 err = p9_check_error(h->rx_buf, rx_len);
    if (err)
        return err;

    DPRINTF("P9_Attach: root fid=%lu attached\n", root_fid);
    return 0;
}

int32 P9_Walk(struct V9PHandler *h, uint32 fid, uint32 newfid, const char *path)
{
    uint8 *buf = h->tx_buf;

    /* Split path into components */
    char pathbuf[1024];
    strncpy(pathbuf, path, sizeof(pathbuf) - 1);
    pathbuf[sizeof(pathbuf) - 1] = '\0';

    char *components[P9_MAXWELEM];
    uint16 nwname = 0;

    /* Tokenise on '/', treating any run of slashes as a single separator.
     * This handles leading, trailing, and embedded "//" correctly — "/foo",
     * "foo/", and "foo//bar" all yield the same components as "foo" and
     * "foo/bar" respectively. */
    char *p = pathbuf;
    while (*p) {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        if (nwname >= P9_MAXWELEM) {
            DPRINTF("P9_Walk: path too deep (>%u components)\n",
                    (uint32)P9_MAXWELEM);
            return -36; /* ENAMETOOLONG */
        }
        components[nwname++] = p;
        while (*p && *p != '/')
            p++;
        if (*p == '/') {
            *p = '\0';
            p++;
        }
    }

    uint32 off = 0;
    p9_put_header(buf, &off, P9_TWALK, p9_next_tag(h));
    p9_put_u32(buf, &off, fid);
    p9_put_u32(buf, &off, newfid);
    p9_put_u16(buf, &off, nwname);

    uint16 i;
    for (i = 0; i < nwname; i++) {
        p9_put_str(buf, &off, components[i]);
    }
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    int32 err = p9_check_error(h->rx_buf, rx_len);
    if (err)
        return err;

    /* P1-4: validate the Rwalk has nwqid == nwname.  9P2000.L allows the
     * server to return a *partial* Rwalk if some elements fail mid-path;
     * newfid is then bound to the partial path and we'd silently operate
     * on the wrong directory.  Treat any short walk as ENOENT and best-
     * effort clunk newfid to release server-side state. */
    err = p9_require_size(rx_len, P9_RHDR_SIZE + 2);  /* +nwqid */
    if (err)
        return err;
    uint32 roff = 7;
    uint16 nwqid = p9_get_u16(h->rx_buf, &roff);
    if (nwqid != nwname) {
        DPRINTF("P9_Walk: partial walk nwqid=%u nwname=%u — treating as ENOENT\n",
                (uint32)nwqid, (uint32)nwname);
        /* Best-effort clunk; if newfid is bound to the partial path the
         * server will release it.  If newfid was never bound (full
         * failure with nwqid=0 and an Rlerror is what we usually see in
         * that case — handled above), Tclunk returns Rlerror EBADF. */
        if (nwqid > 0)
            (void)P9_Clunk(h, newfid);
        return -2;  /* ENOENT */
    }

    return 0;
}

int32 P9_Clunk(struct V9PHandler *h, uint32 fid)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TCLUNK, p9_next_tag(h));
    p9_put_u32(buf, &off, fid);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    return p9_check_error(h->rx_buf, rx_len);
}

int32 P9_Lopen(struct V9PHandler *h, uint32 fid, uint32 flags, uint32 *iounit)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TLOPEN, p9_next_tag(h));
    p9_put_u32(buf, &off, fid);
    p9_put_u32(buf, &off, flags);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    int32 err = p9_check_error(h->rx_buf, rx_len);
    if (err)
        return err;

    /* Rlopen: size[4] type[1] tag[2] qid[13] iounit[4] = 24 bytes. */
    err = p9_require_size(rx_len, 24);
    if (err)
        return err;

    uint32 roff = 7;
    struct P9Qid qid;
    p9_get_qid(h->rx_buf, &roff, &qid);
    if (iounit)
        *iounit = p9_get_u32(h->rx_buf, &roff);

    return 0;
}

int32 P9_Lcreate(struct V9PHandler *h, uint32 dfid, const char *name,
                  uint32 flags, uint32 mode, uint32 *iounit)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TLCREATE, p9_next_tag(h));
    p9_put_u32(buf, &off, dfid);
    p9_put_str(buf, &off, name);
    p9_put_u32(buf, &off, flags);
    p9_put_u32(buf, &off, mode);
    p9_put_u32(buf, &off, 0);  /* gid */
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    int32 err = p9_check_error(h->rx_buf, rx_len);
    if (err)
        return err;

    /* Rlcreate: size[4] type[1] tag[2] qid[13] iounit[4] = 24 bytes. */
    err = p9_require_size(rx_len, 24);
    if (err)
        return err;

    uint32 roff = 7;
    struct P9Qid qid;
    p9_get_qid(h->rx_buf, &roff, &qid);
    if (iounit)
        *iounit = p9_get_u32(h->rx_buf, &roff);

    return 0;
}

int32 P9_Read(struct V9PHandler *h, uint32 fid, uint64 offset,
              uint32 count, void *buf_out, uint32 *actual)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    /* Cap to msize - header overhead (size[4]+type[1]+tag[2]+count[4] = 11) */
    uint32 max_read = h->msize - 11;
    if (count > max_read)
        count = max_read;

    p9_put_header(buf, &off, P9_TREAD, p9_next_tag(h));
    p9_put_u32(buf, &off, fid);
    p9_put_u64(buf, &off, offset);
    p9_put_u32(buf, &off, count);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    int32 err = p9_check_error(h->rx_buf, rx_len);
    if (err)
        return err;

    /* Rread: size[4] type[1] tag[2] count[4] data[count] — 11 bytes + data. */
    err = p9_require_size(rx_len, 11);
    if (err)
        return err;

    uint32 roff = 7;
    uint32 data_count = p9_get_u32(h->rx_buf, &roff);

    if (data_count > count)
        data_count = count;
    /* Clamp to what's actually present in the buffer. */
    if (data_count > rx_len - roff)
        data_count = rx_len - roff;

    memcpy(buf_out, h->rx_buf + roff, data_count);
    if (actual)
        *actual = data_count;

    return 0;
}

int32 P9_Write(struct V9PHandler *h, uint32 fid, uint64 offset,
               uint32 count, const void *buf_in, uint32 *actual)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    /* Cap to msize - header overhead (size[4]+type[1]+tag[2]+fid[4]+offset[8]+count[4] = 23) */
    uint32 max_write = h->msize - 23;
    if (count > max_write)
        count = max_write;

    p9_put_header(buf, &off, P9_TWRITE, p9_next_tag(h));
    p9_put_u32(buf, &off, fid);
    p9_put_u64(buf, &off, offset);
    p9_put_u32(buf, &off, count);

    /* Append data directly */
    memcpy(buf + off, buf_in, count);
    off += count;
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    int32 err = p9_check_error(h->rx_buf, rx_len);
    if (err)
        return err;

    /* Rwrite: size[4] type[1] tag[2] count[4] = 11 bytes. */
    err = p9_require_size(rx_len, 11);
    if (err)
        return err;

    uint32 roff = 7;
    uint32 written = p9_get_u32(h->rx_buf, &roff);
    if (actual)
        *actual = written;

    return 0;
}

int32 P9_Getattr(struct V9PHandler *h, uint32 fid, uint64 mask, struct P9Stat *st)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TGETATTR, p9_next_tag(h));
    p9_put_u32(buf, &off, fid);
    p9_put_u64(buf, &off, mask);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    int32 err = p9_check_error(h->rx_buf, rx_len);
    if (err)
        return err;

    /* Rgetattr: size[4] type[1] tag[2] valid[8] qid[13] mode[4] uid[4] gid[4]
     * nlink[8] rdev[8] size[8] blksize[8] blocks[8]
     * atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8]
     * ctime_sec[8] ctime_nsec[8] btime_sec[8] btime_nsec[8]
     * gen[8] data_version[8]
     * = 7 + 8 + 13 + 12 + 40 + 48 + 32 + 16 = 160 bytes. */
    err = p9_require_size(rx_len, 160);
    if (err)
        return err;

    uint32 roff = 7;
    st->valid      = p9_get_u64(h->rx_buf, &roff);
    p9_get_qid(h->rx_buf, &roff, &st->qid);
    st->mode       = p9_get_u32(h->rx_buf, &roff);
    st->uid        = p9_get_u32(h->rx_buf, &roff);
    st->gid        = p9_get_u32(h->rx_buf, &roff);
    st->nlink      = p9_get_u64(h->rx_buf, &roff);
    st->rdev       = p9_get_u64(h->rx_buf, &roff);
    st->size       = p9_get_u64(h->rx_buf, &roff);
    st->blksize    = p9_get_u64(h->rx_buf, &roff);
    st->blocks     = p9_get_u64(h->rx_buf, &roff);
    st->atime_sec  = p9_get_u64(h->rx_buf, &roff);
    st->atime_nsec = p9_get_u64(h->rx_buf, &roff);
    st->mtime_sec  = p9_get_u64(h->rx_buf, &roff);
    st->mtime_nsec = p9_get_u64(h->rx_buf, &roff);
    st->ctime_sec  = p9_get_u64(h->rx_buf, &roff);
    st->ctime_nsec = p9_get_u64(h->rx_buf, &roff);
    st->btime_sec  = p9_get_u64(h->rx_buf, &roff);
    st->btime_nsec = p9_get_u64(h->rx_buf, &roff);
    st->gen        = p9_get_u64(h->rx_buf, &roff);
    st->data_version = p9_get_u64(h->rx_buf, &roff);

    return 0;
}

int32 P9_Setattr(struct V9PHandler *h, uint32 fid, struct P9Iattr *attr)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TSETATTR, p9_next_tag(h));
    p9_put_u32(buf, &off, fid);
    p9_put_u32(buf, &off, attr->valid);
    p9_put_u32(buf, &off, attr->mode);
    p9_put_u32(buf, &off, attr->uid);
    p9_put_u32(buf, &off, attr->gid);
    p9_put_u64(buf, &off, attr->size);
    p9_put_u64(buf, &off, attr->atime_sec);
    p9_put_u64(buf, &off, attr->atime_nsec);
    p9_put_u64(buf, &off, attr->mtime_sec);
    p9_put_u64(buf, &off, attr->mtime_nsec);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    return p9_check_error(h->rx_buf, rx_len);
}

int32 P9_Statfs(struct V9PHandler *h, uint32 fid, struct P9Statfs *st)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TSTATFS, p9_next_tag(h));
    p9_put_u32(buf, &off, fid);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    int32 err = p9_check_error(h->rx_buf, rx_len);
    if (err)
        return err;

    /* Rstatfs: size[4] type[1] tag[2] type[4] bsize[4] blocks[8] bfree[8]
     * bavail[8] files[8] ffree[8] fsid[8] namelen[4]
     * = 7 + 4 + 4 + 40 + 4 = 59 bytes. */
    err = p9_require_size(rx_len, 59);
    if (err)
        return err;

    uint32 roff = 7;
    st->type    = p9_get_u32(h->rx_buf, &roff);
    st->bsize   = p9_get_u32(h->rx_buf, &roff);
    st->blocks  = p9_get_u64(h->rx_buf, &roff);
    st->bfree   = p9_get_u64(h->rx_buf, &roff);
    st->bavail  = p9_get_u64(h->rx_buf, &roff);
    st->files   = p9_get_u64(h->rx_buf, &roff);
    st->ffree   = p9_get_u64(h->rx_buf, &roff);
    st->fsid    = p9_get_u64(h->rx_buf, &roff);
    st->namelen = p9_get_u32(h->rx_buf, &roff);

    return 0;
}

int32 P9_Readdir(struct V9PHandler *h, uint32 fid, uint64 offset,
                  uint32 count, uint8 **data_out, uint32 *actual)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    uint32 max_read = h->msize - 11;
    if (count > max_read)
        count = max_read;

    p9_put_header(buf, &off, P9_TREADDIR, p9_next_tag(h));
    p9_put_u32(buf, &off, fid);
    p9_put_u64(buf, &off, offset);
    p9_put_u32(buf, &off, count);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    int32 err = p9_check_error(h->rx_buf, rx_len);
    if (err)
        return err;

    /* Rreaddir: size[4] type[1] tag[2] count[4] data[count] — 11 bytes + data. */
    err = p9_require_size(rx_len, 11);
    if (err)
        return err;

    uint32 roff = 7;
    uint32 data_count = p9_get_u32(h->rx_buf, &roff);

    if (data_count > count)
        data_count = count;
    /* Clamp to what's actually present in the buffer so the caller never
     * walks past the R-message when iterating entries. */
    if (data_count > rx_len - roff)
        data_count = rx_len - roff;

    /* Return pointer into rx_buf — caller must process before next Transact */
    if (data_out)
        *data_out = h->rx_buf + roff;
    if (actual)
        *actual = data_count;

    return 0;
}

int32 P9_Mkdir(struct V9PHandler *h, uint32 dfid, const char *name, uint32 mode)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TMKDIR, p9_next_tag(h));
    p9_put_u32(buf, &off, dfid);
    p9_put_str(buf, &off, name);
    p9_put_u32(buf, &off, mode);
    p9_put_u32(buf, &off, 0);  /* gid */
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    return p9_check_error(h->rx_buf, rx_len);
}

int32 P9_Unlinkat(struct V9PHandler *h, uint32 dfid, const char *name, uint32 flags)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TUNLINKAT, p9_next_tag(h));
    p9_put_u32(buf, &off, dfid);
    p9_put_str(buf, &off, name);
    p9_put_u32(buf, &off, flags);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    return p9_check_error(h->rx_buf, rx_len);
}

int32 P9_Renameat(struct V9PHandler *h, uint32 olddirfid, const char *oldname,
                   uint32 newdirfid, const char *newname)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TRENAMEAT, p9_next_tag(h));
    p9_put_u32(buf, &off, olddirfid);
    p9_put_str(buf, &off, oldname);
    p9_put_u32(buf, &off, newdirfid);
    p9_put_str(buf, &off, newname);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    return p9_check_error(h->rx_buf, rx_len);
}

int32 P9_Fsync(struct V9PHandler *h, uint32 fid, uint32 datasync)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TFSYNC, p9_next_tag(h));
    p9_put_u32(buf, &off, fid);
    p9_put_u32(buf, &off, datasync);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    return p9_check_error(h->rx_buf, rx_len);
}

int32 P9_Flush(struct V9PHandler *h, uint16 oldtag)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TFLUSH, p9_next_tag(h));
    p9_put_u16(buf, &off, oldtag);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    return p9_check_error(h->rx_buf, rx_len);
}

int32 P9_Symlink(struct V9PHandler *h, uint32 dfid, const char *name,
                  const char *target)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TSYMLINK, p9_next_tag(h));
    p9_put_u32(buf, &off, dfid);
    p9_put_str(buf, &off, name);
    p9_put_str(buf, &off, target);
    p9_put_u32(buf, &off, 0);  /* gid */
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    return p9_check_error(h->rx_buf, rx_len);
}

int32 P9_Readlink(struct V9PHandler *h, uint32 fid, char *target, uint32 maxlen)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TREADLINK, p9_next_tag(h));
    p9_put_u32(buf, &off, fid);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    int32 err = p9_check_error(h->rx_buf, rx_len);
    if (err)
        return err;

    /* Rreadlink: size[4] type[1] tag[2] target[s] — 9 bytes minimum
     * (empty target). */
    err = p9_require_size(rx_len, 9);
    if (err)
        return err;

    uint32 roff = 7;
    p9_get_str(h->rx_buf, &roff, target, maxlen);

    return 0;
}

int32 P9_Link(struct V9PHandler *h, uint32 dfid, uint32 fid, const char *name)
{
    uint8 *buf = h->tx_buf;
    uint32 off = 0;

    p9_put_header(buf, &off, P9_TLINK, p9_next_tag(h));
    p9_put_u32(buf, &off, dfid);
    p9_put_u32(buf, &off, fid);
    p9_put_str(buf, &off, name);
    p9_finalize(buf, off);

    uint32 rx_len = V9P_Transact(h, off);
    if (rx_len == 0)
        return -5;

    return p9_check_error(h->rx_buf, rx_len);
}
