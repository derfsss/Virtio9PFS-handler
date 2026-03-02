#ifndef VIRTIO_IRQ_H
#define VIRTIO_IRQ_H

#include <exec/types.h>

struct V9PHandler; /* forward declaration */

BOOL V9P_InstallInterrupt(struct V9PHandler *handler);
void V9P_RemoveInterrupt(struct V9PHandler *handler);

#endif /* VIRTIO_IRQ_H */
