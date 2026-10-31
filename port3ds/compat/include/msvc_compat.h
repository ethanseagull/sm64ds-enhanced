// Force-included into every game/HAL file in the 3DS build (see CMakeLists.txt).
// Maps the MSVC runtime's underscore names the PC HAL uses to newlib. Only
// declarations and macros, so it's harmless in the matched src/ files.
#ifndef PORT3DS_MSVC_COMPAT_H
#define PORT3DS_MSVC_COMPAT_H

#include <strings.h>

// MSVC's __pragma() carries section/segment pragmas that mean nothing to GCC;
// the 3DS build places those globals another way where layout matters.
#define __pragma(x)
#define MAX_PATH 260

/* __declspec: compat/include/msvc_declspec.h, force-included everywhere. */

/* MSVC integer and intrinsic spellings. */
#define __int64 long long
#define __int32 int
#define __int16 short
#define __int8 char
#define __debugbreak() __builtin_trap()
#define __assume(x) ((void)0)
#define __forceinline inline __attribute__((always_inline))
#define __noop(...) ((void)0)
#define _ReturnAddress() __builtin_return_address(0)
#define _resetstkoflw() 1
/* _AddressOfReturnAddress is a function in compat/win32_compat.cpp: one HAL
   file declares it by hand, which a macro would break. */
#define _aligned_malloc(size, align) memalign((align), (size))
#define _aligned_free(p) free(p)
#include <malloc.h>
#include <stdlib.h>

// Structured exception handling. The PC HAL wraps its crash diagnostics in
// __try/__except; on 3DS crashes are reported by our own exception handler
// (crash_handler.cpp), so the guarded block just runs and the handler only has
// to compile:
// __try { A } __except (filter) { B }   ->   if (1) { A } else if (0) { B }
#ifdef __cplusplus
// In C++, libstdc++ already defines __try as `try`, so write it as a try block
// whose catch never runs (nothing throws a C++ exception through these):
// __try { A } __except (filter) { B }   ->   try { A } catch (...) { B }
#define __try try
#define __except(...) catch (...)
#else
#define __try if (1)
#define __except(...) else if (0)
#endif
#define __finally if (1)
#define __leave ((void)0)
#define GetExceptionCode() 0u
#define GetExceptionInformation() ((struct _EXCEPTION_POINTERS *)0)

#define _stricmp    strcasecmp
#define _strnicmp   strncasecmp
#define _snprintf   snprintf
#define _vsnprintf  vsnprintf
#define _strdup     strdup
#define _fileno     fileno
#define _isatty     isatty
#define _access     access
#define _unlink     unlink
#define _getcwd     getcwd
#define _fseeki64   fseeko
#define _ftelli64   ftello

#endif
