/*
 * VirtIO 9P FileSysBox FUSE Callbacks
 *
 * Each callback uses the global handler to issue 9P operations.
 * FBX runs a single-threaded event loop, so no concurrency concerns.
 */

#include "virtio9p_handler.h"
#include "p9_client.h"
#include "p9_protocol.h"
#include "fid_pool.h"
#include <libraries/filesysbox.h>
#include <proto/exec.h>
#include <proto/filesysbox.h>
#include <errno.h>
#include "string_utils.h"
#include <sys/statvfs.h>

/* Global handler pointer (set by main before FbxSetupFS) */
extern struct V9PHandler *g_handler;

/*
 * Split a path into parent directory and basename.
 * "/foo/bar/baz" → parent="/foo/bar", name="baz"
 * "/test.txt"    → parent="/", name="test.txt"
 */
static void split_path(const char *path, char *parent, int parent_max,
                        char *name, int name_max)
{
    if (strcmp(path, "/") == 0) {
        strncpy(parent, "/", parent_max - 1);
        parent[parent_max - 1] = '\0';
        name[0] = '\0';
        return;
    }

    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) {
        strncpy(parent, "/", parent_max - 1);
        parent[parent_max - 1] = '\0';
        const char *n = (last_slash == path) ? path + 1 : path;
        strncpy(name, n, name_max - 1);
        name[name_max - 1] = '\0';
    } else {
        int plen = (int)(last_slash - path);
        if (plen >= parent_max)
            plen = parent_max - 1;
        memcpy(parent, path, plen);
        parent[plen] = '\0';
        strncpy(name, last_slash + 1, name_max - 1);
        name[name_max - 1] = '\0';
    }
}

/* Walk to path from root, allocate a fid. Caller must Clunk + FidPool_Free. */
static int32 walk_to(struct V9PHandler *h, const char *path, uint32 *out_fid)
{
    uint32 fid = FidPool_Alloc(h->fid_pool);
    int32 err = P9_Walk(h, h->root_fid, fid, path);
    if (err) {
        FidPool_Free(h->fid_pool, fid);
        return err;
    }
    *out_fid = fid;
    return 0;
}

/* Convert P9Stat to fbx_stat */
static void p9stat_to_fbxstat(const struct P9Stat *p9, struct fbx_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode    = (mode_t)p9->mode;
    st->st_nlink   = (nlink_t)p9->nlink;
    st->st_uid     = (fbx_uid_t)p9->uid;
    st->st_gid     = (fbx_gid_t)p9->gid;
    st->st_rdev    = (dev_t)p9->rdev;
    st->st_size    = (fbx_off_t)p9->size;
    st->st_blocks  = (int64)p9->blocks;
    st->st_blksize = (int32)p9->blksize;
    st->st_ino     = p9->qid.path;
    st->st_atime   = (time_t)p9->atime_sec;
    st->st_atimensec = (unsigned int)p9->atime_nsec;
    st->st_mtime   = (time_t)p9->mtime_sec;
    st->st_mtimensec = (unsigned int)p9->mtime_nsec;
    st->st_ctime   = (time_t)p9->ctime_sec;
    st->st_ctimensec = (unsigned int)p9->ctime_nsec;
}

/* ---- FUSE Callbacks ---- */

static int v9p_getattr(const char *path, struct fbx_stat *st)
{
    struct V9PHandler *h = g_handler;
    uint32 fid;

    DPRINTF("getattr: '%s'\n", path);

    int32 err = walk_to(h, path, &fid);
    if (err) {
        DPRINTF("getattr: walk failed: %ld\n", err);
        return (int)err;
    }

    struct P9Stat p9st;
    err = P9_Getattr(h, fid, P9_GETATTR_BASIC, &p9st);
    P9_Clunk(h, fid);
    FidPool_Free(h->fid_pool, fid);

    if (err) {
        DPRINTF("getattr: Getattr failed: %ld\n", err);
        return (int)err;
    }

    p9stat_to_fbxstat(&p9st, st);
    DPRINTF("getattr: '%s' mode=0x%08lX size=%lu\n",
            path, (uint32)p9st.mode, (uint32)p9st.size);
    return 0;
}

static int v9p_opendir(const char *path, struct fuse_file_info *fi)
{
    struct V9PHandler *h = g_handler;
    uint32 fid;

    DPRINTF("opendir: '%s'\n", path);

    int32 err = walk_to(h, path, &fid);
    if (err) {
        DPRINTF("opendir: walk failed: %ld\n", err);
        return (int)err;
    }

    err = P9_Lopen(h, fid, 0 /* O_RDONLY */, NULL);
    if (err) {
        DPRINTF("opendir: Lopen failed: %ld\n", err);
        P9_Clunk(h, fid);
        FidPool_Free(h->fid_pool, fid);
        return (int)err;
    }

    DPRINTF("opendir: '%s' fid=%lu\n", path, fid);
    fi->fh = (long long)fid;
    return 0;
}

