#ifndef VIRTIO_INIT_H
#define VIRTIO_INIT_H

#include <exec/types.h>

struct V9PHandler; /* forward declaration */

BOOL V9P_InitVirtIO(struct V9PHandler *handler);
void V9P_CleanupVirtIO(struct V9PHandler *handler);
void V9P_ReadMountTag(struct V9PHandler *handler);

#endif /* VIRTIO_INIT_H */
