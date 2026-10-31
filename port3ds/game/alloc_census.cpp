// GAME3DS_ALLOC_CENSUS=1 builds only: who's holding the heap and linear heap.
// malloc, calloc, realloc, memalign, new/new[] and libctru's linear allocator
// are wrapped (CMakeLists.txt), and every block of kBig bytes or more is kept
// with its caller's address. alloc_census_dump prints the live ones by caller
// (look them up with addr2line). Smaller blocks only show in mallinfo's total.
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t k);
void *__real_realloc(void *p, size_t n);
void *__real_memalign(size_t a, size_t n);
void __real_free(void *p);
void *__real_linearAlloc(size_t n);
void *__real_linearMemAlign(size_t n, size_t a);
void __real_linearFree(void *p);
}

namespace {

const size_t kBig = 16 * 1024;
const unsigned kSlots = 16384;   // a power of two

struct Rec {
    void *p;
    unsigned size;
    unsigned caller;
    bool linear;
};
Rec g_rec[kSlots];
volatile int g_lock;
unsigned g_lost;

void lock()
{
    while (__atomic_test_and_set(&g_lock, __ATOMIC_ACQUIRE)) svcSleepThread(10000);
}
void unlock() { __atomic_clear(&g_lock, __ATOMIC_RELEASE); }

unsigned slot_of(void *p) { return ((unsigned)(uintptr_t)p >> 3) * 2654435761u & (kSlots - 1); }

void add(void *p, size_t n, void *caller, bool linear)
{
    if (!p || n < kBig) return;
    lock();
    unsigned i = slot_of(p);
    for (unsigned k = 0; k < kSlots; ++k, i = (i + 1) & (kSlots - 1)) {
        if (!g_rec[i].p || g_rec[i].p == (void *)1) {
            g_rec[i] = Rec{p, (unsigned)n, (unsigned)(uintptr_t)caller, linear};
            unlock();
            return;
        }
    }
    ++g_lost;
    unlock();
}

void drop(void *p)
{
    if (!p) return;
    lock();
    unsigned i = slot_of(p);
    for (unsigned k = 0; k < kSlots && g_rec[i].p; ++k, i = (i + 1) & (kSlots - 1)) {
        if (g_rec[i].p == p) {
            g_rec[i].p = (void *)1;   // a tombstone: the probe goes on past it
            break;
        }
    }
    unlock();
}

}  // namespace

extern "C" {

__attribute__((noinline)) void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);
    add(p, n, __builtin_return_address(0), false);
    return p;
}

__attribute__((noinline)) void *__wrap_calloc(size_t n, size_t k)
{
    void *p = __real_calloc(n, k);
    add(p, n * k, __builtin_return_address(0), false);
    return p;
}

__attribute__((noinline)) void *__wrap_realloc(void *old, size_t n)
{
    drop(old);
    void *p = __real_realloc(old, n);
    add(p, n, __builtin_return_address(0), false);
    return p;
}

__attribute__((noinline)) void *__wrap_memalign(size_t a, size_t n)
{
    void *p = __real_memalign(a, n);
    add(p, n, __builtin_return_address(0), false);
    return p;
}

void __wrap_free(void *p)
{
    drop(p);
    __real_free(p);
}

// operator new / new[]: straight to malloc, so the caller is new's
__attribute__((noinline)) void *__wrap__Znwj(size_t n)
{
    void *p = __real_malloc(n ? n : 1);
    if (!p) abort();
    add(p, n, __builtin_return_address(0), false);
    return p;
}

__attribute__((noinline)) void *__wrap__Znaj(size_t n)
{
    void *p = __real_malloc(n ? n : 1);
    if (!p) abort();
    add(p, n, __builtin_return_address(0), false);
    return p;
}

__attribute__((noinline)) void *__wrap_linearAlloc(size_t n)
{
    void *p = __real_linearAlloc(n);
    add(p, n, __builtin_return_address(0), true);
    return p;
}

__attribute__((noinline)) void *__wrap_linearMemAlign(size_t n, size_t a)
{
    void *p = __real_linearMemAlign(n, a);
    add(p, n, __builtin_return_address(0), true);
    return p;
}