static int v9p_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        fbx_off_t offset, struct fuse_file_info *fi)
{
    struct V9PHandler *h = g_handler;
    uint32 fid = (uint32)fi->fh;
    uint64 dir_offset = 0;

    DPRINTF("readdir: '%s' fid=%lu\n", path, fid);

    (void)path;
    (void)offset;

    /* Request up to msize worth of directory data per round-trip.
     * P9_Readdir returns a pointer into rx_buf (zero-copy). */
    uint32 max_count = h->msize - 11;

    for (;;) {
        uint32 actual = 0;
        uint8 *data = NULL;
        int32 err = P9_Readdir(h, fid, dir_offset, max_count,
                                &data, &actual);
        if (err)
            return (int)err;
        if (actual == 0)
            break;

        /* Parse packed 9P readdir entries: qid[13] offset[8] type[1] name[s]
         * Data is in rx_buf — must be fully consumed before next Transact. */
        uint32 pos = 0;
        while (pos < actual) {
            if (pos + 24 > actual)
                break; /* Minimum entry: 13+8+1+2 = 24 bytes */

            struct P9Qid qid;
            p9_get_qid(data, &pos, &qid);
            dir_offset = p9_get_u64(data, &pos);
            uint8 dtype = p9_get_u8(data, &pos);

            char name[256];
            p9_get_str(data, &pos, name, sizeof(name));

            (void)dtype;
            if (filler(buf, name, NULL, 0) != 0)
                return 0;
        }
    }

    return 0;
}

static int v9p_releasedir(const char *path, struct fuse_file_info *fi)
{
    struct V9PHandler *h = g_handler;
    uint32 fid = (uint32)fi->fh;

    DPRINTF("releasedir: '%s' fid=%lu\n", path, fid);

    (void)path;
    P9_Clunk(h, fid);
    FidPool_Free(h->fid_pool, fid);
    return 0;
}

static int v9p_open(const char *path, struct fuse_file_info *fi)
{
    struct V9PHandler *h = g_handler;
    uint32 fid;

    DPRINTF("open: '%s' flags=0x%lX\n", path, (uint32)fi->flags);

    int32 err = walk_to(h, path, &fid);
    if (err)
        return (int)err;

    /* Pass access mode flags to Lopen */
    uint32 flags = (uint32)fi->flags & 0x3; /* O_RDONLY/O_WRONLY/O_RDWR */
    err = P9_Lopen(h, fid, flags, NULL);
    if (err) {
        P9_Clunk(h, fid);
        FidPool_Free(h->fid_pool, fid);
        return (int)err;
    }

    fi->fh = (long long)fid;
    return 0;
}

static int v9p_release(const char *path, struct fuse_file_info *fi)
{
    struct V9PHandler *h = g_handler;
    uint32 fid = (uint32)fi->fh;

    (void)path;
    P9_Clunk(h, fid);
    FidPool_Free(h->fid_pool, fid);
    return 0;
}

static int v9p_read(const char *path, char *buf, size_t size, fbx_off_t off,
                     struct fuse_file_info *fi)
{
    struct V9PHandler *h = g_handler;
    uint32 fid = (uint32)fi->fh;
    uint32 total_read = 0;

    (void)path;

    while (total_read < (uint32)size) {
        uint32 actual = 0;
        uint32 want = (uint32)size - total_read;
        int32 err = P9_Read(h, fid, (uint64)off + total_read, want,
                             buf + total_read, &actual);
        if (err)
            return (int)err;
        if (actual == 0)
            break; /* EOF */
        total_read += actual;
    }

    return (int)total_read;
}

static int v9p_write(const char *path, const char *buf, size_t size, fbx_off_t off,
                      struct fuse_file_info *fi)
{
    struct V9PHandler *h = g_handler;
    uint32 fid = (uint32)fi->fh;
    uint32 total_written = 0;

    (void)path;

    while (total_written < (uint32)size) {
        uint32 actual = 0;
        uint32 want = (uint32)size - total_written;
        int32 err = P9_Write(h, fid, (uint64)off + total_written, want,
                              buf + total_written, &actual);
        if (err)
            return (int)err;
        if (actual == 0)
            break;
        total_written += actual;
    }

    return (int)total_written;
}

static int v9p_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct V9PHandler *h = g_handler;
    char parent[1024], name[256];
    split_path(path, parent, sizeof(parent), name, sizeof(name));

    uint32 dfid;
    int32 err = walk_to(h, parent, &dfid);
    if (err)
        return (int)err;

    /* Lcreate mutates dfid to point to the newly created+opened file */
    uint32 flags = (uint32)fi->flags & 0x3;
    err = P9_Lcreate(h, dfid, name, flags, (uint32)mode, NULL);
    if (err) {
        P9_Clunk(h, dfid);
        FidPool_Free(h->fid_pool, dfid);
        return (int)err;
    }

    fi->fh = (long long)dfid;
    return 0;
}

