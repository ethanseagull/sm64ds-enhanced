// Map the DS's fixed address ranges before any static initializer runs.
//
// On PC, ntr/io.cpp does this from a TLS callback ahead of the CRT, because
// the game's static constructors already touch DS memory. On 3DS the
// equivalent is a constructor with the highest user priority (libctru has set
// up the heap by then). io_init() in main is then a no-op, same as PC.
#include "ntr/mmio.h"

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

// Progress marks before the main log exists, appended to
// sdmc:/sm64ds3ds/game3ds_early.log and flushed one at a time (libctru mounts
// the SD card before the static initializers run). The file stays open because
// an SD open + close costs 30-40 ms and every boot step logs a mark.
extern "C" void game3ds_early_mark(const char *what)
{
    // emptied by the first mark, then appended to (early_write below appends
    // to the same file through its own opens)
    static FILE *f = [] {
        // a card the game has never run on has no folder yet (the .cia brings
        // its data along: mount_romfs_data below)
        mkdir("sdmc:/sm64ds3ds", 0777);
        if (FILE *t = fopen("sdmc:/sm64ds3ds/game3ds_early.log", "w")) fclose(t);
        return fopen("sdmc:/sm64ds3ds/game3ds_early.log", "a");
    }();
    if (f) { fprintf(f, "%s\n", what); fflush(f); }
}

// Send what the HAL prints during static init (stdout and stderr) to the same
// file, since the console and main's log don't exist yet. The original devices
// are restored before main.
namespace {
const devoptab_t *g_std_out, *g_std_err;
devoptab_t g_early_dev;
ssize_t early_write(struct _reent *, void *, const char *ptr, size_t len)
{
    FILE *f = fopen("sdmc:/sm64ds3ds/game3ds_early.log", "a");
    if (f) { fwrite(ptr, 1, len, f); fclose(f); }
    return (ssize_t)len;
}
}  // namespace

