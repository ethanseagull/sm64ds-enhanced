// Watchdog: reports where the game thread is when it stops finishing frames.
//
// A hang on hardware (Memory Match) froze the picture while music kept
// playing, and the log stopped too, since its writer runs below the game thread
// and a spinning game thread starves it.
//
// A thread one priority above the game thread wakes once a second and checks
// the frame counter game3ds_main_loop bumps. If nothing moved for kStallS
// seconds and the game isn't in the APT (HOME, sleep) or the options menu, it
// prints a "[hang]" report and flushes the log: the current or last phase and
// any wait in it, the scene, the actor being walked, and the stack words below
// the phase's entry (return addresses; look them up with
// arm-none-eabi-addr2line -f -C -e game3ds.elf). Repeats every kAgainS seconds
// while it lasts, and prints "went on" if it recovers. GAME3DS_WATCHDOG=0
// disables it.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>

#include "console_tee.h"
#include "gpu3ds.h"

extern "C" {
extern void *data_020a4b68;    // the ROM actor walk's current node
extern unsigned data_02099f24;  // its phase
int port_scene_live_id(void);   // hal/scene_boot.cpp: the scene path's id, or -1
void game3ds_phase_trail(const char **in, const char **done, const char **wait,
                         const unsigned **sp);   // game/phase_timers.cpp
extern const char *volatile g_card3ds_wait;     // src/card3ds.cpp
extern const char *volatile g_card3ds_worker;
}

namespace {

constexpr int kStallS = 4, kAgainS = 10;

volatile unsigned g_beat;
volatile bool g_away;   // in the APT or the options menu: not counted
Thread g_thread;
unsigned g_hang_at;     // GAME3DS_TEST_HANG

bool heap_pointer(const void *p, unsigned bytes)
{
    const unsigned at = (unsigned)p;
    return !(at & 3) && at >= 0x08000000 && at <= 0x0E000000 - bytes;
}

void report(int secs, unsigned beat, bool first)
{
    const char *in, *done, *wait;
    const unsigned *sp;
    game3ds_phase_trail(&in, &done, &wait, &sp);
    printf("[hang] the game thread has not ended a frame for %d s (beat %u): %s%s, last finished %s%s%s; "
           "scene %d\n",
           secs, beat, in ? "in " : "between phases", in ? in : "", done ? done : "-",
           wait ? ", waiting for " : "", wait ? wait : "", port_scene_live_id());
    static const char *const zones[] = {"other", "prepare", "targets", "effects", "pace_wait",
        "vblank_wait", "gpu_queue_wait", "begin_cpu", "geometry", "textures", "draw_submit",
        "overlay_submit", "overlay_swizzle", "overlay_flush", "vbo_flush", "frame_submit"};
    const int z = gpu3ds::present_zone();
    const char *const cw = g_card3ds_wait;
    printf("[hang] present: the game thread %s%s; the worker %s, zone %s\n",
           cw ? "waits for " : "is not waiting on it", cw ? cw : "", g_card3ds_worker,
           z >= 0 && z < (int)(sizeof zones / sizeof *zones) ? zones[z] : "?");
    void *node = data_020a4b68;
    printf("[hang] actor walk: phase %u node %p", data_02099f24, node);
    if (heap_pointer(node, 12)) {
        const void *actor = ((void *const *)node)[2];
        if (heap_pointer(actor, 0x68))
            printf(", actor %p id %u vptr %08x", actor,
                   (unsigned)*(const unsigned short *)((const char *)actor + 0x0c),
                   *(const unsigned *)actor);
    }
    printf("\n");
    if (first && sp && (unsigned)sp >= 0x08000000 + 1024 && (unsigned)sp < 0x10000000) {
        // the 256 words below the phase's own frame: what it called, deepest last
        printf("[hang] stack below %p:", (const void *)sp);
        const unsigned *w = sp - 256;
        for (int i = 0; i < 256; ++i) {
            if (!(i & 15)) printf("\n[hang]  ");
            printf(" %08x", w[i]);
        }
        printf("\n");
    }
    console_tee_flush();
}

void watch(void *)
{
    unsigned last = g_beat;
    int still = 0, said = 0;
    for (;;) {
        svcSleepThread(1000 * 1000 * 1000LL);
        const unsigned beat = g_beat;
        if (beat != last || g_away) {
            if (said) {
                printf("[hang] the game thread went on after %d s\n", still);
                console_tee_flush();
            }
            last = beat;
            still = said = 0;
            continue;
        }
        ++still;
        if (still == kStallS || (still > kStallS && (still - kStallS) % kAgainS == 0)) {
            report(still, beat, !said);
            said = 1;
        }
    }
}

}  // namespace

extern "C" void game3ds_watchdog_start(void)
{
    if (getenv("GAME3DS_TEST_HANG")) g_hang_at = g_beat + atoi(getenv("GAME3DS_TEST_HANG"));
    if (g_thread || (getenv("GAME3DS_WATCHDOG") && atoi(getenv("GAME3DS_WATCHDOG")) == 0)) return;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    const s32 wprio = prio > 0x19 ? 0x19 : prio;
    g_thread = threadCreate(watch, nullptr, 16 * 1024, wprio, -2, true);
    printf("[hang] watchdog %s (core 0, priority %ld: a frame not ended in %d s is reported)\n",
           g_thread ? "on" : "refused", (long)wprio, kStallS);
}

// Each frame's end (game3ds_main_loop). GAME3DS_TEST_HANG=N: the game thread
// spins for good N frames after the watchdog starts (its own test).
extern "C" void game3ds_watchdog_beat(void)
{
    if (++g_beat >= g_hang_at && g_hang_at) {
        printf("[hang] GAME3DS_TEST_HANG: the game thread spins from here\n");
        for (volatile unsigned n = 0;; n = n + 1) {
        }
    }
}

// Around the APT's waits (HOME, sleep) and the options menu: a stop there is
// the player's, not a hang
extern "C" void game3ds_watchdog_away(int away)
{
    g_away = away != 0;
}
