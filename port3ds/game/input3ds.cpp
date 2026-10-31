// 3DS controls, read once a frame. Separate file because <3ds.h> and the
// decomp's types.h can't be in the same file (u32/s32 differ); main_game.cpp
// passes what this reads to the HAL.
//
// Buttons: hidKeysHeld()'s low 12 bits match the DS's KEYINPUT layout, so the
// raw word passes straight through. The Circle Pad also acts as the D-pad
// (menus, and below the analog dead zone) and is read as a stick for analog
// movement.
// Touch: the bottom screen is 320x240 showing the DS's 256x192 scaled up, so
// touches map by 256/320.
// SELECT never reaches the game; it opens the options menu instead of the DS's
// control style menu.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_cpad_x, g_cpad_y, g_cstick_x, g_cstick_y;

extern "C" int game3ds_options_poll(void);   // game/options3ds.cpp

// the held positions of a probe list `from-to:x:y[,...]` at input read `reads`
static void probe_apply(const char *probe, int reads, int &px, int &py)
{
    for (const char *q = probe; q && *q;) {
        int a = 0, b = 0, x = 0, y = 0;
        if (sscanf(q, "%d-%d:%d:%d", &a, &b, &x, &y) == 4 && reads >= a && reads <= b) {
            px = x;
            py = y;
        }
        q = strchr(q, ',');
        if (q) ++q;
    }
}

extern "C" void game3ds_input_read(unsigned *raw_keys, int *touch_down, int *tx, int *ty)
{
    hidScanInput();
    // What was held as the options menu closed (the B that closed it, the
    // tap on BACK) is not the game's until it is let go.
    static u32 swallow;
    if (game3ds_options_poll()) {
        hidScanInput();
        swallow = hidKeysHeld();
    }
    swallow &= hidKeysHeld();
    const u32 held = hidKeysHeld() & ~swallow;
    circlePosition cp;
    hidCircleRead(&cp);
    g_cpad_x = cp.dx;
    g_cpad_y = cp.dy;
    // The New 3DS C-stick, through IR:rst (an Old 3DS reads it as centred).
    static int irrst = -1;
    if (irrst < 0) irrst = R_SUCCEEDED(irrstInit()) ? 1 : 0;
    g_cstick_x = g_cstick_y = 0;
    if (irrst) {
        irrstScanInput();
        circlePosition cs;
        irrstCstickRead(&cs);
        g_cstick_x = cs.dx;
        g_cstick_y = cs.dy;
    }
    // GAME3DS_CPAD_PROBE / GAME3DS_CSTICK_PROBE=from-to:x:y[,...]: hold the stick
    // at (x, y) over those input reads (for tests, since Azahar can't be driven
    // that way).
    static const char *cpad_probe = getenv("GAME3DS_CPAD_PROBE");
    static const char *cstick_probe = getenv("GAME3DS_CSTICK_PROBE");
    static int reads;
    ++reads;
    probe_apply(cpad_probe, reads, g_cpad_x, g_cpad_y);
    probe_apply(cstick_probe, reads, g_cstick_x, g_cstick_y);
    unsigned raw = held & 0x0fffu & ~(u32)KEY_SELECT;
    // GAME3DS_KEYS_PROBE=from-to:bits[,...]: DS pad bits (hex, KEYINPUT's
    // layout: 1 A, 8 START) held over those input reads, for the same tests
    static const char *keys_probe = getenv("GAME3DS_KEYS_PROBE");
    for (const char *q = keys_probe; q && *q;) {
        int a = 0, b = 0;
        unsigned bits = 0;
        if (sscanf(q, "%d-%d:%x", &a, &b, &bits) == 3 && reads >= a && reads <= b) raw |= bits & 0x0fffu;
        q = strchr(q, ',');
        if (q) ++q;
    }
    if (held & KEY_CPAD_RIGHT) raw |= KEY_DRIGHT;
    if (held & KEY_CPAD_LEFT)  raw |= KEY_DLEFT;
    if (held & KEY_CPAD_UP)    raw |= KEY_DUP;
    if (held & KEY_CPAD_DOWN)  raw |= KEY_DDOWN;
    *raw_keys = raw;

    *touch_down = 0;
    if (held & KEY_TOUCH) {
        touchPosition t;
        hidTouchRead(&t);
        const int x = (int)t.px * 256 / 320, y = (int)t.py * 192 / 240;
        if (x >= 0 && x < 256 && y >= 0 && y < 192) {
            *touch_down = 1;
            *tx = x;
            *ty = y;
        }
    }
}

// The Circle Pad as hidScanInput last read it: x right, y UP, about +-156 at
// the rim.
extern "C" void game3ds_cpad(int *x, int *y)
{
    *x = g_cpad_x;
    *y = g_cpad_y;
}

// The C-stick likewise (New 3DS; 0, 0 elsewhere).
extern "C" void game3ds_cstick(int *x, int *y)
{
    *x = g_cstick_x;
    *y = g_cstick_y;
}

// The HOME menu / power button: false once the app is asked to close.
void game3ds_save_frame(void);   // game/save_async.cpp
// Every loop's frame boundary: a finished save goes to its writer first, so
// the APT hook's flush (HOME, sleep, exit) inside aptMainLoop sees it.
extern "C" void game3ds_watchdog_beat(void);       // game/watchdog3ds.cpp
extern "C" void game3ds_watchdog_away(int away);
extern "C" void game3ds_lid_frame(void);           // game/lid3ds.cpp
extern "C" int game3ds_main_loop(void)
{
    game3ds_save_frame();
    game3ds_watchdog_beat();
    game3ds_lid_frame();        // the lid's sounds, and when the sleep below may come
    game3ds_watchdog_away(1);   // HOME and sleep wait in here
    const bool on = aptMainLoop();
    game3ds_watchdog_away(0);
    return on ? 1 : 0;
}
