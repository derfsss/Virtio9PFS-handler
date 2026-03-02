/*
 * string_utils.h — Inline string/memory helpers (no newlib dependency)
 *
 * These replace C stdlib functions so the handler can be built with
 * -nostartfiles and no newlib.  All are simple byte loops — safe for
 * DMA (MEMF_SHARED) buffers where ClearMem/SetMem would be unsafe.
 */

#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <exec/types.h>

static inline void *v9p_memset(void *dst, int c, uint32 n)
{
    uint8 *d = (uint8 *)dst;
    uint8 val = (uint8)c;

    /* Byte-align first */
    while (n && ((uint32)d & 3)) {
        *d++ = val;
        n--;
    }

    /* Word-fill the bulk (4 bytes per iteration) */
    if (n >= 4) {
        uint32 w = val | ((uint32)val << 8) | ((uint32)val << 16) | ((uint32)val << 24);
        uint32 *dw = (uint32 *)d;
        while (n >= 4) {
            *dw++ = w;
            n -= 4;
        }
        d = (uint8 *)dw;
    }

    /* Remaining tail bytes */
    while (n--)
        *d++ = val;

    return dst;
}

static inline void *v9p_memcpy(void *dst, const void *src, uint32 n)
{
    uint8 *d = (uint8 *)dst;
    const uint8 *s = (const uint8 *)src;

    /* If both pointers share the same alignment, use word copies */
    if (n >= 16 && (((uint32)d ^ (uint32)s) & 3) == 0) {
        /* Byte-align first */
        while (n && ((uint32)d & 3)) {
            *d++ = *s++;
            n--;
        }

        /* Word-copy the bulk (4 bytes per iteration) */
        uint32 *dw = (uint32 *)d;
        const uint32 *sw = (const uint32 *)s;
        while (n >= 4) {
            *dw++ = *sw++;
            n -= 4;
        }
        d = (uint8 *)dw;
        s = (const uint8 *)sw;
    }

    /* Remaining bytes (or misaligned case) */
    while (n--)
        *d++ = *s++;

    return dst;
}

static inline void *v9p_memmove(void *dst, const void *src, uint32 n)
{
    uint8 *d = (uint8 *)dst;
    const uint8 *s = (const uint8 *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

static inline uint32 v9p_strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (uint32)(p - s);
}

static inline char *v9p_strncpy(char *dst, const char *src, uint32 n)
{
    char *d = dst;
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    while (n--)
        *d++ = '\0';
    return dst;
}

static inline int v9p_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8)*a - (int)(uint8)*b;
}

static inline char *v9p_strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)0;
}

static inline char *v9p_strrchr(const char *s, int c)
{
    const char *last = (char *)0;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)last;
}

/* Drop-in macros so existing code compiles without changes */
#define memset  v9p_memset
#define memcpy  v9p_memcpy
#define memmove v9p_memmove
#define strlen  v9p_strlen
#define strncpy v9p_strncpy
#define strcmp   v9p_strcmp
#define strchr  v9p_strchr
#define strrchr v9p_strrchr

#endif /* STRING_UTILS_H */
