#ifndef P9_CLIENT_H
#define P9_CLIENT_H

#include <exec/types.h>
#include "p9_protocol.h"

struct V9PHandler; /* forward declaration */

/*
 * V9P_Transact — Send a T-message and wait for the R-message.
 *
 * tx_buf must already contain the complete T-message (tx_size bytes).
 * On return, rx_buf contains the R-message.
 * Returns the number of bytes in the R-message, or 0 on error.
 */
uint32 V9P_Transact(struct V9PHandler *h, uint32 tx_size);

/*
 * 9P2000.L Client Session API
 *
 * All functions return 0 on success, or a negative errno on failure.
 */

/* Session lifecycle */
int32 P9_Version(struct V9PHandler *h);
int32 P9_Attach(struct V9PHandler *h, uint32 root_fid);

/* Path operations */
int32 P9_Walk(struct V9PHandler *h, uint32 fid, uint32 newfid, const char *path);
int32 P9_Clunk(struct V9PHandler *h, uint32 fid);

/* File operations */
int32 P9_Lopen(struct V9PHandler *h, uint32 fid, uint32 flags, uint32 *iounit);
int32 P9_Lcreate(struct V9PHandler *h, uint32 dfid, const char *name,
                  uint32 flags, uint32 mode, uint32 *iounit);
int32 P9_Read(struct V9PHandler *h, uint32 fid, uint64 offset,
              uint32 count, void *buf, uint32 *actual);
int32 P9_Write(struct V9PHandler *h, uint32 fid, uint64 offset,
               uint32 count, const void *buf, uint32 *actual);

/* Metadata */
int32 P9_Getattr(struct V9PHandler *h, uint32 fid, uint64 mask, struct P9Stat *st);
int32 P9_Setattr(struct V9PHandler *h, uint32 fid, struct P9Iattr *attr);
int32 P9_Statfs(struct V9PHandler *h, uint32 fid, struct P9Statfs *st);

/* Directory operations */
int32 P9_Readdir(struct V9PHandler *h, uint32 fid, uint64 offset,
                  uint32 count, uint8 **data_out, uint32 *actual);
int32 P9_Mkdir(struct V9PHandler *h, uint32 dfid, const char *name, uint32 mode);
int32 P9_Unlinkat(struct V9PHandler *h, uint32 dfid, const char *name, uint32 flags);
int32 P9_Renameat(struct V9PHandler *h, uint32 olddirfid, const char *oldname,
                   uint32 newdirfid, const char *newname);

#endif /* P9_CLIENT_H */
