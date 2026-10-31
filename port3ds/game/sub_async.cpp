// Scan out engine B (the bottom screen) on a worker thread, overlapped with
// the next tick.
//
// hal/sub_screen.cpp's present scans the bottom screen into g_sub every frame
// (~7 ms on hardware). For plain gameplay (map panel on, no menu, no big map)
// this hands the scan to a worker on core 2, which scans into one of its own
// two buffers while the game thread presents and runs the next tick. The
// bottom screen ends up one frame behind the top.
//
// The next tick can change engine B's registers (fades, the castle map toggle)
// and the ROM's vblank code republishes OAM and registers at the message
// compositor. So the worker reads registers from a copy of the I/O page taken
// at the hand-off, and the level loop joins right before the message
// compositor instead of at the top of the frame. The only unguarded case is
// VRAM written mid-tick while a scan runs (castle map toggle), which at worst
// mixes old and new tiles for one frame.
//
// The card reads the result straight from the worker's buffer
// (game3ds_sub_latest) instead of copying it into g_sub (192 KB, ~1.3 ms).
// The buffers alternate, so the one being presented is never the one being
// scanned. g_sub is only updated when something needs it
// (game3ds_sub_materialize).
//
// All other scan-outs (menus, big map, dumps) stay on the game thread and join
// first, since the scan-out's caches aren't thread-safe.
//
// At the game's own 60 Hz (title, file select, star select), when scans can't
// keep up, skip every other one: if this tick waited over kWaitMs at the join
// and is past kLateMs at the next hand-off, or the last scan took over
// kHalfMs, the frame after a scanned one starts no scan. The bottom screen then
// updates at 30 Hz there; the 3D is unaffected. The level loop (2 vblanks a
// tick) scans every tick. GAME3DS_SUB_HALF_OFF=1 scans every frame.
//
// On Old 3DS the worker runs on core 0 below the game thread. If the thread
// can't be created, or with GAME3DS_SUB_ASYNC_OFF=1, everything scans inline.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include "ntr/gx.h"
#include "ntr/ppu.h"
#include "ntr/rt.h"
#include "gpu3ds.h"
#include "sub_affine.h"
#include "card3ds.h"

extern "C" void __real__ZN3ntr15ppu_scanout_subERNS_14SubFramebufferE(ntr::SubFramebuffer &fb);
namespace ntr {
size_t ppu_sub_release_caches();   // ppu_sub.cpp (a gccfix rule): sub_scan_single.inl's
}

// Scans by path (counted in sub_scan_single.inl): one BG, several BGs, alpha
// mode, the general walk. A scan in none of them was skipped as unchanged (or
// the screen was blank).
extern "C" unsigned game3ds_sub_paths[4];
unsigned game3ds_sub_paths[4];

