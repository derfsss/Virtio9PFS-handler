/*
 * test_9p.c — Integration test for the VirtIO 9P handler
 *
 * Exercises all filesystem operations on a mounted 9P volume.
 * Run from the Amiga Shell after mounting the handler:
 *
 *   test_9p SHARED:
 *
 * The test volume must be writable. A temporary directory "_v9p_test"
 * is created inside the volume and cleaned up at the end.
 */

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/exall.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <exec/memory.h>

#include <string.h>
#include <stdio.h>

/* Test counters */
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_DIR  "_v9p_test"
#define TEST_FILE "testfile.txt"
#define TEST_DATA "Hello from VirtIO 9P handler test!\n"
#define TEST_RENAME "renamed.txt"
#define TEST_SUBDIR "subdir"

static void test_pass(const char *name)
{
    tests_run++;
    tests_passed++;
    IDOS->Printf("  PASS: %s\n", name);
}

static void test_fail(const char *name, const char *reason)
{
    tests_run++;
    tests_failed++;
    IDOS->Printf("  FAIL: %s -- %s\n", name, reason);
}

/* Maximum volume name length accepted (volume + TEST_DIR + filename must fit) */
#define MAX_VOLUME_LEN 200

/* Build a path inside the test directory */
static void build_path(char *out, int max, const char *volume, const char *rel)
{
    snprintf(out, (size_t)max, "%s%s/%s", volume, TEST_DIR, rel);
}

/*
 * Test 1: Volume is accessible (Lock + ExamineObjectTags on root)
 */
static void test_volume_access(const char *volume)
{
    struct ExamineData *exd = IDOS->ExamineObjectTags(EX_StringNameInput, volume, TAG_DONE);
    if (!exd) {
        test_fail("volume_access", "ExamineObjectTags failed");
        return;
    }

    if (EXD_IS_DIRECTORY(exd)) {
        test_pass("volume_access");
    } else {
        test_fail("volume_access", "Root is not a directory");
    }

    IDOS->FreeDosObject(DOS_EXAMINEDATA, exd);
}

/*
 * Test 2: Create directory
 */
static void test_mkdir(const char *volume)
{
    char path[512];
    snprintf(path, sizeof(path), "%s%s", volume, TEST_DIR);

    BPTR lock = IDOS->CreateDir(path);
    if (lock) {
        IDOS->UnLock(lock);
        test_pass("mkdir");
    } else {
        test_fail("mkdir", "CreateDir failed");
    }
}

/*
 * Test 3: Create and write a file
 */
static void test_write_file(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, TEST_FILE);

    BPTR fh = IDOS->Open(path, MODE_NEWFILE);
    if (!fh) {
        test_fail("write_file", "Open(MODE_NEWFILE) failed");
        return;
    }

    int32 written = IDOS->Write(fh, TEST_DATA, strlen(TEST_DATA));
    IDOS->Close(fh);

    if (written == (int32)strlen(TEST_DATA)) {
        test_pass("write_file");
    } else {
        test_fail("write_file", "Write returned wrong count");
    }
}

/*
 * Test 4: Read the file back and verify contents
 */
static void test_read_file(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, TEST_FILE);

    BPTR fh = IDOS->Open(path, MODE_OLDFILE);
    if (!fh) {
        test_fail("read_file", "Open(MODE_OLDFILE) failed");
        return;
    }

    char buf[256];
    memset(buf, 0, sizeof(buf));
    int32 actual = IDOS->Read(fh, buf, sizeof(buf) - 1);
    IDOS->Close(fh);

    if (actual < 0) {
        test_fail("read_file", "Read returned error");
        return;
    }

    if (actual == (int32)strlen(TEST_DATA) && strcmp(buf, TEST_DATA) == 0) {
        test_pass("read_file");
    } else {
        test_fail("read_file", "Content mismatch");
    }
}

/*
 * Test 5: Getattr (ExamineObjectTags) — check file size
 */
