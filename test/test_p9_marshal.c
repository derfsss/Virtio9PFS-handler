/*
 * test_p9_marshal.c -- host-native unit tests for p9_marshal.c bounds.
 *
 * Builds against test/exec/types.h (typedef shim) and pulls in the
 * real src/p9_marshal.c as a single translation unit.  Bypasses the
 * inline string_utils helpers (we only need memcpy, which p9_marshal
 * uses indirectly via the macro alias from string_utils.h).
 *
 * Build: gcc -I test -I include test/test_p9_marshal.c \
 *            -o build/test_p9_marshal_native
 *
 * Returns 0 if all assertions pass, non-zero on first failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provide the v9p_* aliases that string_utils.h would define on Amiga
 * builds.  The real string_utils.h includes <exec/types.h> + a bunch
 * of inline functions we don't need here.  Stub it. */
#define STRING_UTILS_H
#define memcpy memcpy
#define memset memset
#define strlen strlen
/* p9_marshal.c only uses memcpy from string_utils.h. */

#include "../src/p9_marshal.c"

/* ---- assertions --------------------------------------------------- */
static int g_failures = 0;
static int g_total    = 0;

#define EXPECT(cond, msg) do {                                      \
    g_total++;                                                      \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        g_failures++;                                               \
    }                                                               \
} while (0)

/* ---- p9_get_str bounds tests ------------------------------------- */
static void test_p9_get_str_normal(void)
{
    /* layout: len[2]=5 "hello" */
    uint8 buf[16] = { 5, 0, 'h','e','l','l','o' };
    uint32 buf_len = 7;
    uint32 off = 0;
    char out[32] = { 0 };
    p9_get_str(buf, buf_len, &off, out, sizeof(out));
    EXPECT(strcmp(out, "hello") == 0, "normal: out should be 'hello'");
    EXPECT(off == 7, "normal: off should advance to 7");
}

static void test_p9_get_str_truncated_header(void)
{
    /* buf_len=1 means we can't even read the 2-byte length prefix */
    uint8 buf[2] = { 5, 0 };
    uint32 buf_len = 1;
    uint32 off = 0;
    char out[16];
    out[0] = 'X';
    p9_get_str(buf, buf_len, &off, out, sizeof(out));
    EXPECT(out[0] == '\0', "truncated header: out should be empty string");
}

static void test_p9_get_str_oversized_len(void)
{
    /* len=99 but only 7 bytes total in buf -- must NOT walk past */
    uint8 buf[16] = { 99, 0, 'h','e','l','l','o' };
    uint32 buf_len = 7;
    uint32 off = 0;
    char out[16];
    out[0] = 'X';
    p9_get_str(buf, buf_len, &off, out, sizeof(out));
    EXPECT(out[0] == '\0', "oversized len: out should be empty string");
}

static void test_p9_get_str_empty(void)
{
    uint8 buf[4] = { 0, 0 };
    uint32 buf_len = 2;
    uint32 off = 0;
    char out[16];
    out[0] = 'X';
    p9_get_str(buf, buf_len, &off, out, sizeof(out));
    EXPECT(out[0] == '\0', "empty len=0: out should be empty string");
    EXPECT(off == 2, "empty: off should advance past the 2-byte length");
}

static void test_p9_get_str_truncates_to_max(void)
{
    /* len=10 'abcdefghij' but caller's max is only 5 -> truncate to 4+NUL */
    uint8 buf[16] = { 10, 0, 'a','b','c','d','e','f','g','h','i','j' };
    uint32 buf_len = 12;
    uint32 off = 0;
    char out[5] = { 0 };
    p9_get_str(buf, buf_len, &off, out, sizeof(out));
    EXPECT(strcmp(out, "abcd") == 0,
           "max=5 buffer should give first 4 chars + NUL");
    EXPECT(off == 12, "off should still advance the full length");
}

static void test_p9_get_str_offset_at_end(void)
{
    /* off already at buf_len -- can't even start */
    uint8 buf[8] = { 5, 0, 'a','b','c','d','e' };
    uint32 buf_len = 7;
    uint32 off = 7;
    char out[16];
    out[0] = 'X';
    p9_get_str(buf, buf_len, &off, out, sizeof(out));
    EXPECT(out[0] == '\0', "off at end: out should be empty");
}

/* ---- main --------------------------------------------------------- */
int main(void)
{
    test_p9_get_str_normal();
    test_p9_get_str_truncated_header();
    test_p9_get_str_oversized_len();
    test_p9_get_str_empty();
    test_p9_get_str_truncates_to_max();
    test_p9_get_str_offset_at_end();

    fprintf(stderr, "%d/%d native marshal tests passed\n",
            g_total - g_failures, g_total);
    return g_failures ? 1 : 0;
}