namespace {

// In linear memory when there is room, so the card's present can hand them
// to the GPU's transfer engine (gpu3ds::overlay_transfer).
ntr::SubFramebuffer *g_back;     // two of them, made with the worker
// The I/O page as it was at the hand-off. The worker reads registers from
// here, so the game thread can write next frame's registers meanwhile.
alignas(8) uint8_t g_io[0x2000];
// Engine B's per-line window edges at the hand-off (ntr::rt_window_rows), read
// by the worker the same way: the wipe's next lines are recorded meanwhile.
uint32_t g_rows[192];
bool g_rows_live;
int g_next;                      // the buffer the next job scans into
ntr::SubFramebuffer *g_job;       // the buffer the pending job scans into
ntr::SubFramebuffer *g_target;    // where the result belongs (g_sub)
// The newest finished scan-out when it is one of g_back (null: g_target holds
// it, an inline scan or nothing yet), and whether g_target still lacks it.
const ntr::SubFramebuffer *g_latest;
bool g_target_stale;
Thread g_thread;
LightEvent g_go, g_done;
volatile bool g_quit;
bool g_busy;                      // a job was handed over and not yet joined
int g_state = -1;                 // -1 unknown, 0 off, 1 on

// counters for the [perf] line, reset at each report
u64 g_work_ticks, g_wait_ticks, g_copy_ticks;
unsigned g_jobs, g_inline, g_compares, g_halved, g_still_busy;
unsigned g_from_capture;   // pictures shown from the card's capture, not scanned
unsigned g_latest_cap;     // g_latest is that capture's picture (its number), else 0
// the half rate's state: the last job's own time, whether the last frame
// scanned, and this tick's start (game3ds_sub_tick_begin) and wait at joins
constexpr u64 kHalfMs = 15, kWaitMs = 1, kLateMs = 13;
volatile u64 g_last_ticks;
bool g_scanned_last;
u64 g_tick_start, g_tick_wait;

// What each buffer holds, as a number, so the card only re-sends a picture
// when it changed. A skipped scan keeps its number; a drawn one gets the other
// buffer's number if it came out identical, otherwise a new one. 0 = unknown.
uint64_t g_id[2], g_id_next;
// The same for the buffer the inline scans write (g_sub): only a scan-out
// (game3ds_sub_scanned) or game3ds_sub_materialize writes it.
const void *g_inline_fb;
uint64_t g_inline_id;

// Old 3DS half rate: without core 2 the scan runs on the game thread, so in
// gameplay a scan right after a scanned frame is skipped and the bottom screen
// updates at half the tick rate. On screens that tick every vblank (the title)
// only when the tick is already late. GAME3DS_SUB_EVERY=1 scans every tick.
bool old_half()
{
    static int on = -1;
    if (on < 0) {
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        on = !n3ds && !getenv("GAME3DS_SUB_EVERY");
        if (on) printf("[sub-async] Old 3DS: engine B scanned every other tick (GAME3DS_SUB_EVERY=1: every tick)\n");
    }
    return on == 1;
}

// On Old 3DS, never wait for the worker. It runs below the game thread, so it
// only scans while the game thread waits, and a scan took ~37 ms on hardware,
// with the join waiting 17 ms a tick for it. Now the tick's join only collects
// a finished scan, and a new scan isn't started while the last is still
// running; the bottom screen just updates as often as idle time allows. Since
// the scan can then overlap a tick that writes OAM, palettes or VRAM, one frame
// may mix two frames' data. Other joins (inline scans, level changes, the end
// of the title) still wait. GAME3DS_SUB_WAIT=1 always waits.
bool no_wait()
{
    static int on = -1;
    if (on < 0) {
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        on = !n3ds && !getenv("GAME3DS_SUB_WAIT");
        if (on) printf("[sub-async] Old 3DS: the tick never waits for engine B's scan (GAME3DS_SUB_WAIT=1: it does)\n");
    }
    return on == 1;
}

// A handed-over job is finished: its buffer is the newest picture
void finish()
{
    g_latest = g_job;
    g_latest_cap = 0;
    g_target_stale = true;
    g_busy = false;
}

// The job's result when it is done, without waiting; false: still running
bool poll()
{
    if (!g_busy) return true;
    if (!LightEvent_TryWait(&g_done)) return false;
    finish();
    return true;
}

// Sky strips for each buffer (for the menu side bars): one for each g_back
// buffer and one for any other buffer an inline scan writes. Each belongs to
// the buffer whose scan made it. A scan that draws without making strips
// clears them; a skipped scan keeps them. In linear memory for the transfer
// engine. Only one scan runs at a time, so g_strip_made refers to the current
// one.
struct Strips {
    const void *fb;   // the buffer they are beside
    uint32_t *px;     // 64 x 192
    bool ok;
};
Strips g_strips[3];
const void *g_strip_made;   // the buffer the running scan made strips for
long g_hits_before;         // ppu_sub_scan_hits before the running inline scan

Strips &strips_for(const void *fb)
{
    return g_back && fb == &g_back[0] ? g_strips[0] : g_back && fb == &g_back[1] ? g_strips[1] : g_strips[2];
}

// Affine BG descriptor for each g_back buffer (the painter left the BG to the
// GPU), tracked like the strips. It keeps its own copy of the BG picture,
// since the painter's may be remade by the next scan while the card is still
// reading this one. Inline scans don't have one.
struct AffSlot {
    SubAffine d;
    uint32_t *pic;
    size_t cap;    // pixels
    unsigned ver;  // the number pic was copied at
};
AffSlot g_affs[2];
const void *g_aff_made;   // the buffer the running scan described

// Engine B's whole-screen fade, done at present time (for the smooth fades in
// card3ds.cpp). A brightness fade (BLDCNT mode 2 or 3) on every layer and the
// backdrop with no windows is removed from the worker's register copy, so the
// scan draws the picture unfaded (and the cache can reuse it through the
// fade), and the level is noted per buffer for the card (game3ds_sub_fade_of).
// A picture read back as pixels gets the fade applied on the CPU
// (game3ds_sub_materialize). GAME3DS_FADE_SMOOTH=0 lets the scan fade as
// before.
struct FadeSlot {
    int evy;      // 0..16; 0 none
    bool white;   // brightness up (mode 2), else down
};
FadeSlot g_fades[2];

FadeSlot *fade_for(const void *fb)
{
    return g_back && fb == &g_back[0] ? &g_fades[0] : g_back && fb == &g_back[1] ? &g_fades[1] : nullptr;
}

// the DS's own arithmetic on the 5-bit channels (ppu_sub.cpp's BLDY modes)
void fade_fill(ntr::SubFramebuffer &fb, const FadeSlot &f)
{
    for (int y = 0; y < ntr::SUB_H; ++y)
        for (int x = 0; x < ntr::SUB_W; ++x) {
            const uint32_t p = fb.px[y][x];
            uint32_t out = p & 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                int c = ((p >> sh) & 0xFF) >> 3;
                c = f.white ? c + (((31 - c) * f.evy) >> 4) : c - ((c * f.evy) >> 4);
                out |= (uint32_t)(c << 3 | c >> 2) << sh;
            }
            fb.px[y][x] = out;
        }
}