static int v9p_mkdir(const char *path, mode_t mode)
{
    struct V9PHandler *h = g_handler;
    char parent[1024], name[256];
    split_path(path, parent, sizeof(parent), name, sizeof(name));

    uint32 dfid;
    int32 err = walk_to(h, parent, &dfid);
    if (err)
        return (int)err;

    err = P9_Mkdir(h, dfid, name, (uint32)mode);
    P9_Clunk(h, dfid);
    FidPool_Free(h->fid_pool, dfid);

    return (int)err;
}

static int v9p_unlink(const char *path)
{
    struct V9PHandler *h = g_handler;
    char parent[1024], name[256];
    split_path(path, parent, sizeof(parent), name, sizeof(name));

    uint32 dfid;
    int32 err = walk_to(h, parent, &dfid);
    if (err)
        return (int)err;

    err = P9_Unlinkat(h, dfid, name, 0);
    P9_Clunk(h, dfid);
    FidPool_Free(h->fid_pool, dfid);

    return (int)err;
}

static int v9p_rmdir(const char *path)
{
    struct V9PHandler *h = g_handler;
    char parent[1024], name[256];
    split_path(path, parent, sizeof(parent), name, sizeof(name));

    uint32 dfid;
    int32 err = walk_to(h, parent, &dfid);
    if (err)
        return (int)err;

    err = P9_Unlinkat(h, dfid, name, P9_AT_REMOVEDIR);
    P9_Clunk(h, dfid);
    FidPool_Free(h->fid_pool, dfid);

    return (int)err;
}

static int v9p_rename(const char *oldpath, const char *newpath)
{
    struct V9PHandler *h = g_handler;
    char oldparent[1024], oldname[256];
    char newparent[1024], newname[256];

    split_path(oldpath, oldparent, sizeof(oldparent), oldname, sizeof(oldname));
    split_path(newpath, newparent, sizeof(newparent), newname, sizeof(newname));

    uint32 olddirfid, newdirfid;
    int32 err = walk_to(h, oldparent, &olddirfid);
    if (err)
        return (int)err;

    err = walk_to(h, newparent, &newdirfid);
    if (err) {
        P9_Clunk(h, olddirfid);
        FidPool_Free(h->fid_pool, olddirfid);
        return (int)err;
    }

    err = P9_Renameat(h, olddirfid, oldname, newdirfid, newname);
    P9_Clunk(h, olddirfid);
    P9_Clunk(h, newdirfid);
    FidPool_Free(h->fid_pool, olddirfid);
    FidPool_Free(h->fid_pool, newdirfid);

    return (int)err;
}

static int v9p_truncate(const char *path, fbx_off_t size)
{
    struct V9PHandler *h = g_handler;
    uint32 fid;

    int32 err = walk_to(h, path, &fid);
    if (err)
        return (int)err;

    struct P9Iattr attr;
    memset(&attr, 0, sizeof(attr));
    attr.valid = P9_SETATTR_SIZE;
    attr.size = (uint64)size;

    err = P9_Setattr(h, fid, &attr);
    P9_Clunk(h, fid);
    FidPool_Free(h->fid_pool, fid);

    return (int)err;
}

static int v9p_ftruncate(const char *path, fbx_off_t size,
                         struct fuse_file_info *fi)
{
    struct V9PHandler *h = g_handler;
    uint32 fid = (uint32)fi->fh;

    (void)path;

    DPRINTF("ftruncate: fid=%lu size=%lld\n", (unsigned long)fid, (long long)size);

    struct P9Iattr attr;
    memset(&attr, 0, sizeof(attr));
    attr.valid = P9_SETATTR_SIZE;
    attr.size = (uint64)size;

    int32 err = P9_Setattr(h, fid, &attr);
    return (int)err;
}

static int v9p_statfs(const char *path, struct statvfs *st)
{
    struct V9PHandler *h = g_handler;
    struct P9Statfs p9st;

    (void)path;
    DPRINTF("statfs: '%s'\n", path);

    int32 err = P9_Statfs(h, h->root_fid, &p9st);
    if (err) {
        DPRINTF("statfs: failed: %ld\n", err);
        return (int)err;
    }

    memset(st, 0, sizeof(*st));
    st->f_bsize   = p9st.bsize;
    st->f_frsize  = p9st.bsize;
    st->f_blocks  = (unsigned long)p9st.blocks;
    st->f_bfree   = (unsigned long)p9st.bfree;
    st->f_bavail  = (unsigned long)p9st.bavail;
    st->f_files   = (unsigned long)p9st.files;
    st->f_ffree   = (unsigned long)p9st.ffree;
    st->f_favail  = (unsigned long)p9st.ffree;
    st->f_fsid    = (unsigned long)p9st.fsid;
    st->f_namemax = p9st.namelen;

    return 0;
}

