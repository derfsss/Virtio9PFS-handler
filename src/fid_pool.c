#include "fid_pool.h"
#include <proto/exec.h>
#include <exec/memory.h>

/* Create a new FID pool. Returns NULL on allocation failure. */
struct FidPool *FidPool_Create(void)
{
    struct FidPool *pool = (struct FidPool *)IExec->AllocVecTags(
        sizeof(struct FidPool), AVT_ClearWithValue, 0, AVT_Type, MEMF_PRIVATE, TAG_DONE);
    if (!pool)
        return NULL;

    pool->free_list = (uint32 *)IExec->AllocVecTags(
        sizeof(uint32) * FID_POOL_INITIAL_CAPACITY,
        AVT_Type, MEMF_PRIVATE, TAG_DONE);
    if (!pool->free_list) {
        IExec->FreeVec(pool);
        return NULL;
    }

    pool->next_fid = 1;  /* FID 0 reserved for root */
    pool->free_count = 0;
    pool->free_capacity = FID_POOL_INITIAL_CAPACITY;

    return pool;
}

/* Destroy the pool and free all backing memory. */
void FidPool_Destroy(struct FidPool *pool)
{
    if (!pool)
        return;
    if (pool->free_list)
        IExec->FreeVec(pool->free_list);
    IExec->FreeVec(pool);
}

/* Allocate a FID. Reuses freed FIDs first, otherwise increments monotonically.
 * FID 0 is never returned (reserved for root). */
uint32 FidPool_Alloc(struct FidPool *pool)
{
    if (pool->free_count > 0) {
        pool->free_count--;
        return pool->free_list[pool->free_count];
    }
    return pool->next_fid++;
}

/* Return a FID to the pool for reuse. If the free list cannot grow,
 * the FID is leaked rather than crashing the handler. */
void FidPool_Free(struct FidPool *pool, uint32 fid)
{
    if (pool->free_count >= pool->free_capacity) {
        /* Grow the free list */
        uint32 new_cap = pool->free_capacity * 2;
        uint32 *new_list = (uint32 *)IExec->AllocVecTags(
            sizeof(uint32) * new_cap, AVT_Type, MEMF_PRIVATE, TAG_DONE);
        if (!new_list)
            return;  /* Leak the fid rather than crash */

        uint32 i;
        for (i = 0; i < pool->free_count; i++) {
            new_list[i] = pool->free_list[i];
        }
        IExec->FreeVec(pool->free_list);
        pool->free_list = new_list;
        pool->free_capacity = new_cap;
    }

    pool->free_list[pool->free_count] = fid;
    pool->free_count++;
}