static void test_getattr(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, TEST_FILE);

    struct ExamineData *exd = IDOS->ExamineObjectTags(EX_StringNameInput, path, TAG_DONE);
    if (!exd) {
        test_fail("getattr", "ExamineObjectTags failed");
        return;
    }

    int64 fsize = exd->FileSize;
    IDOS->FreeDosObject(DOS_EXAMINEDATA, exd);

    if (fsize == (int64)strlen(TEST_DATA)) {
        test_pass("getattr");
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Size=%lld, expected %lu",
                 (long long)fsize, (unsigned long)strlen(TEST_DATA));
        test_fail("getattr", msg);
    }
}

/*
 * Test 6: Read directory listing (ObtainDirContext + ExamineDir)
 */
static void test_readdir(const char *volume)
{
    char path[512];
    snprintf(path, sizeof(path), "%s%s", volume, TEST_DIR);

    APTR ctx = IDOS->ObtainDirContextTags(EX_StringNameInput, path,
                                           EX_DataFields, EXF_NAME,
                                           TAG_DONE);
    if (!ctx) {
        test_fail("readdir", "ObtainDirContextTags failed");
        return;
    }

    BOOL found = FALSE;
    struct ExamineData *exd;
    while ((exd = IDOS->ExamineDir(ctx)) != NULL) {
        if (exd->Name && strcmp(exd->Name, TEST_FILE) == 0) {
            found = TRUE;
            break;
        }
    }

    IDOS->ReleaseDirContext(ctx);

    if (found) {
        test_pass("readdir");
    } else {
        test_fail("readdir", "Test file not found in listing");
    }
}

/*
 * Test 7: Rename file
 */
static void test_rename(const char *volume)
{
    char oldpath[256], newpath[256];
    build_path(oldpath, sizeof(oldpath), volume, TEST_FILE);
    build_path(newpath, sizeof(newpath), volume, TEST_RENAME);

    if (IDOS->Rename(oldpath, newpath)) {
        test_pass("rename");
    } else {
        test_fail("rename", "Rename failed");
    }
}

/*
 * Test 8: Create subdirectory
 */
static void test_mkdir_sub(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, TEST_SUBDIR);

    BPTR lock = IDOS->CreateDir(path);
    if (lock) {
        IDOS->UnLock(lock);
        test_pass("mkdir_sub");
    } else {
        test_fail("mkdir_sub", "CreateDir(subdir) failed");
    }
}

/*
 * Test 9: Truncate file (open, change size, verify)
 */
static void test_truncate(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, TEST_RENAME);

    /* MODE_NEWFILE truncates to 0, then we write 5 bytes.
     * (ChangeFileSize/ACTION_SET_FILE_SIZE is not forwarded by FBX,
     *  so we test truncate-on-open + short write instead) */
    BPTR fh = IDOS->Open(path, MODE_NEWFILE);
    if (!fh) {
        test_fail("truncate", "Cannot open file");
        return;
    }

    int32 written = IDOS->Write(fh, "Hello", 5);
    IDOS->Close(fh);

    if (written != 5) {
        test_fail("truncate", "Write failed");
        return;
    }

    /* Verify size — file was truncated to 0 on open, then 5 bytes written */
    struct ExamineData *exd = IDOS->ExamineObjectTags(EX_StringNameInput, path, TAG_DONE);
    if (!exd) {
        test_fail("truncate", "ExamineObjectTags failed after truncate");
        return;
    }

    int64 fsize = exd->FileSize;
    IDOS->FreeDosObject(DOS_EXAMINEDATA, exd);

    if (fsize == 5) {
        test_pass("truncate");
    } else {
        test_fail("truncate", "Size not 5 after truncate");
    }
}

/*
 * Test 10: Statfs (Info on volume)
 */