bool g_dry;          // game3ds_sub_set_dry
unsigned g_dry_n;    // scan-outs a dry frame left out, since the last report

AffSlot *aff_for(const void *fb)
{
    return g_back && fb == &g_back[0] ? &g_affs[0] : g_back && fb == &g_back[1] ? &g_affs[1] : nullptr;
}

// Fill in the BG under the sprites on the CPU (same math as
// sub_scan_single.inl's aff_line): where fb is 0, the BG pixel through its
// matrix, otherwise the backdrop.
void affine_fill(ntr::SubFramebuffer &fb, const SubAffine &d)
{
    const unsigned w = (unsigned)d.map_w, h = (unsigned)d.map_h;
    const int sh = __builtin_ctz(w);
    for (int y = 0; y < ntr::SUB_H; ++y) {
        int sx = d.refx + d.pb * y, sy = d.refy + d.pd * y;
        for (int x = 0; x < ntr::SUB_W; ++x, sx += d.pa, sy += d.pc) {
            if (fb.px[y][x]) continue;
            unsigned px = (unsigned)(sx >> 8), py = (unsigned)(sy >> 8);
            if (d.wrap) {
                px &= w - 1;
                py &= h - 1;
            } else if (px >= w || py >= h) {
                fb.px[y][x] = d.backdrop;
                continue;
            }
            const uint32_t v = d.pic[(py << sh) + px];
            fb.px[y][x] = v ? v : d.backdrop;
        }
    }
}

// After a scan of fb: drawn without strips, it has none
void strips_after(const void *fb, bool drawn)
{
    Strips &s = strips_for(fb);
    if (drawn && g_strip_made != fb && s.fb == fb) s.ok = false;
    g_strip_made = nullptr;
    // and drawn by the CPU, no affine BG beside it
    if (AffSlot *a = aff_for(fb))
        if (drawn && g_aff_made != fb) a->d.on = 0;
    g_aff_made = nullptr;
}

void worker(void *)
{
    ntr::ppu_sub_io_snapshot(g_io);   // this thread's register reads, for good
    ntr::ppu_sub_rows_snapshot(g_rows, &g_rows_live);   // and engine B's window edges per line
    for (;;) {
        LightEvent_Wait(&g_go);
        if (g_quit) break;
        const u64 t0 = svcGetSystemTick();
        const long hits = ntr::ppu_sub_scan_hits();
        g_strip_made = nullptr;
        __real__ZN3ntr15ppu_scanout_subERNS_14SubFramebufferE(*g_job);
        strips_after(g_job, ntr::ppu_sub_scan_hits() == hits);
        const int k = g_job == &g_back[1];
        if (ntr::ppu_sub_scan_hits() == hits || !g_id[k]) {
            ++g_compares;
            g_id[k] = g_id[k ^ 1] && memcmp(g_job->px, g_back[k ^ 1].px, sizeof g_job->px) == 0
                          ? g_id[k ^ 1]
                          : ++g_id_next;
        }
        g_last_ticks = svcGetSystemTick() - t0;
        g_work_ticks += g_last_ticks;
        LightEvent_Signal(&g_done);
    }
}

