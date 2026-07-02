/*
 * shared.h - minimal shim standing in for Genesis Plus GX's shared.h so the
 * vendored ym2612.c compiles standalone (host gcc and m68k-amigaos-gcc).
 * Provides only what ym2612.c actually references: fixed-width int typedefs,
 * INLINE, M_PI, and the save/load_param state macros (both sides use the same
 * incrementing semantics, so the SaveContext/LoadContext layouts stay
 * symmetric).
 */
#ifndef GPGX_SHARED_SHIM_H
#define GPGX_SHARED_SHIM_H

#include <stdint.h>
#include <string.h>
#include <math.h>

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef int8_t   INT8;
typedef int16_t  INT16;
typedef int32_t  INT32;
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;

#ifndef INLINE
#define INLINE static __inline__
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define load_param(param, size) \
  do { memcpy(param, &state[bufferptr], size); bufferptr += (int)(size); } while (0)
#define save_param(param, size) \
  do { memcpy(&state[bufferptr], param, size); bufferptr += (int)(size); } while (0)

#include "ym2612.h"

#endif /* GPGX_SHARED_SHIM_H */
