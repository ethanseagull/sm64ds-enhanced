// Entry point for every smoke test binary. The PC smoke's main() is compiled
// as sm64ds_smoke_main() (see sm64ds_smoke() in CMakeLists.txt); this runs it
// on 3DS with output on the top screen and in sdmc:/sm64ds3ds/<gate>.log.
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "console_tee.h"
#include "crash_handler.h"
#include "memprobe.h"

#ifndef SMOKE_NAME
#define SMOKE_NAME "smoke"
#endif

int sm64ds_smoke_main(void);
// game/save_async.cpp, in the game only
extern "C" void game3ds_save_flush(void) __attribute__((weak));
// game/main_game.cpp, in the game only: the run ended on save and quit
extern "C" int game3ds_quit_to_title(void) __attribute__((weak));
extern "C" { extern int __system_argc; extern char **__system_argv; }

// Save and quit back to the title, the old way. The PC port restarts the
// process because it can't tear a level down to the title. Here we ask the
// homebrew loader (Luma's hb:ldr, like the Homebrew Launcher does) to start
// this same .3dsx when we exit, and it boots to the title. Without hb:ldr the
// app just exits to the launcher. Installed as a .cia, APT restarts the title.
static bool relaunch_self(void)
{
    if (!envIsHomebrew()) {
        aptSetChainloaderToSelf();
        printf("[quit] installed title: APT will start it again\n");
        return true;
    }
    if (__system_argc < 1 || !__system_argv || !__system_argv[0]) {
        printf("[quit] no argv[0]: cannot name this .3dsx, exiting to the launcher\n");
        return false;
    }
    const char *full = __system_argv[0];
    const char *path = strncmp(full, "sdmc:/", 6) == 0 ? full + 5 : full;
    Handle h = 0;
    Result r = svcConnectToPort(&h, "hb:ldr");
    if (R_FAILED(r)) {
        printf("[quit] no hb:ldr (%08lx): exiting to the launcher\n", (unsigned long)r);
        return false;
    }
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(2, 0, 2);   // SetTarget
    cmd[1] = IPC_Desc_StaticBuffer(strlen(path) + 1, 0);
    cmd[2] = (u32)path;
    r = svcSendSyncRequest(h);
    if (R_SUCCEEDED(r)) r = cmd[1];
    if (R_SUCCEEDED(r)) {
        static u32 argv_buf[0x400 / 4];   // argc, then the NUL-terminated strings
        memset(argv_buf, 0, sizeof argv_buf);
        argv_buf[0] = 1;
        strncpy((char *)&argv_buf[1], full, sizeof argv_buf - 5);
        cmd = getThreadCommandBuffer();
        cmd[0] = IPC_MakeHeader(3, 0, 2);   // SetArgv
        cmd[1] = IPC_Desc_StaticBuffer(sizeof argv_buf, 1);
        cmd[2] = (u32)argv_buf;
        r = svcSendSyncRequest(h);
        if (R_SUCCEEDED(r)) r = cmd[1];
    }
    svcCloseHandle(h);
    printf("[quit] hb:ldr %s %s (%08lx)\n", R_SUCCEEDED(r) ? "will start" : "refused", path,
           (unsigned long)r);
    return R_SUCCEEDED(r);
}
extern "C" { extern u32 __ctru_heap_size, __ctru_linear_heap_size; extern char *fake_heap_start, *fake_heap_end; }

// Main thread stack size. libctru's default is 32 KB, but the PC port's code
// assumes Windows' 1 MB and puts whole framebuffers on the stack (one
// ntr::Framebuffer is 196 KB), so the default overflowed.
extern "C" { u32 __stacksize__ = 2 * 1024 * 1024; }

int main()
{
    // New 3DS: 804 MHz and the L2 cache instead of 268 MHz without it (a
    // no-op on an old 3DS).
    osSetSpeedupEnable(true);
    gfxInitDefault();
    // Clear both screens right away. The framebuffers hold whatever the last app
    // left, and the game draws nothing for a few seconds while booting, so
    // launched from HOME the screens kept showing the Homebrew Launcher. Clear
    // both buffers of each screen so a swap can't bring it back.
    for (int pass = 0; pass < 2; ++pass) {
        static const gfxScreen_t screens[] = {GFX_TOP, GFX_BOTTOM};
        for (gfxScreen_t s : screens) {
            u16 w = 0, h = 0;
            u8 *fb = gfxGetFramebuffer(s, GFX_LEFT, &w, &h);
            if (fb) memset(fb, 0, (size_t)w * h * gspGetBytesPerPixel(gfxGetScreenFormat(s)));
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
#ifdef SMOKE_CONSOLE_BOTTOM
    consoleInit(GFX_BOTTOM, nullptr);   // the GPU owns the top screen
#else
    consoleInit(GFX_TOP, nullptr);
#endif
    const char *log = "sdmc:/sm64ds3ds/" SMOKE_NAME ".log";
    bool logged = console_tee_open(log);
    // The game boots straight into its own screens: its log goes only to the SD
    // card (game3ds.log), and the console text the smoke tests show only comes
    // back if the run fails or GAME3DS_BOOT_CONSOLE=1.
    const bool game = strcmp(SMOKE_NAME, "game3ds") == 0;
    if (game) console_tee_quiet(true);

    printf("sm64ds-3ds %s\n", SMOKE_NAME);
    // how this run was started, so a log says which of the two it came from
    printf("[launch] %s\n", envIsHomebrew() ? "from the Homebrew Launcher (.3dsx)"
                                            : "installed title, from the HOME Menu (.cia)");
    if (!logged) printf("(could not open %s)\n", log);
    crash_handler_install();

#ifdef SMOKE_MEMPROBE
    memprobe_run();
    printf("\n");
#endif

    int rc = sm64ds_smoke_main();
    if (game3ds_save_flush) game3ds_save_flush();   // the game's last save, written
    if (game && rc != 0) console_tee_quiet(false);
    printf("%s rc=%d\n", SMOKE_NAME, rc);
    {
        // What the run needed: newlib's arena only grows, so it is the peak.
        const struct mallinfo mi = mallinfo();
        printf("[mem] heap arena %u KB (in use %u KB) of %lu KB; linear free %lu KB of %lu KB\n",
               (unsigned)(mi.arena / 1024), (unsigned)(mi.uordblks / 1024),
               (unsigned long)((fake_heap_end - fake_heap_start) / 1024),   // less the main stack
               (unsigned long)(linearSpaceFree() / 1024),
               (unsigned long)(__ctru_linear_heap_size / 1024));
    }
    printf("RESULT %s\n", rc == 0 ? "PASS" : "FAIL");
    // save and quit: straight back to the title, no "Press START"
    const bool quit = game3ds_quit_to_title && game3ds_quit_to_title();
    if (quit) relaunch_self();
    console_tee_close();
    if (quit) {
        gfxExit();
        return 0;
    }

    printf("\nPress START to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