bool enabled()
{
    if (g_state >= 0) return g_state == 1;
    g_state = 0;
    if (getenv("GAME3DS_SUB_ASYNC_OFF")) {
        printf("[sub-async] off (GAME3DS_SUB_ASYNC_OFF)\n");
        return false;
    }
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    if (!n3ds && getenv("GAME3DS_SUB_CORE0_OFF")) {
        printf("[sub-async] off (an Old 3DS, GAME3DS_SUB_CORE0_OFF)\n");
        return false;
    }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    LightEvent_Init(&g_go, RESET_ONESHOT);
    LightEvent_Init(&g_done, RESET_ONESHOT);
    if (void *m = linearAlloc(2 * sizeof(ntr::SubFramebuffer)))
        g_back = static_cast<ntr::SubFramebuffer *>(m);
    else
        g_back = new ntr::SubFramebuffer[2];
    // One priority below the game thread, the lowest on core 2: the audio mix is
    // one above, the present worker at the same level. The present has a vblank to
    // hit, and this result isn't needed until the next tick's compositor, so it
    // runs while the present waits. On Old 3DS the worker goes on core 0 below the
    // game thread and scans while it waits (mostly for the vblank).
    const int core = n3ds ? 2 : 0;
    g_thread = threadCreate(worker, nullptr, 64 * 1024, prio + 1, core, false);
    if (!g_thread) {
        printf("[sub-async] off (core %d thread refused)\n", core);
        return false;
    }
    g_state = 1;
    printf("[sub-async] on: engine B scan-out on core %d%s, one frame behind the top screen\n",
           core, n3ds ? "" : " below the game thread");
    return true;
}

}  // namespace

void game3ds_sub_tick_begin(void)
{
    g_tick_start = svcGetSystemTick();
    g_tick_wait = 0;
}

void game3ds_sub_join(void)
{
    if (!g_busy) return;
    const u64 t0 = svcGetSystemTick();
    LightEvent_Wait(&g_done);
    const u64 waited = svcGetSystemTick() - t0;
    g_wait_ticks += waited;
    g_tick_wait += waited;
    finish();
}

void game3ds_sub_join_tick(void)
{
    if (g_state == 1 && no_wait())
        poll();
    else
        game3ds_sub_join();
}

void game3ds_sub_release(void)
{
    game3ds_sub_join();
    const size_t bytes = ntr::ppu_sub_release_caches();
    printf("[sub-async] engine B's scan caches released: %u KB\n", (unsigned)(bytes >> 10));
}

// Neither of these joins. A job handed over this frame is shown next frame;
// the newest finished one is last frame's (and isn't the buffer being written).
const uint32_t *game3ds_sub_latest(void)
{
    return g_latest ? &g_latest->px[0][0] : nullptr;
}

// game3ds_sub_latest's picture as a number (g_id): two pictures with one
// number are the same picture; 0 when there is none or it is not known.
unsigned long long game3ds_sub_latest_sig(void)
{
    if (!g_latest || (g_latest != &g_back[0] && g_latest != &g_back[1])) return 0;
    return g_id[g_latest == &g_back[1]];
}

// The number of `picture` when it is the newest one (null latest: an inline
// scan's buffer), else 0.
unsigned long long game3ds_sub_sig_of(const uint32_t *picture)
{
    if (!picture) return 0;
    if (g_latest) return picture == &g_latest->px[0][0] ? game3ds_sub_latest_sig() : 0;
    return g_inline_fb && picture == &static_cast<const ntr::SubFramebuffer *>(g_inline_fb)->px[0][0]
               ? g_inline_id
               : 0;
}

void game3ds_sub_materialize(void)
{
    if (!g_target_stale || !g_latest || !g_target) return;
    if (g_latest_cap) ntr::gx_card_capture_fence(g_latest_cap);   // its job may still be writing it
    const u64 t0 = svcGetSystemTick();
    memcpy(g_target, g_latest, sizeof *g_target);
    // A sprites-only picture (BG left to the GPU) gets its BG filled in here, so a
    // frame composed by the HAL reads the right pixels.
    if (const AffSlot *a = aff_for(g_latest))
        if (a->d.on) affine_fill(*g_target, a->d);
    if (const FadeSlot *f = fade_for(g_latest))
        if (f->evy > 0) fade_fill(*g_target, *f);
    if (g_target == g_inline_fb) g_inline_id = 0;
    g_copy_ticks += svcGetSystemTick() - t0;
    g_target_stale = false;
}

