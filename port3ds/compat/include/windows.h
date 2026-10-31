// A minimal <windows.h> for building the PC port's HAL on 3DS.
//
// Only what port/hal and port/ntr actually call, implemented on libctru in
// win32_compat.cpp. Add to it when the link reports a missing Win32 name,
// rather than patching the HAL, so the upstream code stays close to the PC
// port.
//
// The MSVC keywords (__declspec, __stdcall, __cdecl, __fastcall, __thiscall)
// are defined away on the command line in CMakeLists.txt, because some HAL
// files declare Win32 functions by hand instead of including this.
#ifndef PORT3DS_COMPAT_WINDOWS_H
#define PORT3DS_COMPAT_WINDOWS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int BOOL;
typedef short SHORT;
typedef unsigned short USHORT;
typedef unsigned int UINT;
typedef unsigned long ULONG;
typedef unsigned char UCHAR;
typedef long long INT64;
typedef unsigned long long UINT64;
typedef void *FARPROC;
typedef void *HINSTANCE;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef void *HANDLE;
typedef void *HMODULE;
typedef void *PVOID;
typedef void *LPVOID;
typedef DWORD *LPDWORD;
#define MAKEWORD(lo, hi) ((WORD)(((BYTE)(lo)) | (((WORD)(BYTE)(hi)) << 8)))
typedef const char *LPCSTR;
typedef char *LPSTR;
typedef long long LONGLONG;
typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER;

#define TRUE 1
#define FALSE 0
#define WINAPI
#define CALLBACK
#define INFINITE 0xFFFFFFFFul

// Timing uses the 3DS system tick (268,111,856 Hz). Win32 takes a
// LARGE_INTEGER*, some HAL files declare these with long long*; both are the
// same symbol and the same 8 bytes.
BOOL QueryPerformanceCounter(LARGE_INTEGER *count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *freq);
typedef struct _SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute,
    wSecond, wMilliseconds; } SYSTEMTIME;
void GetLocalTime(SYSTEMTIME *st);
void GetSystemTime(SYSTEMTIME *st);
DWORD GetTickCount(void);
void Sleep(DWORD ms);

/* Files. */
BOOL CreateDirectoryA(LPCSTR path, void *security);
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFul
#define FILE_ATTRIBUTE_DIRECTORY 0x10ul
#define FILE_ATTRIBUTE_NORMAL 0x80ul
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400ul
#define FILE_ATTRIBUTE_HIDDEN 0x2ul
typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
} WIN32_FIND_DATAA;
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd);   /* "dir/*" or "dir/*.ext" */
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
BOOL FindClose(HANDLE h);
DWORD GetFileAttributesA(LPCSTR path);
DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD size);  /* sdmc:/3ds/sm64ds3ds.3dsx */
DWORD GetFullPathNameA(LPCSTR path, DWORD size, LPSTR buf, LPSTR *file_part);
/* No dynamic libraries on the 3DS: every lookup fails, and callers keep
   their fallbacks (the PC HAL loads user32/d3d11/plugins this way). */
HMODULE LoadLibraryA(LPCSTR name);
FARPROC GetProcAddress(HMODULE m, LPCSTR name);
BOOL FreeLibrary(HMODULE m);
BOOL DeleteFileA(LPCSTR path);
BOOL MoveFileA(LPCSTR from, LPCSTR to);
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);

// Virtual memory. The PC HAL reserves fixed ranges and asks which pages a
// frame wrote (for rollback). The 3DS can't do either, so VirtualAlloc fails
// and those features stay off.
#define MEM_COMMIT 0x1000ul
#define MEM_RESERVE 0x2000ul
#define MEM_RELEASE 0x8000ul
#define MEM_DECOMMIT 0x4000ul
#define MEM_WRITE_WATCH 0x200000ul
#define MEM_FREE 0x10000ul
#define PAGE_NOACCESS 0x01ul
#define PAGE_READONLY 0x02ul
#define PAGE_READWRITE 0x04ul
#define PAGE_EXECUTE 0x10ul
#define PAGE_EXECUTE_READ 0x20ul
#define PAGE_EXECUTE_READWRITE 0x40ul
#define PAGE_EXECUTE_WRITECOPY 0x80ul
#define WRITE_WATCH_FLAG_RESET 1ul
typedef unsigned long ULONG_PTR, SIZE_T;
typedef struct _MEMORY_BASIC_INFORMATION { PVOID BaseAddress, AllocationBase;
    DWORD AllocationProtect; SIZE_T RegionSize; DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION;
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, DWORD *old);
SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len);
unsigned GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addrs,
                       ULONG_PTR *count, DWORD *granularity);
unsigned ResetWriteWatch(LPVOID base, SIZE_T size);
void RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args);

// Windows and the mouse: on PC the sub screen is a window region you click
// into. The 3DS has no window and touch goes through port3ds's own input
// path. These only have to compile.
typedef void *HWND;
typedef void *HDC;
typedef struct tagPOINT { LONG x, y; } POINT;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT;
BOOL GetCursorPos(POINT *p);
BOOL ScreenToClient(HWND w, POINT *p);
BOOL ClientToScreen(HWND w, POINT *p);
BOOL GetClientRect(HWND w, RECT *r);
short GetAsyncKeyState(int vkey);
short GetKeyState(int vkey);
HWND GetForegroundWindow(void);
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_F1 0x70
void *_AddressOfReturnAddress(void);