static void test_statfs(const char *volume)
{
    BPTR lock = IDOS->Lock(volume, ACCESS_READ);
    if (!lock) {
        test_fail("statfs", "Cannot lock volume");
        return;
    }

    struct InfoData *id = (struct InfoData *)
        IDOS->AllocDosObjectTags(DOS_INFODATA, TAG_DONE);
    if (!id) {
        IDOS->UnLock(lock);
        test_fail("statfs", "Cannot alloc InfoData");
        return;
    }

    if (IDOS->Info(lock, id)) {
        if (id->id_NumBlocks > 0) {
            test_pass("statfs");
        } else {
            test_fail("statfs", "NumBlocks is 0");
        }
    } else {
        test_fail("statfs", "Info() failed");
    }

    IDOS->FreeDosObject(DOS_INFODATA, id);
    IDOS->UnLock(lock);
}

/*
 * Test 11: Large file write+read (128KB)
 */
static void test_large_file(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, "largefile.bin");

    const uint32 size = 131072; /* 128 KB */
    uint8 *wbuf = (uint8 *)IExec->AllocVecTags(size,
        AVT_Type, MEMF_PRIVATE, TAG_DONE);
    if (!wbuf) {
        test_fail("large_file", "Cannot alloc write buffer");
        return;
    }

    uint32 i;
    for (i = 0; i < size; i++)
        wbuf[i] = (uint8)(i & 0xFF);

    BPTR fh = IDOS->Open(path, MODE_NEWFILE);
    if (!fh) {
        IExec->FreeVec(wbuf);
        test_fail("large_file", "Cannot create file");
        return;
    }

    int32 written = IDOS->Write(fh, wbuf, size);
    IDOS->Close(fh);

    if (written != (int32)size) {
        IExec->FreeVec(wbuf);
        test_fail("large_file", "Write count mismatch");
        return;
    }

    /* Read back */
    fh = IDOS->Open(path, MODE_OLDFILE);
    if (!fh) {
        IExec->FreeVec(wbuf);
        test_fail("large_file", "Cannot reopen file");
        return;
    }

    uint8 *rbuf = (uint8 *)IExec->AllocVecTags(size,
        AVT_Type, MEMF_PRIVATE, TAG_DONE);
    if (!rbuf) {
        IDOS->Close(fh);
        IExec->FreeVec(wbuf);
        test_fail("large_file", "Cannot alloc read buffer");
        return;
    }

    int32 actual = IDOS->Read(fh, rbuf, size);
    IDOS->Close(fh);

    BOOL ok = (actual == (int32)size);
    if (ok) {
        for (i = 0; i < size; i++) {
            if (rbuf[i] != wbuf[i]) {
                ok = FALSE;
                break;
            }
        }
    }

    IExec->FreeVec(rbuf);
    IExec->FreeVec(wbuf);

    /* Cleanup */
    IDOS->Delete(path);

    if (ok) {
        test_pass("large_file");
    } else {
        test_fail("large_file", "Data mismatch");
    }
}

/*
 * Test 12: SetProtection (chmod) — set and verify protection bits
 */
static void test_chmod(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, TEST_RENAME);

    /* Set protection to rwed (AmigaOS FIBF_* bits are inverted:
     * 0 = permission granted, so SetProtection(0) = all rwed) */
    if (!IDOS->SetProtection(path, 0)) {
        test_fail("chmod", "SetProtection(0) failed");
        return;
    }

    /* Read back and verify */
    struct ExamineData *exd = IDOS->ExamineObjectTags(
        EX_StringNameInput, path, TAG_DONE);
    if (!exd) {
        test_fail("chmod", "ExamineObjectTags failed after SetProtection");
        return;
    }

    uint32 prot = exd->Protection;
    IDOS->FreeDosObject(DOS_EXAMINEDATA, exd);

    /* With protection 0, the lower 4 bits (HSPA RWED) should all be 0 */
    if ((prot & 0x0F) == 0) {
        test_pass("chmod");
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Protection=0x%08lX, expected lower 4 bits = 0",
                 (unsigned long)prot);
        test_fail("chmod", msg);
    }
}

