#ifndef VIRTIO_INIT_H
#define VIRTIO_INIT_H

#include <exec/types.h>

struct V9PHandler; /* forward declaration */

BOOL V9P_InitVirtIO(struct V9PHandler *handler);
void V9P_CleanupVirtIO(struct V9PHandler *handler);
void V9P_ReadMountTag(struct V9PHandler *handler);

/* P1-5 — transport reset.  Tear the virtqueue down, reset the device,
 * re-run the VirtIO handshake, then re-do P9_Version + P9_Attach.
 * Outstanding fids on the server become invalid — caller is responsible
 * for invalidating its FID pool (P1-6).  Returns TRUE on success.
 *
 * Does NOT recursively call V9P_Transact's reset escalation: the call
 * site sets handler->in_reset before calling and clears it after, and
 * V9P_Transact's escalation path checks that flag. */
BOOL V9P_Reset(struct V9PHandler *handler);

#endif /* VIRTIO_INIT_H */
