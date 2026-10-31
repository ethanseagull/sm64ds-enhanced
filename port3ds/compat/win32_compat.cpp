// The Win32 calls the PC port's HAL makes, over libctru. See
// compat/include/windows.h for the policy.
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>

#include "windows.h"

extern "C" {

// ---- fibers --------------------------------------------------------------
void host3ds_fiber_switch(uint32_t *from, const uint32_t *to);
void host3ds_fiber_entry(void);

namespace {
struct Fiber {
    uint32_t ctx[26];   // r4-r11, lr, sp, d8-d15 (see fiber_switch.S)
    void *stack;
    LPVOID param;
};
Fiber *g_current;
Fiber g_thread_fiber;   // the converted thread's own context
}  // namespace

LPVOID ConvertThreadToFiber(LPVOID param)
{
    if (!g_current) {
        g_thread_fiber.param = param;
        g_current = &g_thread_fiber;
    }
    return g_current;
}

BOOL ConvertFiberToThread(void)
{
    g_current = nullptr;
    return TRUE;
}

LPVOID CreateFiber(size_t stack_size, LPFIBER_START_ROUTINE start, LPVOID param)
{
    Fiber *f = (Fiber *)calloc(1, sizeof(Fiber));
    if (!f) return nullptr;
    if (stack_size < 64 * 1024) stack_size = 64 * 1024;
    f->stack = memalign(16, stack_size);
    if (!f->stack) { free(f); return nullptr; }
    f->param = param;
    f->ctx[0] = (uint32_t)start;                               // r4
    f->ctx[1] = (uint32_t)param;                               // r5
    f->ctx[8] = (uint32_t)&host3ds_fiber_entry;                // lr
    f->ctx[9] = ((uint32_t)f->stack + stack_size) & ~7u;       // sp, 8-aligned
    return f;
}

void SwitchToFiber(LPVOID fiber)
{
    Fiber *to = (Fiber *)fiber;
    Fiber *from = g_current;
    if (!from || to == from) return;
    g_current = to;
    host3ds_fiber_switch(from->ctx, to->ctx);
}

void DeleteFiber(LPVOID fiber)
{
    Fiber *f = (Fiber *)fiber;
    if (!f || f == &g_thread_fiber || f == g_current) return;
    free(f->stack);
    free(f);
}

LPVOID GetCurrentFiber(void)
{
    return g_current;
}

BOOL QueryPerformanceCounter(LARGE_INTEGER *count)
{
    count->QuadPart = (long long)svcGetSystemTick();
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *freq)
{
    freq->QuadPart = (long long)SYSCLOCK_ARM11;
    return TRUE;
}

static void fill_time(SYSTEMTIME *st, bool local)
{
    (void)local;   // the 3DS clock is the console's local time
    const time_t t = time(nullptr);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    st->wYear = (WORD)(tmv.tm_year + 1900);
    st->wMonth = (WORD)(tmv.tm_mon + 1);
    st->wDayOfWeek = (WORD)tmv.tm_wday;
    st->wDay = (WORD)tmv.tm_mday;
    st->wHour = (WORD)tmv.tm_hour;
    st->wMinute = (WORD)tmv.tm_min;
    st->wSecond = (WORD)tmv.tm_sec;
    st->wMilliseconds = (WORD)(osGetTime() % 1000);
}
void GetLocalTime(SYSTEMTIME *st) { fill_time(st, true); }
void GetSystemTime(SYSTEMTIME *st) { fill_time(st, false); }

// ---- files ---------------------------------------------------------------
namespace {
struct FindState {
    DIR *dir;
    char suffix[64];     // "" for "*", ".ext" for "*.ext"
    char dirpath[256];
};
bool find_next(FindState *s, WIN32_FIND_DATAA *fd)
{
    while (struct dirent *e = readdir(s->dir)) {
        const char *n = e->d_name;
        const size_t ln = strlen(n), ls = strlen(s->suffix);
        if (ls && (ln < ls || strcasecmp(n + ln - ls, s->suffix) != 0)) continue;
        memset(fd, 0, sizeof *fd);
        strncpy(fd->cFileName, n, sizeof fd->cFileName - 1);
        char full[600];
        snprintf(full, sizeof full, "%s/%s", s->dirpath, n);
        struct stat stv;
        if (stat(full, &stv) == 0) {
            fd->dwFileAttributes = S_ISDIR(stv.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            fd->nFileSizeLow = (DWORD)stv.st_size;
        }
        return true;
    }
    return false;
}
}  // namespace

HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd)
{
    FindState *s = (FindState *)calloc(1, sizeof(FindState));
    if (!s) return INVALID_HANDLE_VALUE;
    strncpy(s->dirpath, pattern, sizeof s->dirpath - 1);
    for (char *p = s->dirpath; *p; ++p) if (*p == '\\') *p = '/';
    char *slash = strrchr(s->dirpath, '/');
    const char *mask = slash ? slash + 1 : s->dirpath;
    const char *star = strchr(mask, '*');
    if (star) strncpy(s->suffix, star + 1, sizeof s->suffix - 1);
    if (slash) *slash = 0; else strcpy(s->dirpath, ".");
    s->dir = opendir(s->dirpath);
    if (!s->dir || !find_next(s, fd)) {
        if (s->dir) closedir(s->dir);
        free(s);
        return INVALID_HANDLE_VALUE;
    }
    return s;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd)
{
    return (h && h != INVALID_HANDLE_VALUE) ? find_next((FindState *)h, fd) : FALSE;
}
BOOL FindClose(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    closedir(((FindState *)h)->dir);
    free(h);
    return TRUE;
}
DWORD GetFileAttributesA(LPCSTR path)
{
    struct stat stv;
    if (stat(path, &stv) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(stv.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD size)
{
    // There's no exe folder on 3DS, so use the usual homebrew location and
    // "<exe folder>/save" and the like end up under sdmc:/3ds/.
    const char *p = "sdmc:/3ds/sm64ds3ds.3dsx";
    const DWORD n = (DWORD)strlen(p);
    if (!buf || size == 0) return 0;
    const DWORD c = n < size - 1 ? n : size - 1;
    memcpy(buf, p, c);
    buf[c] = 0;
    return c;
}
BOOL DeleteFileA(LPCSTR path) { return remove(path) == 0; }
DWORD GetFullPathNameA(LPCSTR path, DWORD size, LPSTR buf, LPSTR *file_part)
{
    // Paths on the SD card are already absolute ("sdmc:/..."); a relative one
    // is relative to the current directory, which the HAL does not rely on.
    const DWORD n = (DWORD)strlen(path);
    if (!buf || size <= n) return n + 1;
    memcpy(buf, path, n + 1);
    if (file_part) {
        char *s = strrchr(buf, '/');
        *file_part = s ? s + 1 : buf;
    }
    return n;
}
// Where the caller's return address is stored, which the PC HAL only reads in
// crash diagnostics. On ARM it's in lr; the closest stack slot GCC exposes is
// the caller's frame address.
void *_AddressOfReturnAddress(void) { return __builtin_frame_address(1); }
// MSVC's _ReturnAddress(), which some rewritten src/ files call as a function
// (only to log it). As a real call it returns an address inside the wrapper
// instead of its caller, which is good enough for a log. Parenthesized because
// msvc_compat.h makes the name a macro in PC port code.
extern "C" __attribute__((noinline)) void *(_ReturnAddress)(void)
{ return __builtin_return_address(0); }
HMODULE LoadLibraryA(LPCSTR) { return nullptr; }
FARPROC GetProcAddress(HMODULE, LPCSTR) { return nullptr; }
BOOL FreeLibrary(HMODULE) { return FALSE; }
BOOL MoveFileA(LPCSTR from, LPCSTR to) { return rename(from, to) == 0; }
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists)
{
    struct stat stv;
    if (fail_if_exists && stat(to, &stv) == 0) return FALSE;
    FILE *in = fopen(from, "rb");
    if (!in) return FALSE;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return FALSE; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return TRUE;
}

// ---- virtual memory: not available ----------------------------------------
LPVOID VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD) { return nullptr; }
BOOL VirtualFree(LPVOID, SIZE_T, DWORD) { return FALSE; }
BOOL VirtualProtect(LPVOID, SIZE_T, DWORD, DWORD *old) { if (old) *old = PAGE_READWRITE; return FALSE; }
SIZE_T VirtualQuery(const void *, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len)
{
    if (mbi && len >= sizeof *mbi) memset(mbi, 0, sizeof *mbi);
    return 0;
}
unsigned GetWriteWatch(DWORD, PVOID, SIZE_T, PVOID *, ULONG_PTR *count, DWORD *gran)
{
    if (count) *count = 0;
    if (gran) *gran = 0x1000;
    return 1;   // failure
}
unsigned ResetWriteWatch(LPVOID, SIZE_T) { return 1; }
void RaiseException(DWORD code, DWORD, DWORD, const ULONG_PTR *)
{
    fprintf(stderr, "RaiseException(%08lx): aborting\n", (unsigned long)code);
    abort();
}

// ---- windows and the mouse: none ------------------------------------------
BOOL GetCursorPos(POINT *p) { if (p) p->x = p->y = 0; return FALSE; }
BOOL ScreenToClient(HWND, POINT *) { return FALSE; }
BOOL ClientToScreen(HWND, POINT *) { return FALSE; }
BOOL GetClientRect(HWND, RECT *r) { if (r) r->left = r->top = r->right = r->bottom = 0; return FALSE; }
short GetAsyncKeyState(int) { return 0; }
short GetKeyState(int) { return 0; }
HWND GetForegroundWindow(void) { return nullptr; }

DWORD GetTickCount(void)
{
    return (DWORD)osGetTime();
}

void Sleep(DWORD ms)
{
    svcSleepThread((s64)ms * 1000000);
}

BOOL CreateDirectoryA(LPCSTR path, void *)
{
    return mkdir(path, 0777) == 0;
}

LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER)
{
    return nullptr;
}

