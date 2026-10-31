// Write the save file off the game thread.
//
// Every save used to freeze the castle for 4-5 s. One save is sixteen card
// writes, and ntr/backup.cpp persisted each by rereading the whole 8 KB file
// and replacing it through a temp file (~0.25 s each on the SD card). The PC
// needs that because two windows can share a save; one 3DS process doesn't.
//
// So saving is deferred: writes only update the in-memory image. Once a frame
// (in game3ds_main_loop) a dirty image is copied out, by which point all
// sixteen writes have landed, and a worker thread below the game thread writes
// it once. Reads come from the same image, so nothing waits.
//
// The image has to reach the card before the app can go away, so
// game3ds_save_flush runs from the APT hook on suspend (HOME), sleep and exit,
// after the game returns, and from the crash handler. A power cut during the
// ~0.3 s write is covered by the temp-and-replace (save_replace.cpp).
// GAME3DS_SAVE_SYNC=1 writes on every card write like before.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include "ntr/backup.h"

extern "C" void game3ds_save_flush(void);

namespace {

unsigned char g_take[ntr::backup::kSize];      // the game thread's copy
unsigned char g_pending[ntr::backup::kSize];   // the newest image to write
unsigned char g_work[ntr::backup::kSize];      // the one being written
bool g_has;                                    // g_pending is unwritten
bool g_busy;                                   // the worker is writing
LightLock g_lock;                              // g_pending, g_has, g_busy
LightEvent g_wake;
Thread g_thread;
volatile bool g_quit;
bool g_on;
aptHookCookie g_apt;
unsigned g_saves;

void write_one(const unsigned char *img)
{
    const u64 t0 = svcGetSystemTick();
    const bool ok = ntr::backup::persist(img);
    printf("[save] image %u written in %.0f ms%s\n", ++g_saves,
           (svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11, ok ? "" : " -- FAILED");
}

void worker(void *)
{
    for (;;) {
        LightEvent_Wait(&g_wake);
        for (;;) {
            LightLock_Lock(&g_lock);
            if (!g_has) {
                LightLock_Unlock(&g_lock);
                break;
            }
            memcpy(g_work, g_pending, sizeof g_work);
            g_has = false;
            g_busy = true;
            LightLock_Unlock(&g_lock);
            write_one(g_work);
            LightLock_Lock(&g_lock);
            g_busy = false;
            LightLock_Unlock(&g_lock);
        }
        if (g_quit) break;
    }
}

void apt_hook(APT_HookType hook, void *)
{
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONEXIT)
        game3ds_save_flush();
}

}  // namespace

// Before the first card access (main_game.cpp, after the env file).
void game3ds_save_start(void)
{
    // Starting opens the medium, which resolves its path ONCE: the path must
    // be set by now (main_game.cpp), or the game reads a blank chip elsewhere.
    if (!getenv("SM64DS_SAVE_PATH"))
        printf("[save] WARNING: SM64DS_SAVE_PATH is not set yet; the save opens at "
               "the fallback path\n");
    if (getenv("GAME3DS_SAVE_SYNC")) {
        printf("[save] synchronous: every card write persists (GAME3DS_SAVE_SYNC)\n");
        return;
    }
    LightLock_Init(&g_lock);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(worker, nullptr, 16 * 1024, prio < 0x3f ? prio + 1 : prio, -2, false);
    ntr::backup::set_deferred(true);
    aptHook(&g_apt, apt_hook, nullptr);
    g_on = true;
    printf("[save] deferred: one write a save, %s; %s (%s)\n",
           g_thread ? "on a worker thread" : "on the game thread (no worker)",
           ntr::backup::path(), ntr::backup::was_fresh() ? "no file: a blank chip" : "loaded");
}

// Every frame boundary: hand a dirty image to the worker.
void game3ds_save_frame(void)
{
    if (!g_on || !ntr::backup::take_dirty(g_take)) return;
    if (!g_thread) {
        write_one(g_take);
        return;
    }
    LightLock_Lock(&g_lock);
    memcpy(g_pending, g_take, sizeof g_pending);
    g_has = true;
    LightLock_Unlock(&g_lock);
    LightEvent_Signal(&g_wake);
}

// Everything written before this returns.
extern "C" void game3ds_save_flush(void)
{
    if (!g_on) return;
    game3ds_save_frame();
    if (!g_thread) return;
    for (;;) {
        LightLock_Lock(&g_lock);
        const bool done = !g_has && !g_busy;
        LightLock_Unlock(&g_lock);
        if (done) return;
        LightEvent_Signal(&g_wake);
        svcSleepThread(1000 * 1000);
    }
}
