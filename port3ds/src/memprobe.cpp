#include "memprobe.h"

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>

namespace {

// The regions port/ntr/include/ntr/mmio.h maps, at the sizes it uses
// (rounded up to the 4 KB page).
struct Region {
    const char *name;
    u32 base;
    u32 size;
};

constexpr Region kRegions[] = {
    {"main RAM", 0x02000000u, 0x00400000u},
    {"shared",   0x027FF000u, 0x00001000u},
    {"I/O",      0x04000000u, 0x00002000u},
    {"palettes", 0x05000000u, 0x00001000u},
    {"VRAM",     0x06000000u, 0x00900000u},
    {"OAM",      0x07000000u, 0x00001000u},
};

// Write a pattern at both ends and read it back.
bool touch(u32 base, u32 size)
{
    volatile u32 *lo = reinterpret_cast<volatile u32 *>(base);
    volatile u32 *hi = reinterpret_cast<volatile u32 *>(base + size - 4);
    *lo = 0x5A5A1234u;
    *hi = 0xA5A54321u;
    return *lo == 0x5A5A1234u && *hi == 0xA5A54321u;
}

}  // namespace

int memprobe_run()
{
    printf("memprobe: New 3DS? ");
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    printf("%s\n", n3ds ? "yes" : "no");
    printf("  app region free: %lu KB\n",
           (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024));
    printf("  linear free:     %lu KB\n", (unsigned long)(linearSpaceFree() / 1024));

    // Where things land on this system, for comparison with the DS addresses.
    static int a_static;
    int a_local;
    void *a_heap = malloc(16);
    printf("  .text %08lx  .bss %08lx  heap %08lx  stack %08lx\n",
           (unsigned long)(u32)&memprobe_run, (unsigned long)(u32)&a_static,
           (unsigned long)(u32)a_heap, (unsigned long)(u32)&a_local);
    free(a_heap);

    printf("  region     base      ALLOC     MAP\n");
    int ok = 0;
    for (const Region &r : kRegions) {
        u32 out = 0;
        // 1. Fresh memory at exactly that address.
        Result ra = svcControlMemory(&out, r.base, 0, r.size, MEMOP_ALLOC,
                                     (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
        Result rm = -1;
        if (R_FAILED(ra)) {
            // 2. Alias existing heap memory there. The source pages are never freed or
            //    touched again, so their changed permissions don't matter.
            void *src = memalign(0x1000, r.size);
            if (src)
                rm = svcControlMemory(&out, r.base, reinterpret_cast<u32>(src), r.size,
                                      MEMOP_MAP, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
        }
        bool mapped = R_SUCCEEDED(ra) || R_SUCCEEDED(rm);
        bool works = mapped && touch(r.base, r.size);
        if (works) ++ok;
        printf("  %-9s  %08lx  %08lx  %08lx  %s\n", r.name, (unsigned long)r.base,
               (unsigned long)ra, R_SUCCEEDED(ra) ? 0ul : (unsigned long)rm,
               works ? "OK" : (mapped ? "MAPPED, READBACK FAILED" : "no"));
    }
    printf("memprobe: %d / %d regions usable at DS addresses\n", ok,
           (int)(sizeof kRegions / sizeof kRegions[0]));
    return ok;
}
