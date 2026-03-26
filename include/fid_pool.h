#ifndef FID_POOL_H
#define FID_POOL_H

#include <exec/types.h>

/*
 * FID Pool — Simple monotonic FID allocator with free list.
 *
 * FID 0 is reserved for the root directory (from Tattach).
 * Allocated FIDs start at 1 and increment monotonically.
 * Freed FIDs are pushed onto a stack for reuse.
 */

#define FID_POOL_INITIAL_CAPACITY  64

struct FidPool {
    uint32  next_fid;       /* Next fid to allocate (starts at 1) */
    uint32 *free_list;      /* Stack of freed fid numbers */
    uint32  free_count;     /* Number of fids on the free list */
    uint32  free_capacity;  /* Allocated capacity of free_list */
};

struct FidPool *FidPool_Create(void);
void            FidPool_Destroy(struct FidPool *pool);
uint32          FidPool_Alloc(struct FidPool *pool);
void            FidPool_Free(struct FidPool *pool, uint32 fid);

#endif /* FID_POOL_H */