// An inline scan just wrote `fb`. If that's g_sub it's now the newest picture,
// and an older async one must not be shown or copied over it.
void game3ds_sub_scan_before(void)
{
    g_hits_before = ntr::ppu_sub_scan_hits();
    g_strip_made = nullptr;
}

void game3ds_sub_scanned(const void *fb)
{
    const bool drawn = ntr::ppu_sub_scan_hits() == g_hits_before;
    strips_after(fb, drawn);
    // handed back by the unchanged-frame skip: the buffer and its number stay
    if (fb != g_inline_fb || drawn || !g_inline_id) g_inline_id = ++g_id_next;
    g_inline_fb = fb;
    if (g_target && fb != g_target) return;
    g_latest = nullptr;
    g_target_stale = false;
}

// The painter's two calls (sub_scan_single.inl sky_strips): get the strip
// buffer for fb (null if there's no memory), then say whether they're good.
extern "C" uint32_t *game3ds_sub_strips_begin(const void *fb)
{
    Strips &s = strips_for(fb);
    if (!s.px) {
        static bool tried[3];
        bool &t = tried[&s - g_strips];
        if (t) return nullptr;
        t = true;
        s.px = static_cast<uint32_t *>(linearAlloc(64 * 192 * 4));
        if (!s.px) return nullptr;
    }
    s.fb = fb;
    s.ok = false;
    g_strip_made = fb;
    return s.px;
}

extern "C" void game3ds_sub_strips_done(const void *fb, int ok)
{
    Strips &s = strips_for(fb);
    if (s.fb == fb) s.ok = ok != 0;
}

extern "C" SubAffine *game3ds_sub_affine_begin(const void *fb)
{
    AffSlot *a = aff_for(fb);
    if (!a) return nullptr;
    a->d.on = 0;
    return &a->d;
}

extern "C" void game3ds_sub_affine_done(const void *fb, int ok)
{
    AffSlot *a = aff_for(fb);
    if (!a) return;
    a->d.on = 0;
    if (!ok || !a->d.pic) return;
    const size_t px = (size_t)a->d.map_w * a->d.map_h;
    if (!a->pic || a->ver != a->d.ver || px > a->cap) {
        if (px > a->cap) {
            free(a->pic);
            a->pic = static_cast<uint32_t *>(malloc(px * 4));
            a->cap = a->pic ? px : 0;
            if (!a->pic) return;
        }
        memcpy(a->pic, a->d.pic, px * 4);
        a->ver = a->d.ver;
    }
    a->d.pic = a->pic;
    a->d.on = 1;
    g_aff_made = fb;
}

// The whole-screen fade left out of `picture` (one of the worker's buffers):
// BLDY's evy, 0 none, and its direction.
int game3ds_sub_fade_of(const uint32_t *picture, int *white)
{
    *white = 0;
    for (int i = 0; i < 2; ++i)
        if (g_back && picture == &g_back[i].px[0][0]) {
            *white = g_fades[i].white;
            return g_fades[i].evy;
        }
    return 0;
}

const SubAffine *game3ds_sub_affine_of(const uint32_t *picture)
{
    for (int i = 0; i < 2; ++i)
        if (g_back && g_affs[i].d.on && picture == &g_back[i].px[0][0]) return &g_affs[i].d;
    return nullptr;
}

const uint32_t *game3ds_sub_strips(const uint32_t *picture)
{
    for (const Strips &s : g_strips)
        if (s.ok && s.fb && picture == &static_cast<const ntr::SubFramebuffer *>(s.fb)->px[0][0])
            return s.px;
    return nullptr;
}

