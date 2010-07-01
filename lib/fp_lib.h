// This file is a configuration header for soft-float routines in compiler-rt.
// This file does not provide any part of the compiler-rt interface.

// Assumes that float and double correspond to the IEEE-754 binary32 and
// binary64 types, respectively.

#ifndef FP_LIB_HEADER
#define FP_LIB_HEADER

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#if defined SINGLE_PRECISION
#if 0
#pragma mark single definitions
#endif

typedef uint32_t rep_t;
typedef int32_t srep_t;
typedef float fp_t;
#define REP_C UINT32_C
#define significandBits 23

static inline int rep_clz(rep_t a) {
    return __builtin_clz(a);
}

#elif defined DOUBLE_PRECISION
#if 0
#pragma mark double definitions
#endif

typedef uint64_t rep_t;
typedef int64_t srep_t;
typedef double fp_t;
#define REP_C UINT64_C
#define significandBits 52

static inline int rep_clz(rep_t a) {
#if defined __LP64__
    return __builtin_clzl(a);
#else
    if (a & REP_C(0xffffffff00000000))
        return 32 + __builtin_clz(a >> 32);
    else 
        return __builtin_clz(a & REP_C(0xffffffff));
#endif
}

#else
#error Either SINGLE_PRECISION or DOUBLE_PRECISION must be defined.
#endif

#if 0
#pragma mark -
#pragma mark integer constants
#endif

#define typeWidth       (sizeof(rep_t)*CHAR_BIT)
#define exponentBits    (typeWidth - significandBits - 1)
#define maxExponent     ((1 << exponentBits) - 1)
#define exponentBias    (maxExponent >> 1)

#if 0
#pragma mark -
#pragma mark rep_t constants
#endif

#define implicitBit     (REP_C(1) << significandBits)
#define significandMask (implicitBit - 1U)
#define signBit         (REP_C(1) << (significandBits + exponentBits))
#define absMask         (signBit - 1U)
#define exponentMask    (absMask ^ significandMask)
#define oneRep          ((rep_t)exponentBias << significandBits)
#define infRep          exponentMask
#define quietBit        (implicitBit >> 1)
#define qnanRep         (exponentMask | quietBit)

#if 0
#pragma mark -
#pragma mark generic functions
#endif

static inline rep_t toRep(fp_t x) {
    const union { fp_t f; rep_t i; } rep = {.f = x};
    return rep.i;
}

static inline fp_t fromRep(rep_t x) {
    const union { fp_t f; rep_t i; } rep = {.i = x};
    return rep.f;
}

static inline int normalize(rep_t *significand) {
    const int shift = rep_clz(*significand) - rep_clz(implicitBit);
    *significand <<= shift;
    return 1 - shift;
}

static inline void wideLeftShift(rep_t *hi, rep_t *lo, int count) {
    *hi = *hi << count | *lo >> (typeWidth - count);
    *lo = *lo << count;
}

static inline void wideRightShiftWithSticky(rep_t *hi, rep_t *lo, int count) {
    if (count < typeWidth) {
        const bool sticky = *lo << (typeWidth - count);
        *lo = *hi << (typeWidth - count) | *lo >> count | sticky;
        *hi = *hi >> count;
    }
    else if (count < 2*typeWidth) {
        const bool sticky = *hi << (2*typeWidth - count) | *lo;
        *lo = *hi >> (count - typeWidth) | sticky;
        *hi = 0;
    } else {
        const bool sticky = *hi | *lo;
        *lo = sticky;
        *hi = 0;
    }
}

#endif // FP_LIB_HEADER