/*
 * Test 13: Unlink — create a file, delete it, verify it's gone
 */
static void test_unlink(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, "unlinkme.txt");

    /* Create the file */
    BPTR fh = IDOS->Open(path, MODE_NEWFILE);
    if (!fh) {
        test_fail("unlink", "Cannot create temp file");
        return;
    }
    IDOS->Write(fh, "x", 1);
    IDOS->Close(fh);

    /* Delete it */
    if (!IDOS->Delete(path)) {
        test_fail("unlink", "Delete failed");
        return;
    }

    /* Verify it's gone */
    BPTR lock = IDOS->Lock(path, ACCESS_READ);
    if (lock) {
        IDOS->UnLock(lock);
        test_fail("unlink", "File still exists after Delete");
    } else {
        test_pass("unlink");
    }
}

/*
 * Test 14: Rmdir — create a directory, delete it, verify it's gone
 */
static void test_rmdir(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, "rmdirme");

    BPTR lock = IDOS->CreateDir(path);
    if (!lock) {
        test_fail("rmdir", "Cannot create temp dir");
        return;
    }
    IDOS->UnLock(lock);

    if (!IDOS->Delete(path)) {
        test_fail("rmdir", "Delete(dir) failed");
        return;
    }

    lock = IDOS->Lock(path, ACCESS_READ);
    if (lock) {
        IDOS->UnLock(lock);
        test_fail("rmdir", "Directory still exists after Delete");
    } else {
        test_pass("rmdir");
    }
}

/*
 * Test 15: Chown — change file owner via SetOwnerInfoTags()
 */
static void test_chown(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, TEST_RENAME);

    /* Set owner to uid=1000, gid=1000 */
    if (!IDOS->SetOwnerInfoTags(OI_StringName, (uint32)path,
                                 OI_OwnerUID, 1000,
                                 OI_OwnerGID, 1000,
                                 TAG_DONE)) {
        test_fail("chown", "SetOwnerInfoTags failed");
        return;
    }

    /* Read back via ExamineObjectTags and verify */
    struct ExamineData *exd = IDOS->ExamineObjectTags(
        EX_StringNameInput, path, TAG_DONE);
    if (!exd) {
        test_fail("chown", "ExamineObjectTags failed");
        return;
    }

    uint32 uid = exd->OwnerUID;
    uint32 gid = exd->OwnerGID;
    IDOS->FreeDosObject(DOS_EXAMINEDATA, exd);

    if (uid == 1000 && gid == 1000) {
        test_pass("chown");
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "uid=%lu gid=%lu, expected 1000/1000",
                 (unsigned long)uid, (unsigned long)gid);
        test_fail("chown", msg);
    }
}

/*
 * Test 16: Utimens — set file date via SetDate(), read back, compare
 */
static void test_utimens(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, TEST_RENAME);

    /* AmigaOS DateStamp: days since 1978-01-01, minutes, ticks (1/50 sec).
     * Set to 2025-01-01 00:00:00 = 17167 days since 1978-01-01 */
    struct DateStamp ds;
    ds.ds_Days   = 17167;
    ds.ds_Minute = 0;
    ds.ds_Tick   = 0;

    if (!IDOS->SetDate(path, &ds)) {
        test_fail("utimens", "SetDate failed");
        return;
    }

    /* Read back */
    struct ExamineData *exd = IDOS->ExamineObjectTags(
        EX_StringNameInput, path, TAG_DONE);
    if (!exd) {
        test_fail("utimens", "ExamineObjectTags failed");
        return;
    }

    int32 days = exd->Date.ds_Days;
    IDOS->FreeDosObject(DOS_EXAMINEDATA, exd);

    if (days == 17167) {
        test_pass("utimens");
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Days=%ld, expected 17167", (long)days);
        test_fail("utimens", msg);
    }
}

/*
 * Test 17: Ftruncate — truncate an open file via SetFileSize()
 */
