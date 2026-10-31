/* MSVC <intrin.h> on the New 3DS: the SSE part via SIMDe (see emmintrin.h)
 * plus the few non-SIMD MSVC intrinsics the HAL uses. */
#ifndef PORT3DS_COMPAT_INTRIN_H
#define PORT3DS_COMPAT_INTRIN_H
#include "emmintrin.h"

// Not <3ds/svc.h>: libctru defines u32/s32 as uint32_t/int32_t (unsigned
// long/long here) and the decomp uses unsigned int/int, and one file can't
// include both. libctru's own definition satisfies this declaration.
#ifdef __cplusplus
extern "C"
#endif
unsigned long long svcGetSystemTick(void);

#define __debugbreak() __builtin_trap()
/* x86 timestamp counter -> the ARM11 system tick (268 MHz). Only ever
   differenced by the HAL, for timing reports. */
static inline unsigned long long __rdtsc(void) { return svcGetSystemTick(); }
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _mm_pause() ((void)0)
/* compat/win32_compat.cpp, as in windows.h (a HAL file declares it by hand) */
#ifdef __cplusplus
extern "C"
#endif
void *_AddressOfReturnAddress(void);

static inline unsigned char _BitScanForward(unsigned long *index, unsigned long mask)
{
    if (!mask) return 0;
    *index = (unsigned long)__builtin_ctzl(mask);
    return 1;
}
static inline unsigned char _BitScanReverse(unsigned long *index, unsigned long mask)
{
    if (!mask) return 0;
    *index = 31ul - (unsigned long)__builtin_clzl(mask);
    return 1;
}
#endif
