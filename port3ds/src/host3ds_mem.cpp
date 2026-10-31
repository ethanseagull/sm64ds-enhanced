// DS memory regions at their real addresses, for port/ntr/io.cpp.
//
// A 3DS app can't allocate new memory at 0x02000000-0x07FFFFFF, but it can
// alias pages it already owns there. svcControlMemory's MAP is refused on real
// hardware (it only accepts the alias region), so we use
// svcControlProcessMemory, which has no such check; Luma's homebrew loader
// gives 3dsx apps every SVC. The source pages come from the heap and are never
// freed or touched again.
#include <3ds.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>

// svcControlProcessMemory rejects the CUR_PROCESS_HANDLE pseudo-handle on real
// hardware, so open a real handle once.
static Result self_handle(Handle *out)
{
    static Handle self = 0;
    Result r = 0;
    if (!self) {
        Handle dup = 0;
        r = svcDuplicateHandle(&dup, CUR_PROCESS_HANDLE);
        if (R_FAILED(r)) {
            u32 pid = 0;
            if (R_SUCCEEDED(svcGetProcessId(&pid, CUR_PROCESS_HANDLE)))
                r = svcOpenProcess(&dup, pid);
        }
        if (R_SUCCEEDED(r))
            self = dup;
    }
    *out = self;
    return r;
}

extern "C" void *host3ds_map_fixed(uintptr_t base, size_t size)
{
    const size_t page = 0x1000;
    size = (size + page - 1) & ~(page - 1);
    void *src = memalign(page, size);
    if (!src) {
        fprintf(stderr, "host3ds_map_fixed: no %u KB of heap for %08x\n",
                (unsigned)(size / 1024), (unsigned)base);
        return nullptr;
    }
    const MemPerm rw = (MemPerm)(MEMPERM_READ | MEMPERM_WRITE);
    u32 out = 0;
    Result r = svcControlMemory(&out, (u32)base, (u32)src, (u32)size, MEMOP_MAP, rw);
    if (R_SUCCEEDED(r))
        return reinterpret_cast<void *>(base);
    Handle self = 0;
    Result r2 = self_handle(&self);
    if (self)
        r2 = svcControlProcessMemory(self, (u32)base, (u32)src, (u32)size,
                                     MEMOP_MAP, rw);
    if (R_SUCCEEDED(r2)) {
        fprintf(stderr, "host3ds_map_fixed: %08x+%x mapped by ControlProcessMemory "
                "(ControlMemory: %08lx)\n", (unsigned)base, (unsigned)size,
                (unsigned long)r);
        return reinterpret_cast<void *>(base);
    }
    fprintf(stderr, "host3ds_map_fixed: MEMOP_MAP %08x+%x failed: ControlMemory "
            "%08lx, ControlProcessMemory %08lx\n", (unsigned)base, (unsigned)size,
            (unsigned long)r, (unsigned long)r2);
    free(src);
    return nullptr;
}

// The HAL writes host functions into GCC's vtables at runtime, and GCC puts
// vtables in .rodata. Azahar doesn't enforce read-only there, but a real New
// 3DS does (permission fault). So make the whole read-only data block
// (.rodata, .ARM.extab, .ARM.exidx) writable before any static initializer
// runs. svcQueryMemory finds the block containing a .rodata object.
static const char k_rodata_probe[] = "rodata";

extern "C" void host3ds_rodata_writable(void)
{
    MemInfo info;
    PageInfo pinfo;
    Result r = svcQueryMemory(&info, &pinfo, (u32)(uintptr_t)k_rodata_probe);
    if (R_FAILED(r)) {
        fprintf(stderr, "host3ds_rodata_writable: QueryMemory failed: %08lx\n",
                (unsigned long)r);
        return;
    }
    if (info.perm & MEMPERM_WRITE)
        return;  // already writable (Azahar may report RW)
    Handle self = 0;
    r = self_handle(&self);
    if (self)
        r = svcControlProcessMemory(self, info.base_addr, info.base_addr, info.size,
                                    MEMOP_PROT,
                                    (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
    fprintf(stderr, "host3ds_rodata_writable: %08lx+%lx perm %lx -> RW: %08lx\n",
            (unsigned long)info.base_addr, (unsigned long)info.size,
            (unsigned long)info.perm, (unsigned long)r);
}