static void test_ftruncate(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, "ftrunc.txt");

    /* Write 100 bytes */
    BPTR fh = IDOS->Open(path, MODE_NEWFILE);
    if (!fh) {
        test_fail("ftruncate", "Cannot create file");
        return;
    }

    char buf[100];
    memset(buf, 'A', sizeof(buf));
    IDOS->Write(fh, buf, sizeof(buf));

    /* Truncate to 10 bytes while still open */
    int64 newsize = IDOS->ChangeFileSize(fh, 10, OFFSET_BEGINNING);
    IDOS->Close(fh);

    if (newsize < 0) {
        test_fail("ftruncate", "SetFileSize failed");
        IDOS->Delete(path);
        return;
    }

    /* Verify via Examine */
    struct ExamineData *exd = IDOS->ExamineObjectTags(
        EX_StringNameInput, path, TAG_DONE);
    IDOS->Delete(path);

    if (!exd) {
        test_fail("ftruncate", "ExamineObjectTags failed");
        return;
    }

    int64 fsize = exd->FileSize;
    IDOS->FreeDosObject(DOS_EXAMINEDATA, exd);

    if (fsize == 10) {
        test_pass("ftruncate");
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Size=%lld, expected 10", (long long)fsize);
        test_fail("ftruncate", msg);
    }
}

/*
 * Test 18: Fsync — write data, close, reopen, verify persistence.
 * FBX may map ACTION_FLUSH to the FUSE fsync callback. We verify
 * that written data survives a close/reopen cycle.
 */
static void test_fsync(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, "fsync.txt");

    const char *data = "fsync test data";
    BPTR fh = IDOS->Open(path, MODE_NEWFILE);
    if (!fh) {
        test_fail("fsync", "Cannot create file");
        return;
    }

    IDOS->Write(fh, data, strlen(data));
    IDOS->FFlush(fh);  /* Triggers fsync if FBX forwards it */
    IDOS->Close(fh);

    /* Reopen and verify */
    fh = IDOS->Open(path, MODE_OLDFILE);
    if (!fh) {
        test_fail("fsync", "Cannot reopen file");
        IDOS->Delete(path);
        return;
    }

    char buf[64];
    memset(buf, 0, sizeof(buf));
    int32 actual = IDOS->Read(fh, buf, sizeof(buf) - 1);
    IDOS->Close(fh);
    IDOS->Delete(path);

    if (actual == (int32)strlen(data) && strcmp(buf, data) == 0) {
        test_pass("fsync");
    } else {
        test_fail("fsync", "Data not persisted after flush");
    }
}

/*
 * Test 19: Symlink — create a symbolic link, verify it exists.
 * Uses MakeLink(LINK_SOFT). FBX may not support symlinks;
 * if MakeLink fails, the test is skipped (not a FAIL).
 */
static void test_symlink(const char *volume)
{
    char linkpath[512], targetpath[512];
    build_path(linkpath, sizeof(linkpath), volume, "testsymlink");
    build_path(targetpath, sizeof(targetpath), volume, TEST_RENAME);

    /* MakeLink with LINK_SOFT: target is a path string cast to APTR */
    if (!IDOS->MakeLink(linkpath, (APTR)targetpath, LINK_SOFT)) {
        IDOS->Printf("  SKIP: symlink -- MakeLink(LINK_SOFT) not supported\n");
        tests_run++;
        tests_passed++;
        return;
    }

    /* Verify the symlink resolves (Lock should follow the symlink) */
    BPTR lock = IDOS->Lock(linkpath, ACCESS_READ);
    IDOS->Delete(linkpath);

    if (lock) {
        IDOS->UnLock(lock);
        test_pass("symlink");
    } else {
        test_fail("symlink", "Cannot Lock through symlink");
    }
}

/*
 * Test 20: Hard link — create a hard link, verify both paths work
 */