void __wrap_linearFree(void *p)
{
    drop(p);
    __real_linearFree(p);
}

void *port_arena_base(void);
void *port_arena_end(void);
unsigned _ZN22ExpandingHeapAllocator10MemoryLeftEv(void *alloc);
extern void *data_020a0e9c;   // the ROM's root heap

static bool page_used(const char *p)
{
    const unsigned *w = (const unsigned *)p;
    for (unsigned i = 0; i < 0x1000 / 4; ++i)
        if (w[i]) return true;
    return false;
}

// "used KB of total KB", then each `band` bytes as a hex digit: its pages
// written, in sixteenths (0: none, f: all or nearly)
static void census_pages(const char *name, const char *lo, const char *hi, unsigned band)
{
    if (!lo || hi <= lo) return;
    char map[80];
    unsigned used = 0, n = 0;
    for (const char *b = lo; b < hi && n + 1 < sizeof map; b += band) {
        unsigned in = 0, pages = 0;
        for (const char *p = b; p < b + band && p < hi; p += 0x1000, ++pages)
            if (page_used(p)) ++in;
        used += in;
        map[n++] = "0123456789abcdef"[pages ? (in * 15 + pages - 1) / pages : 0];
    }
    map[n] = 0;
    printf("[census] %s %u KB of %u KB written: %s (%u KB a digit)\n", name, used * 4,
           (unsigned)((hi - lo) / 1024), map, band / 1024);
}

void alloc_census_dump(int frame)
{
    struct Row {
        unsigned caller, bytes, n;
        bool linear;
    };
    static Row rows[1024];
    unsigned nrows = 0, heap_big = 0, lin_big = 0;
    lock();
    for (unsigned i = 0; i < kSlots; ++i) {
        const Rec &r = g_rec[i];
        if (!r.p || r.p == (void *)1) continue;
        (r.linear ? lin_big : heap_big) += r.size;
        unsigned j = 0;
        while (j < nrows && (rows[j].caller != r.caller || rows[j].linear != r.linear)) ++j;
        if (j == nrows) {
            if (nrows == 1024) continue;
            rows[nrows++] = Row{r.caller, 0, 0, r.linear};
        }
        rows[j].bytes += r.size;
        ++rows[j].n;
    }
    unlock();
    for (unsigned a = 1; a < nrows; ++a)
        for (unsigned b = a; b > 0 && rows[b].bytes > rows[b - 1].bytes; --b) {
            const Row t = rows[b];
            rows[b] = rows[b - 1];
            rows[b - 1] = t;
        }
    const struct mallinfo mi = mallinfo();
    printf("[census] f%d: heap in use %u KB (%u KB in blocks of %u KB or more), linear %u KB in such "
           "blocks; %u not kept\n",
           frame, (unsigned)(mi.uordblks / 1024), heap_big / 1024, (unsigned)(kBig / 1024),
           lin_big / 1024, g_lost);
    for (unsigned j = 0; j < nrows && j < 48; ++j)
        printf("[census]   %s 0x%08x: %u KB in %u\n", rows[j].linear ? "linear" : "heap  ",
               rows[j].caller, rows[j].bytes / 1024, rows[j].n);
    // the pages of the DS arena, main RAM and VRAM ever written (all start
    // zeroed): what of each the game has used so far
    if (data_020a0e9c) {
        static unsigned least = ~0u;
        const unsigned left = _ZN22ExpandingHeapAllocator10MemoryLeftEv(
            *(void **)((char *)data_020a0e9c + 0x14));
        if (left < least) least = left;
        printf("[census] root heap %u KB free (least %u KB)\n", left / 1024, least / 1024);
    }
    census_pages("arena", (const char *)port_arena_base(), (const char *)port_arena_end(), 0x40000);
    census_pages("main ram", (const char *)0x02000000, (const char *)0x02400000, 0x40000);
    census_pages("vram", (const char *)0x06000000, (const char *)0x06900000, 0x80000);
}

}  // extern "C"