// No vectored handlers, no thread contexts: the 3DS reports crashes from its
// own CPU exception handler. Callers see "not installed" / "failed".
PVOID AddVectoredExceptionHandler(DWORD, PVECTORED_EXCEPTION_HANDLER) { return nullptr; }
DWORD RemoveVectoredExceptionHandler(PVOID) { return 0; }
HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
DWORD GetCurrentThreadId(void) { return 1; }
DWORD GetCurrentProcessId(void) { return 1; }
BOOL GetThreadContext(HANDLE, CONTEXT *) { return FALSE; }
BOOL SetThreadContext(HANDLE, const CONTEXT *) { return FALSE; }

// "Is this pointer readable?" The PC HAL asks before dereferencing pointers it
// found in game memory for diagnostics. Answer from the 3DS address map: the
// image, the DS address aliases, the heap and the linear heap. Anything else
// is bad.
static bool in_mapped(uintptr_t a)
{
    return (a >= 0x00100000u && a < 0x04000000u) ||   // image + main RAM alias
           (a >= 0x04000000u && a < 0x07001000u) ||   // DS I/O, palettes, VRAM, OAM aliases
           (a >= 0x08000000u && a < 0x10000000u) ||   // heap
           (a >= 0x14000000u && a < 0x1C000000u) ||   // linear heap (old map)
           (a >= 0x30000000u && a < 0x38000000u);     // linear heap (new map)
}
BOOL IsBadReadPtr(const void *p, uintptr_t n)
{
    const uintptr_t a = (uintptr_t)p;
    return !(p && in_mapped(a) && (n == 0 || in_mapped(a + n - 1)));
}
BOOL IsBadWritePtr(void *p, uintptr_t n) { return IsBadReadPtr(p, n); }

DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size)
{
    const char *v = getenv(name);
    if (!v) return 0;
    const DWORD n = (DWORD)strlen(v);
    if (buf && size > n) { memcpy(buf, v, n + 1); return n; }
    return n + 1;   // the size needed, as Win32 reports it
}
BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}
DWORD GetLastError(void) { return 0; }

void OutputDebugStringA(LPCSTR s)
{
    fputs(s, stderr);
}

HMODULE GetModuleHandleA(LPCSTR)
{
    return (HMODULE)0x00100000;   // where a 3dsx's .text is loaded
}

WORD CaptureStackBackTrace(DWORD, DWORD, PVOID *, DWORD *hash)
{
    if (hash) *hash = 0;
    return 0;   // no unwinder on this target; callers print what they have
}

}  // extern "C"