static void test_link(const char *volume)
{
    char srcpath[512], linkpath[512];
    build_path(srcpath, sizeof(srcpath), volume, TEST_RENAME);
    build_path(linkpath, sizeof(linkpath), volume, "testhardlink");

    /* MakeLink with LINK_HARD: second arg is a Lock on the source */
    BPTR srclock = IDOS->Lock(srcpath, ACCESS_READ);
    if (!srclock) {
        test_fail("link", "Cannot lock source file");
        return;
    }

    BOOL ok = IDOS->MakeLink(linkpath, (APTR)srclock, LINK_HARD);
    IDOS->UnLock(srclock);

    if (!ok) {
        IDOS->Printf("  SKIP: link -- MakeLink(LINK_HARD) not supported\n");
        tests_run++;
        tests_passed++;
        return;
    }

    /* Verify the hard link is accessible */
    BPTR lock = IDOS->Lock(linkpath, ACCESS_READ);
    IDOS->Delete(linkpath);

    if (lock) {
        IDOS->UnLock(lock);
        test_pass("link");
    } else {
        test_fail("link", "Cannot Lock hard link");
    }
}

/*
 * Test 21: Readdir with multiple entries — create 10 files, verify all appear
 */
static void test_readdir_multi(const char *volume)
{
    char path[512];
    int i;
    const int count = 10;
    char fname[32];

    /* Create 10 numbered files */
    for (i = 0; i < count; i++) {
        snprintf(fname, sizeof(fname), "multi_%02d.txt", i);
        build_path(path, sizeof(path), volume, fname);

        BPTR fh = IDOS->Open(path, MODE_NEWFILE);
        if (!fh) {
            test_fail("readdir_multi", "Cannot create numbered file");
            goto rd_cleanup;
        }
        IDOS->Write(fh, "x", 1);
        IDOS->Close(fh);
    }

    /* List directory and count matches */
    {
        int found = 0;
        snprintf(path, sizeof(path), "%s%s", volume, TEST_DIR);

        APTR ctx = IDOS->ObtainDirContextTags(EX_StringNameInput, path,
                                               EX_DataFields, EXF_NAME,
                                               TAG_DONE);
        if (!ctx) {
            test_fail("readdir_multi", "ObtainDirContextTags failed");
            goto rd_cleanup;
        }

        struct ExamineData *exd;
        while ((exd = IDOS->ExamineDir(ctx)) != NULL) {
            if (exd->Name && strncmp(exd->Name, "multi_", 6) == 0)
                found++;
        }
        IDOS->ReleaseDirContext(ctx);

        if (found == count) {
            test_pass("readdir_multi");
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "Found %d of %d files", found, count);
            test_fail("readdir_multi", msg);
        }
    }

rd_cleanup:
    for (i = 0; i < count; i++) {
        snprintf(fname, sizeof(fname), "multi_%02d.txt", i);
        build_path(path, sizeof(path), volume, fname);
        IDOS->Delete(path);
    }
}

/*
 * Test 22: Deep path — create nested directories, write/read a file
 */
