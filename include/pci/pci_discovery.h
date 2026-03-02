#ifndef PCI_DISCOVERY_H
#define PCI_DISCOVERY_H

#include <exec/types.h>

struct V9PHandler; /* forward declaration */

BOOL V9P_DiscoverDevice(struct V9PHandler *handler);

#endif /* PCI_DISCOVERY_H */