/* PE image layout (the PC mod loader walks its DLLs); compiles, never used. */
typedef struct _IMAGE_DOS_HEADER { WORD e_magic; WORD e_res[29]; LONG e_lfanew; } IMAGE_DOS_HEADER;
typedef struct _IMAGE_FILE_HEADER { WORD Machine, NumberOfSections; DWORD TimeDateStamp,
    PointerToSymbolTable, NumberOfSymbols; WORD SizeOfOptionalHeader, Characteristics; } IMAGE_FILE_HEADER;
typedef struct _IMAGE_DATA_DIRECTORY { DWORD VirtualAddress, Size; } IMAGE_DATA_DIRECTORY;
typedef struct _IMAGE_OPTIONAL_HEADER32 { WORD Magic; BYTE pad[94]; IMAGE_DATA_DIRECTORY DataDirectory[16]; } IMAGE_OPTIONAL_HEADER32;
typedef struct _IMAGE_NT_HEADERS32 { DWORD Signature; IMAGE_FILE_HEADER FileHeader;
    IMAGE_OPTIONAL_HEADER32 OptionalHeader; } IMAGE_NT_HEADERS32;
typedef struct _IMAGE_EXPORT_DIRECTORY { DWORD Characteristics, TimeDateStamp; WORD MajorVersion, MinorVersion;
    DWORD Name, Base, NumberOfFunctions, NumberOfNames, AddressOfFunctions, AddressOfNames,
    AddressOfNameOrdinals; } IMAGE_EXPORT_DIRECTORY;
#define IMAGE_DOS_SIGNATURE 0x5A4Du
#define IMAGE_NT_SIGNATURE 0x00004550u
#define IMAGE_DIRECTORY_ENTRY_EXPORT 0

// Structured exceptions: crashes are reported by our own handler
// (crash_handler.cpp), so a filter is accepted and never called.
typedef struct _EXCEPTION_RECORD { DWORD ExceptionCode; DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord; PVOID ExceptionAddress;
    DWORD NumberParameters; uintptr_t ExceptionInformation[15]; } EXCEPTION_RECORD;
// The x86 registers as the PC HAL's crash and watchpoint code reads them.
// Never filled on 3DS; it only has to compile.
typedef struct _CONTEXT {
    DWORD ContextFlags;
    DWORD Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    DWORD Eax, Ebx, Ecx, Edx, Esi, Edi, Ebp, Esp, Eip, EFlags;
    DWORD SegCs, SegSs;
} CONTEXT, *PCONTEXT;
typedef struct _EXCEPTION_POINTERS { EXCEPTION_RECORD *ExceptionRecord;
    CONTEXT *ContextRecord; } EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS *);
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *);
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_BREAKPOINT 0x80000003u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_STACK_OVERFLOW 0xC00000FDu
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094u
#define CONTEXT_DEBUG_REGISTERS 0x00010010u
#define CONTEXT_FULL 0x00010007u
#define CONTEXT_CONTROL 0x00010001u
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f);
PVOID AddVectoredExceptionHandler(DWORD first, PVECTORED_EXCEPTION_HANDLER h);
DWORD RemoveVectoredExceptionHandler(PVOID h);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
BOOL GetThreadContext(HANDLE thread, CONTEXT *ctx);
BOOL SetThreadContext(HANDLE thread, const CONTEXT *ctx);
BOOL IsBadReadPtr(const void *p, uintptr_t n);
BOOL IsBadWritePtr(void *p, uintptr_t n);

/* Environment (the HAL also reads its knobs with getenv). */
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size);
BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value);
DWORD GetLastError(void);
#define ERROR_ALREADY_FIBER 1280L

/* Diagnostics. */
void OutputDebugStringA(LPCSTR s);
HMODULE GetModuleHandleA(LPCSTR name);   /* returns the image base, 0x00100000 */
WORD CaptureStackBackTrace(DWORD skip, DWORD n, PVOID *frames, DWORD *hash);

/* Fibers: cooperative contexts on one thread (compat/fiber_switch.S).
 * port/ntr/rt.cpp runs the game on one and the host frame loop on the other. */
typedef void (*LPFIBER_START_ROUTINE)(LPVOID param);
LPVOID ConvertThreadToFiber(LPVOID param);
BOOL ConvertFiberToThread(void);
LPVOID CreateFiber(size_t stack_size, LPFIBER_START_ROUTINE start, LPVOID param);
void SwitchToFiber(LPVOID fiber);
void DeleteFiber(LPVOID fiber);
LPVOID GetCurrentFiber(void);

/* Atomics (single address, full barrier), as MSVC's intrinsics behave. */
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG xchg, LONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

#ifdef __cplusplus
}
#endif

#endif /* PORT3DS_COMPAT_WINDOWS_H */