// hal/sub_screen.cpp's present scan-out (via a gccfix rule). async_ok means
// plain gameplay; anything else, or async off, scans inline. C linkage because
// the call site is in an extern "C" block.
extern "C" void game3ds_sub_scanout(ntr::SubFramebuffer &fb, int async_ok)
{
    // a dry frame (game3ds_sub_set_dry): its picture is not shown, so none is
    // made; the one shown stays, as when the last scan is still running
    if (g_dry) {
        ++g_dry_n;
        return;
    }
    if (async_ok && g_state == 1 && no_wait() && !poll()) {
        ++g_still_busy;   // the last scan still running: its picture comes later
        return;
    }
    game3ds_sub_join();
    if (!async_ok || !enabled()) {
        // a screen ticking once a vblank (the title's star) only when late
        constexpr u64 ms = SYSCLOCK_ARM11 / 1000;
        if (async_ok && old_half() && g_scanned_last && g_inline_id && g_inline_fb == &fb &&
            (gpu3ds::vblanks_per_frame() > 1 || svcGetSystemTick() - g_tick_start > kLateMs * ms)) {
            g_scanned_last = false;
            ++g_halved;
            return;
        }
        g_scanned_last = async_ok;   // a menu's scan-out is never left standing
        ++g_inline;
        ntr::ppu_scanout_sub(fb);   // the wrapped, timed call (game3ds_sub_scanned)
        return;
    }
    g_target = &fb;
    // ENGINE B SHOWING THE DISPLAY CAPTURE (the dual-screen minigames' other
    // screen: 1:1, a BG bitmap or bitmap sprites tiling it): its picture is the
    // capture this frame's publish carried there, which the card kept as it
    // wrote it, so it is shown from there. No scan, and this frame's picture: a
    // scan is shown a frame later, by when the two screens have swapped, so
    // each screen showed the other's half every other frame.
    // GAME3DS_SUB_CAPTURE_OFF=1 scans it as before.
    {
        static const bool off = getenv("GAME3DS_SUB_CAPTURE_OFF") != nullptr;
        uint32_t at = 0;
        unsigned seq = 0;
        if (!off && ntr::ppu_sub_bitmap_view(&at) &&
            ntr::ppu_capture_shown(at, ntr::SUB_W * ntr::SUB_H * 2u, &seq))
            if (const ntr::SubFramebuffer *pic = card3ds_capture_picture(seq)) {
                // GAME3DS_SUB_CAPTURE_CHECK=1: scan it anyway and count the
                // pixels the two pictures disagree on
                static const bool check = getenv("GAME3DS_SUB_CAPTURE_CHECK") != nullptr;
                if (check) {
                    static ntr::SubFramebuffer *scan;
                    static unsigned frames, bad, badpx;
                    if (!scan) scan = new ntr::SubFramebuffer;
                    ntr::gx_card_capture_fence(seq);
                    __real__ZN3ntr15ppu_scanout_subERNS_14SubFramebufferE(*scan);
                    unsigned n = 0;
                    for (int y = 0; y < ntr::SUB_H; ++y)
                        for (int x = 0; x < ntr::SUB_W; ++x) n += scan->px[y][x] != pic->px[y][x];
                    badpx += n;
                    bad += n != 0;
                    if (++frames % 120 == 0) {
                        printf("[sub-capture] %u pictures from the capture: %u differ from the scan (%u pixels)\n",
                               frames, bad, badpx);
                        bad = badpx = 0;
                    }
                }
                g_latest = pic;
                g_latest_cap = seq;
                g_target_stale = true;
                g_scanned_last = true;
                ++g_from_capture;
                return;
            }
    }
    static const bool half_off = getenv("GAME3DS_SUB_HALF_OFF") != nullptr;
    constexpr u64 ms = SYSCLOCK_ARM11 / 1000;
    const bool late = g_tick_wait > kWaitMs * ms && svcGetSystemTick() - g_tick_start > kLateMs * ms;
    // an Old 3DS: every other tick in the level loop; at one vblank only when
    // late (its worker's clock counts the game thread's turns too)
    const bool skip = gpu3ds::vblanks_per_frame() == 1
                          ? late || (!old_half() && g_last_ticks > kHalfMs * ms)
                          : old_half();
    if (!half_off && !no_wait() && g_latest && g_scanned_last && skip) {
        g_scanned_last = false;
        ++g_halved;
        return;
    }
    g_scanned_last = true;
    ntr::ppu_vram_settle();   // here, not on core 2 in the scan
    memcpy(g_io, (const void *)0x04000000, sizeof g_io);
    {   // the wipe's per-line window edges too: the next tick records its own
        const uint32_t *rows = nullptr;
        g_rows_live = ntr::rt_window_rows(1, &rows);
        if (g_rows_live) memcpy(g_rows, rows, sizeof g_rows);
    }
    {   // GAME3DS_SUB_TRACE: engine B's configuration every 120 jobs, to see
        // which scan-out path a slow screen takes
        static int on = -1;
        if (on < 0) on = getenv("GAME3DS_SUB_TRACE") ? 1 : 0;
        if (on && g_jobs % 120 == 0) {
            auto r16 = [](unsigned off) { return *(const uint16_t *)(g_io + off); };
            printf("[subtrace] dispcnt %08lx bgcnt %04x %04x %04x %04x bld %04x alpha %04x "
                   "y %04x bright %04x winin %04x winout %04x\n",
                   (unsigned long)*(const uint32_t *)(g_io + 0x1000), r16(0x1008), r16(0x100A),
                   r16(0x100C), r16(0x100E), r16(0x1050), r16(0x1052), r16(0x1054),
                   r16(0x106C), r16(0x1048), r16(0x104A));
            // engine B's sprites (OAM at 0x07000400): shown, affine, double
            // size, and their area in pixels
            static const int kSizes[3][4][2] = {{{8, 8}, {16, 16}, {32, 32}, {64, 64}},
                                                {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
                                                {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};
            const uint16_t *oam = (const uint16_t *)0x07000400;
            unsigned shown = 0, aff = 0, dbl = 0, area = 0, aff_area = 0;
            for (int i = 0; i < 128; ++i) {
                const uint16_t a0 = oam[i * 4], a1 = oam[i * 4 + 1];
                const bool affine = a0 & 0x100;
                if (!affine && (a0 & 0x200)) continue;
                const int shape = (a0 >> 14) & 3;
                if (shape == 3) continue;
                const int size = (a1 >> 14) & 3;
                unsigned px = kSizes[shape][size][0] * kSizes[shape][size][1];
                ++shown;
                if (affine) {
                    ++aff;
                    if (a0 & 0x200) { ++dbl; px *= 4; }
                    aff_area += px;
                }
                area += px;
            }
            printf("[subtrace] obj: %u shown, %u affine (%u double), %u px (%u affine)\n", shown,
                   aff, dbl, area, aff_area);
        }
    }
    {   // engine B's whole-screen fade, left to the present (FadeSlot)
        static const bool smooth = !(getenv("GAME3DS_FADE_SMOOTH") && getenv("GAME3DS_FADE_SMOOTH")[0] == '0');
        uint16_t *cnt = (uint16_t *)(g_io + 0x1050), *bldy = (uint16_t *)(g_io + 0x1054);
        const int mode = (*cnt >> 6) & 3;
        const bool windows = (*(const uint32_t *)(g_io + 0x1000) & 0xE000u) != 0;
        FadeSlot &f = g_fades[g_next];
        f.evy = 0;
        f.white = mode == 2;
        if (smooth && (mode == 2 || mode == 3) && (*cnt & 0x3F) == 0x3F && !windows) {
            f.evy = *bldy & 0x1F;
            if (f.evy > 16) f.evy = 16;
            *cnt &= (uint16_t)~0xC0u;
            *bldy = 0;
        }
    }
    g_job = &g_back[g_next];
    g_next ^= 1;
    g_busy = true;
    ++g_jobs;
    LightEvent_Signal(&g_go);
}

// `ms` converts a tick total to ms per frame (phase_timers_report's).
void game3ds_sub_set_dry(bool dry)
{
    g_dry = dry;
}

void game3ds_sub_async_report(double ms)
{
    if (g_dry_n) printf("[perf] sub_async: %u scan-outs left out by dry frames\n", g_dry_n);
    g_dry_n = 0;
    if (g_state != 1) {
        if (g_halved) printf("[perf] sub_async: inline %u, left standing (Old 3DS half rate) %u\n", g_inline, g_halved);
        g_inline = g_halved = 0;
        return;
    }
    printf("[perf] sub_async: jobs %u inline %u worker %.3f wait %.3f copy %.3f ms/frame; "
           "drawn and compared %u; frames left at the last scan (half rate) %u, (still scanning) %u\n",
           g_jobs, g_inline, g_work_ticks * ms, g_wait_ticks * ms, g_copy_ticks * ms, g_compares,
           g_halved, g_still_busy);
    if (g_from_capture)
        printf("[perf] sub_async: %u pictures shown from the display capture\n", g_from_capture);
    g_from_capture = 0;
    g_work_ticks = g_wait_ticks = g_copy_ticks = 0;
    g_jobs = g_inline = g_compares = g_halved = g_still_busy = 0;
    printf("[perf] sub_paths: single %u multi %u alpha %u walk %u (the rest unchanged or blank)\n",
           game3ds_sub_paths[0], game3ds_sub_paths[1], game3ds_sub_paths[2], game3ds_sub_paths[3]);
    memset(game3ds_sub_paths, 0, sizeof game3ds_sub_paths);
}
