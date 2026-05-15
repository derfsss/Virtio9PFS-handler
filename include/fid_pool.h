#ifndef FID_POOL_H
#define FID_POOL_H

#include <exec/types.h>

/*
 * FID Pool -- Simple monotonic FID allocator with free list.
 *
 * FID 0 is reserved for the root directory (from Tattach).
 * Allocated FIDs start at 1 and increment monotonically.
 * Freed FIDs are pushed onto a stack for reuse.
 */

#define FID_POOL_INITIAL_CAPACITY  64

struct FidPool {
    uint32  next_fid;          /* Next fid to allocate (starts at 1) */
    uint32 *free_list;         /* Stack of freed fid numbers */
    uint32  free_count;        /* Number of fids on the free list */
    uint32  free_capacity;     /* Allocated capacity of free_list */

    /* P1-6 -- orphan list.  FIDs whose server-side state is uncertain
     * (we sent Twalk/Tlcreate, the transport timed out, the server
     * MAY have allocated the fid).  Never handed out until reclaimed.
     * Today we just leak them; a future revision can periodically send
     * Tclunk on each and on success move them back to free_list. */
    uint32 *orphan_list;
    uint32  orphan_count;
    uint32  orphan_capacity;
};

struct FidPool *FidPool_Create(void);
void            FidPool_Destroy(struct FidPool *pool);
uint32          FidPool_Alloc(struct FidPool *pool);
void            FidPool_Free(struct FidPool *pool, uint32 fid);

/* P1-6 -- mark fid as server-state-unknown.  Pool will not hand it out
 * via Alloc until a future reclaim succeeds.  Use this instead of
 * FidPool_Free when V9P_Transact returns EIO for a Twalk/Tlcreate. */
void            FidPool_MarkOrphan(struct FidPool *pool, uint32 fid);
uint32          FidPool_OrphanCount(const struct FidPool *pool);

#endif /* FID_POOL_H */