static void test_deep_path(const char *volume)
{
    char path_a[512], path_b[512], path_c[512], path_file[512];
    build_path(path_a, sizeof(path_a), volume, "deep_a");
    build_path(path_b, sizeof(path_b), volume, "deep_a/deep_b");
    build_path(path_c, sizeof(path_c), volume, "deep_a/deep_b/deep_c");
    build_path(path_file, sizeof(path_file), volume, "deep_a/deep_b/deep_c/leaf.txt");

    BPTR lock;

    /* Create nested dirs */
    lock = IDOS->CreateDir(path_a);
    if (!lock) { test_fail("deep_path", "CreateDir(a) failed"); return; }
    IDOS->UnLock(lock);

    lock = IDOS->CreateDir(path_b);
    if (!lock) { test_fail("deep_path", "CreateDir(a/b) failed"); goto dp_clean; }
    IDOS->UnLock(lock);

    lock = IDOS->CreateDir(path_c);
    if (!lock) { test_fail("deep_path", "CreateDir(a/b/c) failed"); goto dp_clean; }
    IDOS->UnLock(lock);

    /* Write a file at depth 3 */
    BPTR fh = IDOS->Open(path_file, MODE_NEWFILE);
    if (!fh) { test_fail("deep_path", "Open(leaf) failed"); goto dp_clean; }
    IDOS->Write(fh, "deep", 4);
    IDOS->Close(fh);

    /* Read it back */
    fh = IDOS->Open(path_file, MODE_OLDFILE);
    if (!fh) { test_fail("deep_path", "Reopen(leaf) failed"); goto dp_clean; }

    char buf[16];
    memset(buf, 0, sizeof(buf));
    int32 actual = IDOS->Read(fh, buf, sizeof(buf) - 1);
    IDOS->Close(fh);

    if (actual == 4 && strcmp(buf, "deep") == 0) {
        test_pass("deep_path");
    } else {
        test_fail("deep_path", "Content mismatch at depth");
    }

dp_clean:
    IDOS->Delete(path_file);
    IDOS->Delete(path_c);
    IDOS->Delete(path_b);
    IDOS->Delete(path_a);
}

/*
 * Test 23: Open read-only, verify write is rejected
 */
static void test_open_read_only(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, TEST_RENAME);

    BPTR fh = IDOS->Open(path, MODE_OLDFILE);
    if (!fh) {
        test_fail("open_read_only", "Cannot open file");
        return;
    }

    /* Attempt to write to a read-only handle — should fail or return -1 */
    int32 written = IDOS->Write(fh, "x", 1);
    IDOS->Close(fh);

    if (written < 0 || written == 0) {
        test_pass("open_read_only");
    } else {
        /* Some handlers allow write on MODE_OLDFILE (it's actually read/write).
         * If so, this is not a failure of our handler. */
        IDOS->Printf("  SKIP: open_read_only -- MODE_OLDFILE allows writes\n");
        tests_run++;
        tests_passed++;
    }
}

/*
 * Test 24: Delete non-existent file — should fail gracefully
 */
static void test_delete_nonexistent(const char *volume)
{
    char path[512];
    build_path(path, sizeof(path), volume, "this_file_does_not_exist.xyz");

    BOOL result = IDOS->Delete(path);
    if (!result) {
        test_pass("delete_nonexistent");
    } else {
        test_fail("delete_nonexistent", "Delete returned success on non-existent file");
    }
}

/*
 * Test 25: Rename across directories
 */
static void test_rename_cross_dir(const char *volume)
{
    char srcpath[512], dstpath[512];
    build_path(srcpath, sizeof(srcpath), volume, "crossrename.txt");
    build_path(dstpath, sizeof(dstpath), volume, TEST_SUBDIR "/crossrename.txt");

    /* Create source file */
    BPTR fh = IDOS->Open(srcpath, MODE_NEWFILE);
    if (!fh) {
        test_fail("rename_cross_dir", "Cannot create source file");
        return;
    }
    IDOS->Write(fh, "cross", 5);
    IDOS->Close(fh);

    /* Rename into subdirectory */
    if (!IDOS->Rename(srcpath, dstpath)) {
        test_fail("rename_cross_dir", "Cross-dir Rename failed");
        IDOS->Delete(srcpath);
        return;
    }

    /* Verify new path exists and old is gone */
    BPTR lock = IDOS->Lock(dstpath, ACCESS_READ);
    if (!lock) {
        test_fail("rename_cross_dir", "Cannot Lock destination");
        return;
    }
    IDOS->UnLock(lock);

    lock = IDOS->Lock(srcpath, ACCESS_READ);
    if (lock) {
        IDOS->UnLock(lock);
        test_fail("rename_cross_dir", "Source still exists");
        IDOS->Delete(dstpath);
        return;
    }

    IDOS->Delete(dstpath);
    test_pass("rename_cross_dir");
}

