/*
 * test/exec/types.h -- minimal native shim for <exec/types.h>.
 *
 * Used only when building a host-native unit test that pulls in
 * Amiga code (p9_marshal.c).  The real SDK header isn't available
 * outside the cross-compiler image; this provides just the typedefs
 * the bounded-marshal helpers need.
 */
#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H

#include <stdint.h>

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;

#endif
