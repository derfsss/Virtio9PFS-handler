#ifndef DEBUG_H
#define DEBUG_H

/*
 * Debug output macros for VirtIO 9P handler.
 *
 * Define DEBUG at compile time (-DDEBUG) to enable serial debug output
 * via DebugPrintF (QEMU serial console / Sashimi on real hardware).
 *
 * All debug messages are prefixed with "[virtio9p]" for easy filtering.
 */

#include <proto/exec.h>

/* Debug output prefix — appears at the start of every DPRINTF line */
#define D_PREFIX "[virtio9p] "

#ifdef DEBUG
#define DPRINTF(fmt, ...) IExec->DebugPrintF(D_PREFIX fmt, ##__VA_ARGS__)
#else
#define DPRINTF(...) do { if (0) IExec->DebugPrintF(__VA_ARGS__); } while (0)
#endif

#endif /* DEBUG_H */