static int v9p_utimens(const char *path, const struct timespec tv[2])
{
    struct V9PHandler *h = g_handler;
    uint32 fid;

    int32 err = walk_to(h, path, &fid);
    if (err)
        return (int)err;

    struct P9Iattr attr;
    memset(&attr, 0, sizeof(attr));
    attr.valid     = P9_SETATTR_ATIME | P9_SETATTR_ATIME_SET |
                     P9_SETATTR_MTIME | P9_SETATTR_MTIME_SET;
    attr.atime_sec  = (uint64)tv[0].tv_sec;
    attr.atime_nsec = (uint64)tv[0].tv_nsec;
    attr.mtime_sec  = (uint64)tv[1].tv_sec;
    attr.mtime_nsec = (uint64)tv[1].tv_nsec;

    err = P9_Setattr(h, fid, &attr);
    P9_Clunk(h, fid);
    FidPool_Free(h->fid_pool, fid);

    return (int)err;
}

static int v9p_chmod(const char *path, mode_t mode)
{
    struct V9PHandler *h = g_handler;
    uint32 fid;

    DPRINTF("chmod: '%s' mode=0%lo\n", path, (unsigned long)mode);

    int32 err = walk_to(h, path, &fid);
    if (err)
        return (int)err;

    /* Read current mode to preserve file type bits (S_IFREG/S_IFDIR etc.)
     * FBX passes only permission bits; without merging, QEMU would write
     * a broken mode missing the file type, corrupting mapped-xattr metadata. */
    struct P9Stat p9st;
    err = P9_Getattr(h, fid, P9_GETATTR_MODE, &p9st);
    if (err) {
        DPRINTF("chmod: Getattr failed: %ld\n", err);
        P9_Clunk(h, fid);
        FidPool_Free(h->fid_pool, fid);
        return (int)err;
    }

    struct P9Iattr attr;
    memset(&attr, 0, sizeof(attr));
    attr.valid = P9_SETATTR_MODE;
    attr.mode = (p9st.mode & ~07777) | ((uint32)mode & 07777);

    err = P9_Setattr(h, fid, &attr);
    P9_Clunk(h, fid);
    FidPool_Free(h->fid_pool, fid);

    return (int)err;
}

static int v9p_chown(const char *path, fbx_uid_t uid, fbx_gid_t gid)
{
    struct V9PHandler *h = g_handler;
    uint32 fid;

    DPRINTF("chown: '%s' uid=%lu gid=%lu\n", path,
            (unsigned long)uid, (unsigned long)gid);

    int32 err = walk_to(h, path, &fid);
    if (err)
        return (int)err;

    struct P9Iattr attr;
    memset(&attr, 0, sizeof(attr));
    attr.valid = P9_SETATTR_UID | P9_SETATTR_GID;
    attr.uid = (uint32)uid;
    attr.gid = (uint32)gid;

    err = P9_Setattr(h, fid, &attr);
    P9_Clunk(h, fid);
    FidPool_Free(h->fid_pool, fid);

    return (int)err;
}

static void *v9p_init(struct fuse_conn_info *conn)
{
    struct V9PHandler *h = g_handler;

    if (conn) {
        strncpy(conn->volume_name, h->mount_tag, sizeof(conn->volume_name) - 1);
        conn->volume_name[sizeof(conn->volume_name) - 1] = '\0';
    }

    DPRINTF("FUSE init: volume=%s\n", h->mount_tag);
    return h;
}

static void v9p_destroy(void *userdata)
{
    (void)userdata;
    DPRINTF("FUSE destroy\n");
}

/* Fill the fuse_operations table with our callbacks */
void V9P_FillOperations(struct fuse_operations *ops)
{
    memset(ops, 0, sizeof(*ops));

    ops->init       = v9p_init;
    ops->destroy    = v9p_destroy;
    ops->getattr    = v9p_getattr;
    ops->opendir    = v9p_opendir;
    ops->readdir    = v9p_readdir;
    ops->releasedir = v9p_releasedir;
    ops->open       = v9p_open;
    ops->release    = v9p_release;
    ops->read       = v9p_read;
    ops->write      = v9p_write;
    ops->create     = v9p_create;
    ops->mkdir      = v9p_mkdir;
    ops->unlink     = v9p_unlink;
    ops->rmdir      = v9p_rmdir;
    ops->rename     = v9p_rename;
    ops->truncate   = v9p_truncate;
    ops->ftruncate  = v9p_ftruncate;
    ops->chmod      = v9p_chmod;
    ops->chown      = v9p_chown;
    ops->statfs     = v9p_statfs;
    ops->utimens    = v9p_utimens;
}