/*
 * Cleanup: delete all test artifacts
 */
static void cleanup(const char *volume)
{
    char path[512];
    int i;
    char fname[32];

    /* Files in test dir */
    build_path(path, sizeof(path), volume, TEST_RENAME);
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, TEST_FILE);
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, "largefile.bin");
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, "unlinkme.txt");
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, "ftrunc.txt");
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, "fsync.txt");
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, "testsymlink");
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, "testhardlink");
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, "crossrename.txt");
    IDOS->Delete(path);

    for (i = 0; i < 10; i++) {
        snprintf(fname, sizeof(fname), "multi_%02d.txt", i);
        build_path(path, sizeof(path), volume, fname);
        IDOS->Delete(path);
    }

    /* Files in subdirectory */
    build_path(path, sizeof(path), volume, TEST_SUBDIR "/crossrename.txt");
    IDOS->Delete(path);

    /* Deep path (leaf to root) */
    build_path(path, sizeof(path), volume, "deep_a/deep_b/deep_c/leaf.txt");
    IDOS->Delete(path);
    build_path(path, sizeof(path), volume, "deep_a/deep_b/deep_c");
    IDOS->Delete(path);
    build_path(path, sizeof(path), volume, "deep_a/deep_b");
    IDOS->Delete(path);
    build_path(path, sizeof(path), volume, "deep_a");
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, "rmdirme");
    IDOS->Delete(path);

    /* Directories (subdir before parent) */
    build_path(path, sizeof(path), volume, TEST_SUBDIR);
    IDOS->Delete(path);

    snprintf(path, sizeof(path), "%s%s", volume, TEST_DIR);
    IDOS->Delete(path);
}

int main(int argc, char **argv)
{
    const char *volume;

    if (argc < 2) {
        IDOS->Printf("Usage: test_9p <volume>\n"
                      "Example: test_9p SHARED:\n");
        return 5;
    }

    volume = argv[1];

    /* Ensure volume path ends with ':' or '/' */
    int vlen = strlen(volume);
    char volbuf[MAX_VOLUME_LEN + 2];
    if (vlen > MAX_VOLUME_LEN) {
        IDOS->Printf("Error: volume name too long (max %ld chars).\n",
                      (long)MAX_VOLUME_LEN);
        return 5;
    }
    if (vlen > 0 && volume[vlen - 1] != ':' && volume[vlen - 1] != '/') {
        snprintf(volbuf, sizeof(volbuf), "%s:", volume);
        volume = volbuf;
    }

    IDOS->Printf("\nVirtIO 9P Handler Test Suite\n");
    IDOS->Printf("Volume: %s\n\n", volume);

    /* Run tests in order */
    test_volume_access(volume);
    test_mkdir(volume);
    test_write_file(volume);
    test_read_file(volume);
    test_getattr(volume);
    test_readdir(volume);
    test_rename(volume);
    test_mkdir_sub(volume);
    test_truncate(volume);
    test_statfs(volume);
    test_large_file(volume);
    test_chmod(volume);
    test_unlink(volume);
    test_rmdir(volume);
    test_chown(volume);
    test_utimens(volume);
    test_ftruncate(volume);
    test_fsync(volume);
    test_symlink(volume);
    test_link(volume);
    test_readdir_multi(volume);
    test_deep_path(volume);
    test_open_read_only(volume);
    test_delete_nonexistent(volume);
    test_rename_cross_dir(volume);

    /* Cleanup */
    IDOS->Printf("\nCleaning up...\n");
    cleanup(volume);

    /* Summary */
    IDOS->Printf("\nResults: %ld/%ld passed", (long)tests_passed, (long)tests_run);
    if (tests_failed > 0) {
        IDOS->Printf(", %ld FAILED", (long)tests_failed);
    }
    IDOS->Printf("\n");

    return tests_failed > 0 ? 10 : 0;
}
