// SSE/SSE2 intrinsics for the PC port's HAL, via SIMDe (MIT). The ARM11 has no
// NEON, so these become plain scalar C: correct, just slower. The heaviest user
// (gx.cpp's software rasterizer) is replaced by the citro3d renderer anyway.
#ifndef PORT3DS_COMPAT_EMMINTRIN_H
#define PORT3DS_COMPAT_EMMINTRIN_H
#ifndef SIMDE_ENABLE_NATIVE_ALIASES
#define SIMDE_ENABLE_NATIVE_ALIASES
#endif
#include <simde/x86/sse2.h>
#endif
