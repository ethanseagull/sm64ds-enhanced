// Lid close/open sounds.
//
// The DS game plays a sound when the lid closes, waits about two seconds, then
// sleeps, and plays another sound on wake (func_02019ac4). The port doesn't run
// that loop, so this redoes it on top of the 3DS sleep: we refuse sleep, poll
// the lid each tick, play the sound, and only then allow sleep again.
//
// Before the sound system is up, and in the options menu, the lid just sleeps
// right away like before.
//
// GAME3DS_LID=0 turns this off. GAME3DS_LID_SCRIPT=close@T,open@T fakes the lid
// (Azahar doesn't have one).
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
extern unsigned char data_0209b480[];  // sound system is up
extern unsigned char data_0209b474;  // the game's "lid closed" flag
extern int data_0209fc68;  // 6 after a wireless error
extern int data_0208ee44;  // ticks per frame
extern unsigned char data_0209f1d8;
extern unsigned char data_020a0f10;
int func_0203da3c(void);
unsigned func_02012790(unsigned id);    // Sound::Play2D(2, id)
}

namespace {

bool g_on, g_allowed = true, g_woke, g_voice = true;
int g_state, g_tick;
short g_t;  // the game's timer
aptHookCookie g_hook;
const char *g_script;

void allow(bool a)
{
    if (a == g_allowed)
        return;
    g_allowed = a;
    aptSetSleepAllowed(a);
}

void apt_hook(APT_HookType hook, void *)
{
    if (hook == APTHOOK_ONWAKEUP)
        g_woke = true;
}

bool lid_closed()
{
    if (g_script) {
        // last close@T / open@T at or before this tick
        bool shut = false;
        for (const char *p = g_script; *p;) {
            int t = 0;
            const bool close = p[0] == 'c';
            const char *at = p;
            while (*at && *at != '@' && *at != ',') ++at;
            if (*at == '@') t = atoi(at + 1);
            if (g_tick >= t) shut = close;
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
        return shut;
    }
    u8 open = 1;
    if (R_FAILED(PTMU_GetShellState(&open)))
        return false;
    return open == 0;
}

void play(unsigned id)
{
    printf("[lid] tick %d: sound %#x\n", g_tick, id);
    func_02012790(id);
}

}  // namespace

// Called from main_game.cpp before the first frame.
extern "C" void game3ds_lid_start(void)
{
    const char *e = getenv("GAME3DS_LID");
    if (e && e[0] == '0') {
        printf("[lid] off (GAME3DS_LID=0): the 3DS's own sleep, no lid sounds\n");
        return;
    }
    g_script = getenv("GAME3DS_LID_SCRIPT");
    if (!g_script && R_FAILED(ptmuInit())) {
        printf("[lid] no ptm:u: the 3DS's own sleep, no lid sounds\n");
        return;
    }
    aptHook(&g_hook, apt_hook, nullptr);
    g_on = true;
    printf("[lid] the cartridge's close and open sounds%s%s\n",
           g_script ? ", a pretend lid: " : "", g_script ? g_script : "");
}

// The options menu runs its own loop, so let the lid sleep normally there.
extern "C" void game3ds_lid_plain(void)
{
    if (g_on && g_state == 0)
        allow(true);
}

// Called once per tick. Same states as the game's func_02019ac4.
extern "C" void game3ds_lid_frame(void)
{
    if (!g_on)
        return;
    ++g_tick;
    const bool shut = lid_closed();
    const bool skip = func_0203da3c() != 0 || data_0209f1d8 != 0;
    // Same check as func_0203d974 (Download Play or wireless). The port's version
    // always returns 1 for the sound engine, so it can't be used here.
    const bool alt = *(volatile unsigned short *)0x027ffc40 == 2 || data_020a0f10 != 0;
    const int d = data_0208ee44 > 0 ? data_0208ee44 : 1;
    if (data_0209fc68 == 6)
        g_voice = false;
    if (g_state == 0) {
        if (!shut)
            allow(false);
        else if (!data_0209b480[0])
            allow(true);  // no sound yet, just sleep
        else {
            printf("[lid] tick %d: closed\n", g_tick);
            data_0209b474 = 1;
            g_state = 1;
        }
    }
    if (g_state == 1) {
        if (g_t > 0)
            g_t -= d;
        else {
            if (g_voice) {
                g_t = alt ? 0x3c : 0x78;
                play(alt ? 0x44 : 0x10);
            } else {
                g_t = 0;
            }
            g_state = 2;
        }
    }
    if (g_state == 2) {
        if (!shut) {
            printf("[lid] tick %d: opened before the sleep\n", g_tick);
            g_state = 6;
        } else if (g_t > 0) {
            g_t -= d;
        } else {
            g_state = 3;
        }
    }
    if (g_state == 3) {
        if (!skip) {
            printf("[lid] tick %d: sleep\n", g_tick);
            g_woke = false;
            allow(true);  // the next aptMainLoop will sleep
            g_state = 4;
        } else if (!shut) {
            g_state = 4;
        }
    }
    if (g_state == 4) {
        if (g_woke || !shut) {
            printf("[lid] tick %d: %s\n", g_tick, g_woke ? "woke" : "opened (no sleep came)");
            allow(false);
            g_state = 5;
            g_t = 1;
        }
    }
    if (g_state == 5) {
        if (g_t > 0)
            g_t -= d;
        else {
            if (g_voice) {
                g_t = alt ? 0x3c : 0xb4;
                play(alt ? 0x45 : 0x11);
            } else {
                g_t = 0;
            }
            g_state = 6;
        }
    }
    if (g_state == 6) {
        if (g_t <= 0) {
            g_state = 0;
            data_0209b474 = 0;
        } else {
            g_t -= d;
        }
    }
}