extern "C" {
typedef void (*InitFn)(void);
extern InitFn __init_array_start[], __init_array_end[];
// The ROM's ov0 handle table (src/func_02018a24.c reads it for every file
// looked up by handle). The boot builds it later, but some static
// SharedFilePtr subclasses look up a handle in their constructors (on the DS
// those only run when their overlay loads). During static init point it at
// zeros so those lookups return file 0 instead of reading near address 0.
extern unsigned short *data_0209d3b8;
// The PC HAL's MSVC-registered static initializers (.CRT$XCV, .CRT$XCW, run
// after the normal constructors). GCC has no such table, so the two that
// exist are named here, in section order.
extern InitFn port_stage_geom_ctor;   // hal/stage_geom.cpp, .CRT$XCV
extern InitFn port_mods_ctor;         // hal/mods.cpp, .CRT$XCW
// src/host3ds_mem.cpp: the HAL's seats write GCC's vtables in .rodata.
void host3ds_rodata_writable(void);
// Our own version of libctru's heap split. A New 3DS launch gets ~110 MB free;
// with only 8 MB linear libctru asked for a 104 MB heap, past the 96 MB heap
// area, and crashed before any game code ran (Azahar doesn't enforce that
// limit). On Old 3DS libctru's 24/24 MB split leaves too little heap, so the
// linear heap is kOldLinear and the rest is heap (~10-11 MB of linear is used
// for GPU buffers and textures). Azahar's Old 3DS mode gives a .3dsx 96 MB,
// so we cap it to the retail 64 MB there too.
extern u32 __ctru_heap_size, __ctru_linear_heap_size;
extern u32 __ctru_heap, __ctru_linear_heap;
extern char *fake_heap_start, *fake_heap_end;
void mappableInit(u32 addrMin, u32 addrMax);

// 1: this launch got an Old 3DS's region (main_game.cpp sizes what it can to it)
int game3ds_old_budget;
static s64 g_max_commit;   // the resource limit's, for the log below

// libctru's own (allocateHeaps.c), with the small-region case added.
void __system_allocateHeaps(void)
{
    const u32 kOldLinear = 14u << 20;
    const s64 kOldRegion = 64 << 20;
    Handle reslimit = 0;
    if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
    s64 max_commit = 0, cur_commit = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
    svcGetResourceLimitCurrentValues(&cur_commit, reslimit, &type, 1);
    svcCloseHandle(reslimit);
    g_max_commit = max_commit;
    u32 remaining = (u32)(max_commit - cur_commit) & ~0xFFFu;

    if (max_commit < (100 << 20)) {
        // an Old 3DS (64 MB; Azahar's 96 MB kept to it)
        game3ds_old_budget = 1;
        if (max_commit > kOldRegion) remaining = (u32)(kOldRegion - cur_commit) & ~0xFFFu;
        __ctru_linear_heap_size = kOldLinear;
    } else {
        // libctru's default: half and half, the heap at least 24 MB, the
        // linear heap at most 32 MB
        __ctru_linear_heap_size = (remaining / 2) & ~0xFFFu;
        if (remaining - __ctru_linear_heap_size > (24u << 20)) {
            __ctru_linear_heap_size = remaining - (24u << 20);
            if (__ctru_linear_heap_size > (32u << 20)) __ctru_linear_heap_size = 32u << 20;
        }
    }
    __ctru_heap_size = remaining - __ctru_linear_heap_size;

    if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, __ctru_heap_size, MEMOP_ALLOC,
                                  (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
        svcBreak(USERBREAK_PANIC);
    if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
                                  (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
        svcBreak(USERBREAK_PANIC);
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}
}
static void init_done(void) {}

// Game data inside the .cia: the installed title carries build/assets and
// extracted/ in its RomFS (cia/make_cia.py), so it runs without anything
// staged on the SD card. Mounted here because the static initializers already
// read build/assets, and every HAL asset reader checks SM64DS_ASSET_ROOT first.
// A .3dsx has no RomFS and reads sdmc:/sm64ds3ds; it's named explicitly
// because a ROM-clean build has no PORT_REPO_ROOT fallback.
// port_romdata_load is only linked in a ROM-clean build (tools/pcprobe.sh).
extern "C" void port_romdata_load(void) __attribute__((weak));

static void mount_romfs_data(void)
{
    if (R_FAILED(romfsInit())) {
        setenv("SM64DS_ASSET_ROOT", "sdmc:/sm64ds3ds", 0);
        game3ds_early_mark("data: no RomFS, from sdmc:/sm64ds3ds");
        return;
    }
    struct stat st;
    if (stat("romfs:/build/assets/files.tsv", &st) != 0) {
        romfsExit();
        setenv("SM64DS_ASSET_ROOT", "sdmc:/sm64ds3ds", 0);
        game3ds_early_mark("data: a RomFS without build/assets, from sdmc:/sm64ds3ds");
        return;
    }
    setenv("SM64DS_ASSET_ROOT", "romfs:", 0);
    game3ds_early_mark("data: from the title's RomFS (romfs:/)");
}

__attribute__((constructor(101))) static void game3ds_early_io(void)
{
    game3ds_early_mark("early io_reserve");
    g_std_out = devoptab_list[STD_OUT];
    g_std_err = devoptab_list[STD_ERR];
    g_early_dev = *g_std_err;
    g_early_dev.write_r = early_write;
    devoptab_list[STD_OUT] = &g_early_dev;
    devoptab_list[STD_ERR] = &g_early_dev;
    host3ds_rodata_writable();
    {
        // How much memory this launch got: a hardware launch's application
        // region depends on the title the Homebrew Launcher runs under.
        // On a New 3DS a .cia asking for 124 MB still got 64 MB (2026-10-29), so
        // the console and the kernel's memory mode go in the log too (app_memtype:
        // 0 is 64 MB, 6 the New 3DS's 124 MB, 7 its 178 MB).
        bool n3ds = false;
        const bool asked = R_SUCCEEDED(APT_CheckNew3DS(&n3ds));
        char buf[240];
        snprintf(buf, sizeof buf, "memory: heap %lu KB, linear heap %lu KB, "
                 "application region %lu KB (free %lu KB), commit limit %lu KB, memory mode %u, %s",
                 (unsigned long)(__ctru_heap_size / 1024),
                 (unsigned long)(__ctru_linear_heap_size / 1024),
                 (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024),
                 (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
                 (unsigned long)(g_max_commit / 1024),
                 (unsigned)OS_KernelConfig->app_memtype,
                 !asked ? "console unknown" : n3ds ? "a New 3DS" : "an Old 3DS");
        game3ds_early_mark(buf);
    }
    ntr::io_reserve(1);
    game3ds_early_mark("early io_reserve done");
    mount_romfs_data();
    // Load the ROM's tables before the static initializers. In a ROM-clean build
    // the generated tables are zeroed and filled from build/assets/romdata.bin.
    // hal/fs_names.cpp's initializer runs the ROM's file system setup, which
    // writes into those tables; loading them later in main overwrote that, the
    // lookup of the "rom" archive returned null, and level 1 crashed while
    // loading.
    if (port_romdata_load) {
        game3ds_early_mark("romdata: loading");
        port_romdata_load();
        game3ds_early_mark("romdata: loaded");
    }
    // Run the rest of the static initializers here one at a time, with a mark
    // before each (addr2line -e game3ds.elf gives the name), so a hang or crash in
    // one can be traced. .init_array is writable: each entry is replaced with a
    // no-op once run, so when __libc_init_array gets to them afterwards it only
    // calls no-ops.
    static unsigned short no_handles[0x80a];
    const bool stand_in = data_0209d3b8 == nullptr;
    if (stand_in)
        data_0209d3b8 = no_handles;
    for (InitFn *p = __init_array_start; p < __init_array_end; ++p) {
        InitFn fn = *p;
        if (fn == game3ds_early_io || fn == init_done)
            continue;
        char buf[32];
        snprintf(buf, sizeof buf, "init %p", (void *)fn);
        game3ds_early_mark(buf);
        *p = init_done;
        fn();
    }
    if (stand_in && data_0209d3b8 == no_handles)
        data_0209d3b8 = nullptr;
    // Then the MSVC-registered ones.
    game3ds_early_mark("crt_xc");
    port_stage_geom_ctor();
    port_mods_ctor();
    fflush(stdout);
    fflush(stderr);
    devoptab_list[STD_OUT] = g_std_out;
    devoptab_list[STD_ERR] = g_std_err;
    game3ds_early_mark("static initialisers done");
}
