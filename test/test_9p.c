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

/* Build a path inside the test directory */
static void build_path(char *out, int max, const char *volume, const char *rel)
{
    snprintf(out, max, "%s%s/%s", volume, TEST_DIR, rel);
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
    char path[256];
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
    char path[256];
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
    char path[256];
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
    char path[256];
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
    char path[256];
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
    char path[256];
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
    char path[256];
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
    char path[256];
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
    char path[256];
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
 * Cleanup: delete all test artifacts
 */
static void cleanup(const char *volume)
{
    char path[256];

    build_path(path, sizeof(path), volume, TEST_RENAME);
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, TEST_FILE);
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, TEST_SUBDIR);
    IDOS->Delete(path);

    build_path(path, sizeof(path), volume, "largefile.bin");
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
    char volbuf[256];
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
