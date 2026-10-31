#include "card3ds.h"

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "hal/gpu_raster.h"
#include "ntr/gx.h"
#include "ntr/ppu.h"

#include "gpu3ds.h"
#include "interp3ds.h"
#include "sub_affine.h"

extern "C" unsigned char data_0209d660;   // the ROM's: nonzero while a text box is open
extern "C" int port_scene_live_id(void);   // hal/scene_boot.cpp: the scene path's id, or -1

// The watchdog's view (game/watchdog3ds.cpp): what the game thread waits for
// here, and where the present worker is
extern "C" const char *volatile g_card3ds_wait = nullptr;
extern "C" const char *volatile g_card3ds_worker = "not started";

namespace {

// The frame the card is holding. gx_render's triangle list stays valid until
// the next gx_reset (the start of the next frame), so the present can draw
// straight from it.
struct Held {
    bool active;
    const ntr::GxTriangle *tris;
    size_t count;
    uint32_t clear_argb;
    const uint32_t *pic;   // the frame read back by a settle, else null
    int pic_stride;
    int toon_shade;        // the frame's mode-2 table (GxGpuFrame), copied
    float toon_rgb[32 * 3];
    bool fog_on;           // the frame's fog (GxGpuFrame::fog), copied
    ntr::GxFog fog;
    bool wdepth;
} g_held;

int g_fade_evy;
bool g_fade_white;
// The brightness mask the fade is limited by (port_gpu_card_set_mask), valid
// until the next frame's compositor; null = the whole picture.
const unsigned char *g_mask;
int g_mask_stride;
bool g_mask_shown3d, g_mask_clear_opaque;
bool g_up;

// The HAL's menu layer (hal/gpu_raster.h): drawn into two premultiplied planes
// and laid over the composite before or after the fade. One tick only.
struct Layer {
    unsigned px[2][ntr::SCREEN_W * ntr::SCREEN_H];
    int stride, w, h;
    bool live, two, post;
} g_lay;

struct Counts {
    unsigned long kept, refused, settles, presents_kept, presents_cpu, layers;
} g_n;
// Kept frames presented: 0 without a record, 1 with one gpu3ds couldn't draw,
// 2 drawn from the record. Printed every 120.
volatile unsigned g_xform_n[3];

void async_init();       // the core 2 present, below
void async_shutdown();

// ---- ntr/gx.cpp backend ----
//
// The card only takes a whole frame (answer 2, out->kept) when gx_clear_fill
// offered it that way. Everything else is refused (0) and gx.cpp's CPU path
// does the work, which is always correct, just slower:
//   probe (can you draw whole frames?)   yes, but only kept frames are taken
//   settle 1 / 2 (read the picture back) yes, see settle(); 3 no
//   planes (depth, polygon IDs)          no
//   smoothed grids (PC SmoothModels)     no
inline uint32_t combine(uint32_t ov, uint32_t base, bool cov)
{
    const uint32_t mode = ov >> 24;
    if (mode == GX_OV_FRONT) return 0xFF000000u | (ov & 0x00FFFFFFu);
    if (mode == GX_OV_BEHIND) return cov ? base : (0xFF000000u | (ov & 0x00FFFFFFu));
    if (ntr::gx_ov_is_blend(ov)) return ntr::gx_ov_blend_apply(ov, base);
    return base;
}

// ... and its cp_fade.
inline uint32_t fade(uint32_t p, int evy, bool white)
{
    int r = (int)((p >> 16) & 0xFFu), g = (int)((p >> 8) & 0xFFu), b = (int)(p & 0xFFu);
    if (white) {
        r += ((255 - r) * evy) >> 4;
        g += ((255 - g) * evy) >> 4;
        b += ((255 - b) * evy) >> 4;
    } else {
        r -= (r * evy) >> 4;
        g -= (g * evy) >> 4;
        b -= (b * evy) >> 4;
    }
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Settle: something on the CPU needs the held frame's pixels (e.g. the title
// menu cuts pieces out of the 3D). The GPU draws the held triangles offscreen,
// reads them back, and they're resolved under the 2D overlay like
// gpu_raster.cpp's cp_resolve.
//   1  fb becomes the finished frame (with the fade), cover is filled, sink3d
//      gets the 3D alone if asked
//   2  pre gets the picture display capture reads (no fade), fb is kept
// A held frame is read back at most once.
unsigned long long g_settle_ticks[2];   // readback (GPU + wait + copy), resolve

// THE DEFERRED CAPTURE (ntr::gx_card_capture_defer). A dual-screen minigame
// captures its 3D every tick, and making that picture here (the record drawn
// again, read back, resolved under the 2D and copied into VRAM) was ~12 ms of
// the game thread's tick. Instead the capture is taken (g_cap_next), handed to
// the frame's job at the hand-off, drawn by the present beside the screen it
// draws anyway (gpu3ds::capture_queue) and written into VRAM on core 2
// (cap_write); ppu's fence (capture_fence) waits for it before anything reads
// the block. GAME3DS_CAPTURE_DEFER_OFF=1 captures here as before.
struct CapJob {
    bool on;
    uint16_t *dst;
    int x, y, w, h;
    unsigned seq;   // ppu's number for it (ntr::ppu_capture_shown)
};
CapJob g_cap_next;
int capture_defer(const ntr::GxGpuFrame *f);   // below, with the present

int settle(const ntr::GxGpuFrame *f)
{
    ++g_n.settles;
    if (f->settle == 2 && f->defer) return capture_defer(f);
    if (f->settle != 1 && f->settle != 2) return 0;
    if (!g_held.active || !f->fb || (f->settle == 2 && !f->pre)) return 0;
    // gx.cpp hands the held frame's triangles over again: a lean frame (step
    // 5.2b) has none until a settle asks, and makes them then
    if (f->tris || f->count) {
        g_held.tris = f->tris;
        g_held.count = f->count;
    }
    const int stride = f->stride;
    const int cw = f->cw < ntr::SCREEN_W ? f->cw : ntr::SCREEN_W;
    const int ch = f->ch < ntr::SCREEN_H ? f->ch : ntr::SCREEN_H;
    const int rx0 = f->px0, ry0 = f->py0, rx1 = f->px0 + f->pw, ry1 = f->py0 + f->ph;
    const bool one = f->settle == 1;
    const int evy = one ? g_fade_evy : 0;
    // The capture's picture (2) is read only inside the DS picture's rectangle
    // (ppu_display_capture), so only that is made, and only that much of the
    // picture is read back: a native scene's pillarbox was a third of the work.
    const int y0 = one ? 0 : (ry0 > 0 ? ry0 : 0), y1 = one ? ch : (ry1 < ch ? ry1 : ch);
    const int x0 = one ? 0 : (rx0 > 0 ? rx0 : 0), x1 = one ? cw : (rx1 < cw ? rx1 : cw);
    card3ds_wait_idle();   // the readback is citro3d's, and so may the worker be
    const u64 t0 = svcGetSystemTick();
    if (!g_held.pic) {
        gpu3ds::set_fog(g_held.fog_on ? &g_held.fog : nullptr, g_held.wdepth);
        if (f->from_record) {
            // drawn from a copy of the record, as the present will draw it (any
            // failure: gx.cpp makes the triangles and asks again)
            static ntr::GxRecord rec;
            static const bool off = getenv("GAME3DS_CAPTURE_REC_OFF") != nullptr;
            if (off || !ntr::gx_copy_record(rec) ||
                !gpu3ds::record_fits(rec.v.size(), rec.idx.size() / 3))
                return 0;
            g_held.pic = gpu3ds::readback_record(rec, &g_held.pic_stride, x0, y0, x1, y1);
        } else {
            g_held.pic = gpu3ds::readback(g_held.tris, g_held.count, &g_held.pic_stride,
                                          x0, y0, x1, y1);
        }
        if (!g_held.pic) return 0;
    } else {
        gpu3ds::readback_area(x0, y0, x1, y1);   // a settle 1 after a capture's
    }
    g_settle_ticks[0] += svcGetSystemTick() - t0;
    const u64 t1 = svcGetSystemTick();
    // The capture's picture: every pixel inside the DS picture's rectangle, no
    // fade, no planes. Most are no 2D at all, the 3D (or the clear colour) as is.
    if (!one) {
        const uint32_t clear = g_held.clear_argb;
        for (int y = y0; y < y1; ++y) {
            const uint32_t *pic = g_held.pic + (size_t)y * g_held.pic_stride;
            const uint32_t *row = f->fb + (size_t)y * stride;
            uint32_t *dst = f->pre + (size_t)y * stride;
            for (int x = x0; x < x1; ++x) {
                const uint32_t c = pic[x], ov = row[x];
                const uint32_t base = (c >> 24) ? (0xFF000000u | c) : clear;
                dst[x] = (ov >> 24) == GX_OV_NONE ? base : combine(ov, base, (c >> 24) != 0);
            }
        }
        g_settle_ticks[1] += svcGetSystemTick() - t1;
        return 1;
    }
    for (int y = y0; y < y1; ++y) {
        const uint32_t *pic = g_held.pic + (size_t)y * g_held.pic_stride;
        uint32_t *row = f->fb + (size_t)y * stride;
        uint32_t *dst = one ? row : f->pre + (size_t)y * stride;
        const bool inr_y = y >= ry0 && y < ry1;
        for (int x = x0; x < x1; ++x) {
            const bool cov = (pic[x] >> 24) != 0;
            uint32_t base = (inr_y && x >= rx0 && x < rx1) ? g_held.clear_argb
                                                           : f->outside_argb;
            if (cov) base = 0xFF000000u | (pic[x] & 0x00FFFFFFu);
            uint32_t p = combine(row[x], base, cov);
            if (evy) p = fade(p, evy, g_fade_white);
            if (one) {
                if (f->cover) f->cover[(size_t)y * stride + x] = cov ? 1 : 0;
                if (f->sink3d) f->sink3d[(size_t)y * ntr::SCREEN_W + x] = base;
            }
            dst[x] = p;
        }
    }
    g_settle_ticks[1] += svcGetSystemTick() - t1;
    if (one) {
        if (f->out) f->out->pre_valid = 0;
        g_held.active = false;   // an ordinary finished frame from here on
        g_fade_evy = 0;
    }
    return 1;
}

int backend(const ntr::GxGpuFrame *f)
{
    if (f->settle) return settle(f);
    if (f->planes) return 0;
    if (f->probe) {
        if (f->probe_grids) return 0;    // smoothed grids: not taken
        return f->keep ? 3 : 2;
    }
    if (!f->keep || f->full_ok != 2 || f->ngrids) { ++g_n.refused; return 0; }
    g_cap_next.on = false;   // (one never handed to a job went with its frame)
    g_held.active = true;
    g_held.tris = f->tris;
    g_held.count = f->count;
    g_held.clear_argb = f->clear_argb;
    g_held.pic = nullptr;
    g_held.toon_shade = f->toon_rgb ? f->toon_shade : 0;
    if (g_held.toon_shade)
        memcpy(g_held.toon_rgb, f->toon_rgb, sizeof g_held.toon_rgb);
    g_held.fog_on = f->fog != nullptr;
    if (g_held.fog_on) g_held.fog = *f->fog;
    g_held.wdepth = f->wdepth != 0;
    if (f->out) {
        memset(f->out, 0, sizeof *f->out);
        f->out->kept = 1;
    }
    ++g_n.kept;
    return 2;
}

// The clear colour, fogged as the DS fogs its rear plane: CLEAR_COLOR bit 15
// and CLEAR_DEPTH's density (gx.cpp's fog_pass does the same per pixel).
uint32_t rear_plane(const Held &h)
{
    const uint32_t c = h.clear_argb;
    if (!h.fog_on || !h.fog.rear_fog || h.fog.alpha_only) return c;
    const int den = ntr::gx_fog_density(h.fog, h.fog.rear_depth);
    const uint32_t fc = h.fog.color_argb;
    auto mix = [&](int sh) {
        return (uint32_t)(((int)(fc >> sh & 0xFF) * den + (int)(c >> sh & 0xFF) * (128 - den)) >> 7)
               << sh;
    };
    return (c & 0xFF000000u) | mix(16) | mix(8) | mix(0);
}

gpu3ds::Viewport viewport_for(gpu3ds::Screen s, int w, int h)
{
    // Fill the screen's height, keep the picture's aspect, centre it.
    int sw, sh;
    gpu3ds::screen_size(s, &sw, &sh);
    gpu3ds::Viewport vp;
    vp.h = (float)sh;
    vp.w = (float)sh * (float)w / (float)h;
    if (vp.w > (float)sw) { vp.w = (float)sw; vp.h = (float)sw * (float)h / (float)w; }
    vp.x = ((float)sw - vp.w) * 0.5f;
    vp.y = ((float)sh - vp.h) * 0.5f;
    return vp;
}

}  // namespace

bool card3ds_init()
{
    if (g_up) return true;
    if (!gpu3ds::init()) return false;
    ntr::gx_set_gpu_opaque(backend);
    // A semi-transparent sprite over kept 3D is blended at present time instead of
    // reading the picture back (the cannon's aiming view). GAME3DS_OV_BLEND=0 uses
    // the readback.
    {
        const char *e = getenv("GAME3DS_OV_BLEND");
        ntr::gx_card_set_blend_ok(!(e && e[0] == '0'));
    }
    // the geometry stream's hash is the PC harness's (gx_stream_hash_reset);
    // nothing here reads it, and it cost a multiply a command word
    ntr::gx_stream_hash_enable(0);
    // GAME3DS_GPU_XFORM=1 draws kept frames from gx.cpp's record, and gx.cpp only
    // records (lean frames). GAME3DS_GPU_XFORM_LIST=1 also keeps the triangles.
    if (getenv("GAME3DS_GPU_XFORM")) {
        const bool lean = getenv("GAME3DS_GPU_XFORM_LIST") == nullptr;
        ntr::gx_record_configure(lean ? 1 : 0);
        // the interpolator's per-id hash and sums, kept as the record is made
        // (see the recorder's stats in interp3ds.cpp)
        ntr::gx_record_id_stats(interp3ds_wanted() ? 1 : 0);
        printf("[gpu-xform] on: kept frames drawn from the record%s\n",
               lean ? ", the CPU only records them" : ", the triangles still made beside it");
        // Replay display lists already seen into the record (the list memo).
        // GAME3DS_LIST_MEMO=0 decodes every one, =2 does both and compares.
        const char *lm = getenv("GAME3DS_LIST_MEMO");
        const int lm_mode = lean ? (lm ? atoi(lm) : 1) : 0;
        ntr::gx_list_memo(lm_mode);
        printf("[list-memo] %s\n", lm_mode == 2 ? "verify" : lm_mode ? "on" : "off");
    }
    async_init();
    g_up = true;
    return true;
}

void card3ds_shutdown()
{
    if (!g_up) return;
    async_shutdown();
    ntr::gx_set_gpu_opaque(nullptr);
    gpu3ds::shutdown();
    printf("card3ds: %lu kept, %lu refused, %lu settles, presents %lu kept / %lu cpu, "
           "%lu menu layers\n",
           g_n.kept, g_n.refused, g_n.settles, g_n.presents_kept, g_n.presents_cpu,
           g_n.layers);
    g_up = false;
}

// ---- the present ----
//
// Everything one present needs, taken at card3ds_present. A sync present draws
// it right away; the level loop hands it to a worker thread and moves on to the
// next tick (see "on core 2" below).
namespace {

struct Job {
    const uint32_t *fb;          // engine A's picture (fb.px), SCREEN_W a row
    int w, h;                    // ntr::active_w/h at the handoff
    int px, pw, ph;              // ntr::present_x/w/h: the shown part of it
    const uint32_t *other;       // engine B's, or null
    int other_stride;
    uint64_t fb_sig, other_sig;  // the pictures as their makers' numbers (card3ds_picture_sigs)
    bool main_lower, kept;
    bool blend;                  // the overlay holds GX_OV_BLEND words (ntr/gx.h)
    Held held;                   // a copy; tris point into `tris` when async
    std::vector<ntr::GxTriangle> tris;
    // The frame as gx.cpp recorded it (GAME3DS_GPU_XFORM=1), drawn with
    // gpu3ds::draw_record when it was recorded whole and fits.
    ntr::GxRecord rec;
    bool rec_ok;
    CapJob cap;                  // the frame's deferred capture, drawn with it
    // INTERPOLATION (GAME3DS_INTERP=1, interp3ds.cpp): the in-between frame's
    // matrices, one per rec.mtx; mid_ok: this job presents it, then the tick
    std::vector<ntr::GxRecMatrix> mid;
    std::vector<ntr::GxRecVertex> mid_v;   // empty, or its own vertices (meshes that move)
    bool mid_ok;
    int imode;                   // interp_mode() when taken
    // Interpolation pairing (interp3ds_take, else interp3ds_reset) runs at the
    // hand-off on the game thread, or in the worker with GAME3DS_INTERP_CORE2=1.
    // view_ok: the view as it was; interp_vb2: the tick spans two vblanks.
    bool interp_take, interp_vb2, view_ok, interp_done;
    bool text_box;   // a text box was open at the take (no in-between: run_interp)
    int32_t view[12];
    bool menu;                   // card3ds_menu_bars: a menu's frame
    int bar_dark[2];             // edge_dark of its top-screen picture, left / right
    const uint32_t *strips;      // card3ds_other_strips: the sky beside engine B's picture, or null
    SubAffine aff;               // card3ds_other_affine: on, `other` is the sprites over this BG
    bool lay, lay_two, lay_post;  // the menu layer (g_lay.px is read in place)
    int lay_w, lay_h, lay_stride;
    int fade_evy;
    bool fade_white;
    float fade_lv[2];            // the fade shown in the job's two frames (FadeFollow)
    bool other_white;            // engine B's whole-screen fade, left to the present
    float other_lv[2];           // (game3ds_sub_fade_of), shown likewise
    const unsigned char *mask;
    int mask_stride;
    bool mask_shown3d, mask_clear_opaque;
    bool timed;                  // the frame's presentation is timed
    u64 prep_ticks;              // its preparation on the game thread
    unsigned bucket;             // the report interval it belongs to
    gpu3ds::PresentMarks marks;  // its timeline once presented (timed jobs)
};

bool g_menu_frame;   // card3ds_menu_bars
int interp_mode();   // the flicker bisect's mode (below)

int bars_mode();   // below
int edge_dark(const uint32_t *px, int stride, int x, int h);   // below

// Where the pairing runs. On core 2 it took 3-6 ms a tick (core 2 also runs the
// audio mix and engine B's scan-out), the in-between missed its vblank and BoB
// dropped from 82% to 69% full-rate frames. On the game thread at the hand-off
// it costs 2-4 ms, which fits. GAME3DS_INTERP_CORE2=1 moves it back.
//
// GPU pass profile (GAME3DS_GPU_PASS_PROFILE=1, measuring only, the picture is
// wrong while it runs). citro3d's GPU time covers the whole queue, so to see
// what each pass costs this leaves one pass out for 120 jobs at a time, round
// robin, and files the GPU time under that pass. Cost = "all" minus its column.
enum { PASS_REAR = 1, PASS_3D = 2, PASS_OVERLAY = 4, PASS_FADE = 8, PASS_OTHER = 16 };
constexpr int kPassModes = 6;   // 0: every pass; m: pass bit m - 1 left out
const char *const kPassNames[kPassModes] = {"all", "-rear", "-3d", "-overlay", "-fade/bars",
                                            "-other"};
int g_pass_skip;

bool pass_on(int pass)
{
    return !(g_pass_skip & pass);
}

void pass_profile_next()
{
    static const bool on = getenv("GAME3DS_GPU_PASS_PROFILE") != nullptr;
    if (!on) return;
    static unsigned jobs;
    const int mode = (int)((jobs++ / 120) % kPassModes);
    g_pass_skip = mode ? 1 << (mode - 1) : 0;
    gpu3ds::set_frame_tag(mode, kPassNames[mode]);
}

bool interp_core2()
{
    static const bool on = getenv("GAME3DS_INTERP_CORE2") != nullptr;
    return on;
}

void run_interp(Job &j)
{
    j.interp_done = true;
    if (j.interp_take) {
        // an in-between frame only where a tick spans two vblanks (the
        // level loop's 30 Hz; the scene loop ticks every vblank)
        const bool interp = interp3ds_on();
        const bool made = interp3ds_take(j.rec, j.mid, j.mid_v, j.view_ok ? j.view : nullptr);
        // No in-between while a text box is up when take_job says so. With both
        // frames, BoB's text box ticks hit 50 ms (20 fps); one frame a tick holds 30.
        // Pairing still runs, so the in-between comes back as soon as it can.
        j.mid_ok = made && interp && j.interp_vb2 && j.imode != 4 && !j.text_box;
    } else {
        interp3ds_reset();
    }
}

// card3ds_picture_sigs: the next take_job's two numbers, then 0 again
uint64_t g_sig_next[2];
// card3ds_other_strips: the next take_job's, then null again
const uint32_t *g_strips_next;
// card3ds_other_affine: the same
const SubAffine *g_aff_next;
// card3ds_other_fade: the same
int g_other_evy_next;
bool g_other_white_next;

// Smooth fades. The DS fades in 16 brightness steps, roughly one every other
// tick, which looks choppy next to the 60 fps top screen (very visible on the
// HUD after Exit Course). For each screen, ramp the shown level towards each
// new step over the average time a step takes, one frame at a time. It lags by
// about one step but moves every frame. Big jumps (over 4 steps), a colour
// change, or GAME3DS_FADE_SMOOTH=0 show the game's level directly.
bool fade_smooth_on()
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GAME3DS_FADE_SMOOTH");
        on = !(e && e[0] == '0');
    }
    return on == 1;
}

struct FadeFollow {
    float shown = 0.0f, rate = 0.0f;
    float interval = 2.0f;   // ticks a step takes, averaged
    int target = 0, since = 0;
    bool white = false;

    // the level for each of a tick's display frames: fpt of them (1 or 2), the
    // last in out[1]
    void tick(int t, bool w, int fpt, float out[2])
    {
        if (t < 0) t = 0;
        if (t > 16) t = 16;
        ++since;
        if (!fade_smooth_on() || w != white || fabsf((float)t - shown) > 4.0f) {
            shown = (float)t;
            rate = 0.0f;
            since = 0;
        } else if (t != target) {
            const int k = since < 1 ? 1 : since > 4 ? 4 : since;
            interval = 0.5f * interval + 0.5f * (float)k;
            rate = fabsf((float)t - shown) / (interval * (float)fpt);
            since = 0;
        }
        target = t;
        white = w;
        for (int f = 0; f < 2; ++f) {
            if (f < fpt) {
                const float d = (float)target - shown;
                shown = fabsf(d) <= rate ? (float)target : shown + (d > 0 ? rate : -rate);
            }
            out[f] = shown;
        }
        if (fpt < 2) out[0] = out[1];
    }
};
FadeFollow g_fade_top, g_fade_other;

// The captures by number (ntr's): the newest handed to a job (the game
// thread's) and the newest a job has written or let go (core 2's). Jobs run
// in order, so every capture up to `written` is done.
unsigned g_cap_handed;
volatile unsigned g_cap_written;
// Frames left in which the hand-off has been waiting for core 2 (more than a
// millisecond, within the last 60 frames): take_job builds the record's
// matrices on the game thread then.
int g_core2_behind;
// The last two whole-screen captures as engine B's scan-out would show them
// (each pixel BGR555-expanded, as ppu_sub's bgr555), by number: engine B
// showing one is shown from here (card3ds_capture_picture), not scanned back
// out of VRAM. A slot is the number's from its hand-off (g_cap_pic_for); its
// job writes it before the next job, the one that shows it, runs. In linear
// memory for the transfer engine.
ntr::SubFramebuffer *g_cap_pic[2];
unsigned g_cap_pic_for[2];
unsigned long long g_cap_fence_ticks;
unsigned g_cap_jobs;           // captures handed to jobs since the last report

// On core 2, in the job's frame_end: gpu3ds::capture_queue's copy (null: none
// was made) into the block, as ppu_display_capture writes it. Picture column
// x is buffer row x; picture row y is entry rows - 1 - y.
void cap_write(const uint32_t *lin, int rows, void *arg)
{
    CapJob &c = *static_cast<CapJob *>(arg);
    const int slot = c.seq & 1;
    ntr::SubFramebuffer *const pic =
        c.w == ntr::SUB_W && c.h == ntr::SUB_H ? g_cap_pic[slot] : nullptr;
    if (lin) {
        for (int x = 0; x < c.w; ++x) {
            const uint32_t *col = lin + (size_t)(c.x + x) * (size_t)rows + (size_t)(rows - 1 - c.y);
            uint16_t *out = c.dst + x;
            for (int y = 0; y < c.h; ++y) {
                const uint32_t v = col[-y];   // RGBA8, red in the top byte
                out[(size_t)y * (size_t)c.w] =
                    (uint16_t)(0x8000u | ((v >> 1) & 0x7C00u) | ((v >> 14) & 0x03E0u) | (v >> 27));
                if (pic) {
                    // each channel's top 5 bits, and those bits' top 3 again below
                    // them: bgr555's (c << 3 | c >> 2), all three at once
                    const uint32_t q = v & 0xF8F8F800u;
                    pic->px[y][x] = 0xFF000000u | ((q | ((q >> 5) & 0x07070700u)) >> 8);
                }
            }
        }
    }
    c.on = false;
    __dmb();
    g_cap_written = c.seq;
}

// ntr's fence (gx_set_capture_fence): capture `seq` (0: the newest handed to
// a job) is written, or with wait 0 whether it is. One never handed to a job
// is not the card's to wait for.
bool worker_busy();   // below, with the worker

int capture_fence(unsigned seq, int wait)
{
    if (!seq) seq = g_cap_handed;
    if (!seq || (int)(seq - g_cap_handed) > 0 || (int)(g_cap_written - seq) >= 0) return 1;
    if (!wait) return 0;
    const u64 t0 = svcGetSystemTick();
    while ((int)(g_cap_written - seq) < 0) {
        // Its job writes it in its frame_end; should it not have, it is written
        // here once the worker is done.
        if (!worker_busy()) {
            card3ds_wait_idle();
            gpu3ds::capture_finish();
            if ((int)(g_cap_written - seq) >= 0) break;
        }
        if (svcGetSystemTick() - t0 > SYSCLOCK_ARM11 / 2) {
            static bool said;
            if (!said) {
                said = true;
                printf("[capture] a deferred capture was not written within 500 ms; going on without it\n");
            }
            break;
        }
        svcSleepThread(100000);
    }
    __dmb();
    g_cap_fence_ticks += svcGetSystemTick() - t0;
    return 1;
}

void take_job(Job &j, const ntr::Framebuffer &fb, const uint32_t *other, int other_stride,
              bool main_lower)
{
    j.fb = &fb.px[0][0];
    j.fb_sig = g_sig_next[0];
    j.other_sig = other ? g_sig_next[1] : 0;
    g_sig_next[0] = g_sig_next[1] = 0;
    j.w = ntr::active_w;
    j.h = ntr::active_h;
    j.px = ntr::present_x();
    j.pw = ntr::present_w();
    j.ph = ntr::present_h();
    j.other = other;
    j.other_stride = other_stride;
    j.main_lower = main_lower;
    j.kept = g_held.active && ntr::gx_card_overlay_active();
    j.blend = j.kept && ntr::gx_card_blend_seen();
    j.held = g_held;
    static const bool xform = getenv("GAME3DS_GPU_XFORM") != nullptr;
    j.rec_ok = false;
    j.mid_ok = false;
    j.imode = 0;
    j.interp_take = false;
    j.text_box = false;
    j.interp_done = false;
    j.menu = g_menu_frame;
    j.bar_dark[0] = j.bar_dark[1] = 16;
    j.strips = j.menu && main_lower && other ? g_strips_next : nullptr;
    g_strips_next = nullptr;
    j.aff.on = 0;
    if (g_aff_next && other && !main_lower) j.aff = *g_aff_next;
    g_aff_next = nullptr;
    if (j.menu) {
        // the top screen's picture: engine B's when the menus put engine A
        // below (main_lower), else the band of fb
        if (main_lower && other) {
            j.bar_dark[0] = edge_dark(other, other_stride, 0, ntr::SUB_H);
            j.bar_dark[1] = edge_dark(other, other_stride, ntr::SUB_W - 1, ntr::SUB_H);
        } else if (ntr::present_w() < ntr::active_w) {
            const int x0 = ntr::present_x(), x1 = x0 + ntr::present_w() - 1;
            j.bar_dark[0] = edge_dark(&fb.px[0][0], ntr::SCREEN_W, x0, ntr::active_h);
            j.bar_dark[1] = edge_dark(&fb.px[0][0], ntr::SCREEN_W, x1, ntr::active_h);
        }
    }
    if (j.kept && xform) {
        size_t nv = 0, nt = 0;
        ntr::gx_record_size(nv, nt);
        // Only collect matrix factors where an in-between can happen (ticks of two
        // vblanks, i.e. the level loop). The title and menus tick every vblank, and
        // recording and pairing there was ~6% of a frame for nothing. Minigames don't
        // get in-betweens either: their menu (scene 5) and the games (361-390) keep
        // the DS's frames, whatever the 60 fps setting is.
        const int scene = port_scene_live_id();
        const bool minigame = scene == 5 || (scene >= 361 && scene <= 390);
        j.rec.want_raw = interp3ds_wanted() && gpu3ds::vblanks_per_frame() == 2 && !minigame;
        // Without the factors, each matrix is built in present_body instead, on core 2
        // for async jobs. The title's ~490 matrices a frame cost the game thread
        // ~1.4 ms. GAME3DS_REC_DEFER_OFF=1 builds them here like before.
        // ...unless core 2 is the one behind: while the game thread's hand-offs
        // wait for its jobs (g_core2_behind), they are built here, where there is
        // time to spare (a dual-screen minigame's core 2 draws its frame twice).
        static const bool defer = getenv("GAME3DS_REC_DEFER_OFF") == nullptr;
        if (gpu3ds::record_fits(nv, nt))
            j.rec_ok = ntr::gx_take_record(j.rec, defer && !j.rec.want_raw && !g_core2_behind) != 0;
        // The recorder's per-id stats (a hash and three sums per vertex) are only
        // collected for ticks like this one, after the take. The first level tick after
        // a minigame pairs without them (interp3ds computes them itself then).
        ntr::gx_record_id_stats(j.rec.want_raw ? 1 : 0);
        if (j.rec_ok && j.rec.want_raw) {
            // the pairing: run_interp, at the end of this function or in
            // present_body (interp_core2)
            j.interp_take = true;
            j.interp_vb2 = gpu3ds::vblanks_per_frame() == 2;
            // A text box keeps its in-betweens (frames built ahead have room for them)
            // unless its ticks keep missing two vblanks: the third slow tick with the box
            // up drops them until it closes. GAME3DS_INTERP_TEXT=0: never with a box up;
            // =1: always.
            static const int text_interp =
                getenv("GAME3DS_INTERP_TEXT") ? atoi(getenv("GAME3DS_INTERP_TEXT")) : 2;
            static bool was_open, dropped;
            static int misses;
            static u64 last_take;
            const u64 now = svcGetSystemTick();
            const bool open = data_0209d660 != 0;
            if (open && was_open && last_take &&
                now - last_take > (u64)SYSCLOCK_ARM11 * 5 / (2 * 60))   // 2.5 vblanks
                ++misses;
            last_take = now;
            if (open != was_open) {
                misses = 0;
                dropped = false;
                printf("[interp] text box %s\n", open ? "open" : "closed");
            }
            if (open && !dropped && text_interp == 2 && misses >= 3) {
                dropped = true;
                printf("[interp] text box: %d ticks past their two vblanks, one frame a tick until it closes\n",
                       misses);
            }
            j.text_box = open && (text_interp == 0 || dropped);
            was_open = open;
            j.imode = interp_mode();
            j.view_ok = interp3ds_capture_view(j.view);
        }
        if (!j.rec_ok) {
            // the triangles after all (a lean frame makes them now)
            ntr::gx_record_to_list();
            size_t n = 0;
            j.held.tris = ntr::gx_polygons(n);
            j.held.count = n;
        }
    }
    // The capture taken at ppu_display_capture goes with the frame it is of, which
    // the job draws from its record. (A frame settled after the capture took it
    // back first, gx_card_settle_because.)
    j.cap.on = false;
    if (g_cap_next.on) {
        if (j.kept && j.rec_ok) {
            j.cap = g_cap_next;
            ++g_cap_jobs;
            g_cap_pic_for[j.cap.seq & 1] = j.cap.seq;
            __dmb();
            g_cap_handed = j.cap.seq;
        } else {
            static bool said;
            if (!said) {
                said = true;
                printf("[capture] a deferred capture's frame was not presented from its record: "
                       "not written\n");
            }
        }
        g_cap_next.on = false;
    }
    j.lay = j.kept && g_lay.live;
    j.lay_two = g_lay.two;
    j.lay_post = g_lay.post;
    j.lay_w = g_lay.w;
    j.lay_h = g_lay.h;
    j.lay_stride = g_lay.stride;
    j.fade_evy = g_fade_evy;
    j.fade_white = g_fade_white;
    {   // the two screens' fades a display frame at a time (FadeFollow)
        const int fpt = gpu3ds::vblanks_per_frame() > 1 ? 2 : 1;
        g_fade_top.tick(g_fade_evy, g_fade_white, fpt, j.fade_lv);
        const int oe = other ? g_other_evy_next : 0;
        g_fade_other.tick(oe, g_other_white_next, fpt, j.other_lv);
        j.other_white = g_other_white_next;
        g_other_evy_next = 0;
    }
    j.mask = g_mask;
    j.mask_stride = g_mask_stride;
    j.mask_shown3d = g_mask_shown3d;
    j.mask_clear_opaque = g_mask_clear_opaque;
    if (j.kept) ++g_n.presents_kept; else ++g_n.presents_cpu;
    // The frame's state is spent: the next tick starts from none.
    g_held.active = false;
    g_lay.live = false;
    g_mask = nullptr;
    // The fade is the message compositor's per-frame verdict.
    g_fade_evy = 0;
    // the pairing at the handoff (see interp_core2)
    if (!interp_core2()) run_interp(j);
}

// The main picture's viewport. Scenes shown at the DS's 4:3 (title, menus,
// minigames) are pillarboxed inside a wider picture, and the rectangle is fit
// to the screen. On the bottom screen that used to put a 256x192 picture in a
// frame of margin. Everywhere else it's just viewport_for.
gpu3ds::Viewport main_viewport(gpu3ds::Screen s, const Job &j)
{
    if (j.pw <= 0 || j.pw >= j.w || j.ph <= 0 || j.ph > j.h) return viewport_for(s, j.w, j.h);
    gpu3ds::Viewport vp = viewport_for(s, j.pw, j.ph);
    const float kx = vp.w / (float)j.pw, ky = vp.h / (float)j.ph;
    vp.x -= (float)j.px * kx;
    vp.w = (float)j.w * kx;
    vp.h = (float)j.h * ky;
    return vp;
}

// GAME3DS_BARS=auto|edge|black: the menus' pillarbox (fill_bars). auto, the
// default: edge where the picture's edge is plain, black where it is art.
int bars_mode()
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("GAME3DS_BARS");
        mode = !e ? 2 : !strcmp(e, "edge") ? 1 : !strcmp(e, "black") ? 0 : 2;
        printf("[bars] the menus' pillarbox: %s\n", mode == 1 ? "edge" : mode == 2 ? "auto" : "black");
    }
    return mode;
}

// How dark a side bar next to a menu's picture should be (0 = edge colour as
// is, 16 = black) in auto mode. Measured from the picture's edge column as its
// total change over its net change: the title's sky gradient comes out around
// 3, the castle behind the file select 15+. A blank frame is 0. Read on the
// game thread before the buffer is reused.
int edge_dark(const uint32_t *px, int stride, int x, int h)
{
    if (bars_mode() != 2 || h < 2) return 0;
    long tv = 0, net = 0;
    for (int y = 0; y + 1 < h; ++y) {
        const uint32_t a = px[(size_t)y * stride + x], b = px[(size_t)(y + 1) * stride + x];
        for (int sh = 0; sh < 24; sh += 8)
            tv += abs((int)((a >> sh) & 0xFF) - (int)((b >> sh) & 0xFF));
    }
    const uint32_t top = px[x], bottom = px[(size_t)(h - 1) * stride + x];
    for (int sh = 0; sh < 24; sh += 8)
        net += abs((int)((top >> sh) & 0xFF) - (int)((bottom >> sh) & 0xFF));
    // the ratio times 4, in integers: plain up to 5, art from 10
    const long r4 = tv * 4 / (net > 24 ? net : 24);
    if (r4 <= 20) return 0;
    if (r4 >= 40) return 16;
    return (int)((r4 - 20) * 16 / 20);
}

// GAME3DS_SEQ_SHOT=first:count[:stride] (for debugging in-betweens): save the
// top screen of `count` frames starting at kept frame `first` (in-betweens
// counted), as game3ds_seq_NNN_{mid,tick}.bmp. Slows the run while active, and
// a file may be one frame behind its name, but the order is right.
void seq_shot(bool mid)
{
    static int first = -1, count = 0, stride = 1, n = 0;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char *e = getenv("GAME3DS_SEQ_SHOT")) sscanf(e, "%d:%d:%d", &first, &count, &stride);
        if (stride < 1) stride = 1;
    }
    if (first < 0) return;
    const int d = n++ - first;
    if (d < 0 || d % stride != 0 || d / stride >= count) return;
    const int k = d / stride;
    gspWaitForVBlank();
    gspWaitForVBlank();
    char path[64];
    snprintf(path, sizeof path, "sdmc:/sm64ds3ds/game3ds_seq_%03d_%s.bmp", k, mid ? "mid" : "tick");
    // GAME3DS_SEQ_SCREEN=bottom: the bottom screen instead; =both: each screen
    // (_top, _bot), to see the two screens' pictures frame by frame
    static const bool bottom = getenv("GAME3DS_SEQ_SCREEN") && !strcmp(getenv("GAME3DS_SEQ_SCREEN"), "bottom");
    static const bool both = getenv("GAME3DS_SEQ_SCREEN") && !strcmp(getenv("GAME3DS_SEQ_SCREEN"), "both");
    if (both) {
        snprintf(path, sizeof path, "sdmc:/sm64ds3ds/game3ds_seq_%03d_top.bmp", k);
        gpu3ds::screenshot_bmp(path, gpu3ds::SCREEN_TOP);
        snprintf(path, sizeof path, "sdmc:/sm64ds3ds/game3ds_seq_%03d_bot.bmp", k);
        gpu3ds::screenshot_bmp(path, gpu3ds::SCREEN_BOTTOM);
        return;
    }
    gpu3ds::screenshot_bmp(path, bottom ? gpu3ds::SCREEN_BOTTOM : gpu3ds::SCREEN_TOP);
}

// Menu pillarbox. Scenes stay at the DS's 4:3, so menus leave 40 px bars on
// the 400-wide top screen. They're black, or on menu frames the picture's
// outermost column stretched across (the title's sky gradient then fills the
// screen). GAME3DS_BARS=auto (default) darkens each side by edge_dark, edge
// never darkens, black never fills. vp is where the picture was drawn, px / pw
// the visible band, fade_evy fades the bars along with the picture.
//
// Where the painter made sky strips (sub_scan_single.inl), the bar shows the
// real continuation of the sky, 32 DS columns past the edge.
void fill_bars(gpu3ds::Screen s, const gpu3ds::Viewport &vp, int pic_w, int pic_h, float px,
               float pw, int slot, const int dark[2], float fade_evy, bool fade_white, int strips = -1)
{
    int sw, sh;
    gpu3ds::screen_size(s, &sw, &sh);
    const float kx = vp.w / (float)pic_w, ky = vp.h / (float)pic_h;
    const float b0 = vp.x + px * kx, b1 = b0 + pw * kx;
    const bool on = slot >= 0 && bars_mode() != 0;
    gpu3ds::Viewport m;
    m.y = 0.0f;
    m.h = (float)sh;
    auto fill = [&](int side, float x0, float x1) {
        m.x = x0;
        m.w = x1 - x0;
        gpu3ds::draw_fade(16, false, m);   // black under it, and where it has nothing
        // the screen's rows onto the picture's
        const float v0 = (0.0f - vp.y) / ky, v1 = ((float)sh - vp.y) / ky;
        if (strips >= 0) {
            gpu3ds::Viewport sm = m;
            const float reach = 32.0f * kx;   // the strips' 32 columns a side
            if (side == 0 && x1 - x0 > reach) sm.x = x1 - reach;
            sm.w = (side == 0 ? x1 : (x0 + reach < x1 ? x0 + reach : x1)) - sm.x;
            const float du = sm.w / kx;
            gpu3ds::overlay_draw_part(strips, gpu3ds::OVERLAY_OPAQUE, sm, side == 0 ? 32.0f - du : 32.0f, v0,
                                      side == 0 ? 32.0f : 32.0f + du, v1);
            if (fade_evy > 0) gpu3ds::draw_fade(fade_evy, fade_white, sm);
            return;
        }
        if (!on || dark[side] >= 16) return;
        const float u = side == 0 ? px + 0.5f : px + pw - 0.5f;
        gpu3ds::overlay_draw_part(slot, gpu3ds::OVERLAY_OPAQUE, m, u, v0, u, v1);
        if (dark[side] > 0) gpu3ds::draw_fade(dark[side], false, m);
        if (fade_evy > 0) gpu3ds::draw_fade(fade_evy, fade_white, m);
    };
    if (b0 > 0.5f) fill(0, 0.0f, b0);
    if (b1 < (float)sw - 0.5f) fill(1, b1, (float)sw);
}

// `inputs_read`, if given, runs once every buffer the game thread rewrites
// next frame (fb, engine B's picture, the menu layer, the mask) has been copied
// into textures.
//
// Interpolation debug modes (GAME3DS_INTERP_MODE=N, with GAME3DS_INTERP on),
// used to track down flicker that showed on hardware but not Azahar:
//   0 normal: in-between, then the tick
//   1 the tick twice (no blend)
//   2 the in-between twice (no tick)
//   3 normal, but hold the game thread until both frames are drawn
//   4 off: no in-between (30 fps)
// (GAME3DS_INTERP_SAME=1 / _SHOW_MID=1 are modes 1 and 2.)
int interp_mode()
{
    static int mode = -1;
    if (mode < 0) {
        mode = getenv("GAME3DS_INTERP_SAME") ? 1 : getenv("GAME3DS_INTERP_SHOW_MID") ? 2 : 0;
        if (const char *e = getenv("GAME3DS_INTERP_MODE")) mode = atoi(e) % 5;
        static const char *const names[] = {"normal", "same (tick twice)", "mid only (in-between twice)",
                                            "late (game held through both draws)", "off (30 fps)"};
        printf("[interp] mode %d = %s\n", mode, names[mode]);
    }
    return mode;
}

// FPS counter (card3ds_set_options): frames drawn on the top screen per second,
// its picture redrawn when the number changes.
Card3dsCounterArt g_counter_art;
struct Counter {
    u64 t0;
    unsigned n, fps, ver;
    bool valid;
    int w, h;
    uint32_t px[128 * 32];
} g_ctr;

void counter_draw(bool count = true)
{
    if (!g_counter_art) return;
    const u64 now = svcGetSystemTick();
    if (!count) {
    } else if (!g_ctr.t0) {
        g_ctr.t0 = now;
        g_ctr.n = 0;
    } else {
        ++g_ctr.n;
        const u64 dt = now - g_ctr.t0;
        if (dt >= SYSCLOCK_ARM11) {
            const unsigned fps = (unsigned)(((u64)g_ctr.n * SYSCLOCK_ARM11 + dt / 2) / dt);
            g_ctr.t0 = now;
            g_ctr.n = 0;
            if (!g_ctr.valid || fps != g_ctr.fps) {
                g_ctr.fps = fps;
                g_ctr.w = g_ctr.h = 0;
                g_counter_art(fps, g_ctr.px, 128, &g_ctr.w, &g_ctr.h);
                ++g_ctr.ver;
                g_ctr.valid = g_ctr.w > 0 && g_ctr.h > 0;
            }
        }
    }
    if (!g_ctr.valid) return;
    // the bottom-left corner, below the red coin row (DS y 157-173, host
    // 196-216): the top-left one sat over the lives counter
    gpu3ds::Viewport vp;
    vp.x = 4.0f;
    vp.y = 240.0f - 4.0f - (float)g_ctr.h;
    vp.w = (float)g_ctr.w;
    vp.h = (float)g_ctr.h;
    gpu3ds::hud_draw(g_ctr.px, 128, g_ctr.w, g_ctr.h, g_ctr.ver, vp);
}

template <typename F>
void present_body(Job &j, F inputs_read)
{
    ntr::gx_record_finish(j.rec);   // its matrices, when take_job left them (deferred)
    const gpu3ds::Screen main_s = j.main_lower ? gpu3ds::SCREEN_BOTTOM : gpu3ds::SCREEN_TOP;
    const gpu3ds::Viewport vp = main_viewport(main_s, j);
    gpu3ds::record_upload_forget();   // a new record: uploaded again
    pass_profile_next();
    // Send the two pictures through the transfer engine where possible (they're in
    // linear memory) before the frame begins. If nothing else needs reading, the
    // game thread is released before this job even waits for its vblank.
    gpu3ds::transfers_begin();
    // Frames built ahead (gpu3ds ahead_flush): an interpolated job's two frames
    // start without waiting for the GPU, so the transfers go inside the first frame
    // and the game thread's buffers are freed once they've run. 1-vblank jobs (the
    // title and menus at 60 Hz) build ahead too; through SYNCDRAW the title missed
    // a vblank every 3-4 frames.
    const bool one = j.mid_ok || gpu3ds::vblanks_per_frame() == 1;
    if (one) gpu3ds::set_vblanks_override(1);
    const bool ahead = one && gpu3ds::ahead_ready();
    int ov_main = -1, ov_other = -1, ov_strips = -1;
    auto transfers = [&] {
        ov_main = gpu3ds::overlay_transfer(j.fb, ntr::SCREEN_W, j.w, j.h, j.fb_sig);
        ov_other = j.other ? gpu3ds::overlay_transfer(j.other, j.other_stride, ntr::SUB_W,
                                                      ntr::SUB_H, j.other_sig)
                           : -1;
        // the sky beside it (fill_bars), the third picture; none: the old bars
        ov_strips = j.strips && ov_other >= 0 ? gpu3ds::overlay_transfer(j.strips, 64, 64, ntr::SUB_H, 0) : -1;
    };
    if (!ahead) transfers();
    const bool masked = j.kept && j.mask && j.fade_evy > 0;
    // The brightness mask is uploaded inside the frame (its texture may still be in
    // use until frame_begin), so keep a copy and release the game thread now
    // rather than after the vblank wait.
    static std::vector<uint8_t> mask_copy;
    const uint8_t *mask = j.mask;
    if (masked && j.mask_stride > 0 && j.h > 0) {
        const size_t n = (size_t)j.mask_stride * (size_t)j.h;
        mask_copy.resize(n);
        memcpy(mask_copy.data(), j.mask, n);
        mask = mask_copy.data();
    }
    const bool early = !ahead && ov_main >= 0 && (!j.other || ov_other >= 0) && !j.lay;
    const bool late = j.interp_take && j.imode == 3;
    if (early && !late) inputs_read();
    // the pairing here only under GAME3DS_INTERP_CORE2 (run_interp); every
    // job either takes or resets, in job order
    if (!j.interp_done) run_interp(j);
    // Interpolation (j.mid_ok): two frames of one vblank each, the in-between
    // (tick N's record with blended matrices) and then the tick. The 2D is
    // uploaded once in the first frame and reused. GAME3DS_INTERP_SHOW_MID=1 draws
    // the in-between twice; GAME3DS_INTERP_SAME=1 draws the tick twice.
    const int frames = j.mid_ok ? 2 : 1;
    const bool stereo = gpu3ds::stereo_on();
    int ov_lay[2] = {-1, -1};
    for (int f = 0; f < frames; ++f) {
        const bool mid = j.mid_ok && j.imode != 1 && (f == 0 || j.imode == 2);
        if (f) gpu3ds::frame_keep_overlays();
        // j.kept: a 3D scene, drawn at 2x under GAME3DS_FSAA until resolve()
        gpu3ds::frame_begin(0xFF000000u, main_s, j.kept);
        if (f == 0) {
            if (ahead) transfers();
            if (ov_main < 0) ov_main = gpu3ds::overlay_upload(j.fb, ntr::SCREEN_W, j.w, j.h);
            if (j.lay) {
                ov_lay[0] = gpu3ds::overlay_upload(g_lay.px[0], j.lay_stride, j.lay_w, j.lay_h);
                if (j.lay_two)
                    ov_lay[1] = gpu3ds::overlay_upload(g_lay.px[1], j.lay_stride, j.lay_w, j.lay_h);
            }
            if (masked)
                gpu3ds::fade_mask_upload(mask, j.mask_stride, j.w, j.h, j.mask_shown3d,
                                         j.mask_clear_opaque);
            if (j.other && ov_other < 0)
                ov_other = gpu3ds::overlay_upload(j.other, j.other_stride, ntr::SUB_W, ntr::SUB_H);
            if (!early && !late && !ahead) inputs_read();
        }
        // With 3D on, draw the top screen once per eye: the record's geometry is
        // shifted for the right eye, the 2D is the same.
        auto draw_main = [&](int eye) {
            if (j.kept) {
                if (pass_on(PASS_REAR)) gpu3ds::draw_rear_plane(rear_plane(j.held), vp);
                gpu3ds::set_fog(j.held.fog_on ? &j.held.fog : nullptr, j.held.wdepth);
                // The 3D picture's own geometry is the DS screen's 256x192, scaled to
                // the active picture (gx.cpp's project() already widened it).
                gpu3ds::set_toon(j.held.toon_shade, j.held.toon_rgb);
                const bool draw3d = pass_on(PASS_3D);
                const bool from_rec =
                    draw3d && j.rec_ok && gpu3ds::draw_record(j.rec, vp, mid ? j.mid.data() : nullptr,
                                                    mid && !j.mid_v.empty() ? j.mid_v.data() : nullptr);
                if (!from_rec && draw3d) gpu3ds::draw(j.held.tris, j.held.count, vp);
                if (f + 1 == frames && eye == 0) ++g_xform_n[j.rec_ok ? (from_rec ? 2 : 1) : 0];
                gpu3ds::resolve();   // the 2D from here on at the screen's own 1x
                if (pass_on(PASS_OVERLAY))
                    gpu3ds::overlay_draw(ov_main, j.blend ? gpu3ds::OVERLAY_KEPT_BLEND : gpu3ds::OVERLAY_KEPT,
                                         vp);
            } else {
                gpu3ds::overlay_draw(ov_main, gpu3ds::OVERLAY_OPAQUE, vp);
            }
            // The menu layer, plane 1 then plane 2, as gpu_raster.cpp's composite
            // lays them (cp_lay_over), before or after the fade.
            auto draw_layer = [&]() {
                gpu3ds::overlay_draw(ov_lay[0], gpu3ds::OVERLAY_PREMUL, vp);
                if (j.lay_two) gpu3ds::overlay_draw(ov_lay[1], gpu3ds::OVERLAY_PREMUL, vp);
            };
            if (j.lay && !j.lay_post) draw_layer();
            const bool fade_on = pass_on(PASS_FADE);
            const float lv = j.fade_lv[frames == 2 ? f : 1];   // FadeFollow's, this frame
            if (!fade_on) {
            } else if (j.kept && j.mask) {
                gpu3ds::fade_masked_draw((int)lroundf(lv), j.fade_white, vp);
            } else {
                gpu3ds::draw_fade(lv, j.fade_white, vp);
            }
            if (j.lay && j.lay_post) draw_layer();
            // The margins next to a 4:3 picture are black, or filled from the picture
            // (fill_bars: GAME3DS_BARS, menu frames, the top screen).
            if (j.pw > 0 && j.pw < j.w && fade_on) {
                fill_bars(main_s, vp, j.w, j.h, (float)j.px, (float)j.pw,
                          j.menu && main_s == gpu3ds::SCREEN_TOP ? ov_main : -1, j.bar_dark,
                          lv, j.fade_white);
            }
            if (main_s == gpu3ds::SCREEN_TOP) counter_draw(eye == 0);
        };
        draw_main(0);
        if (stereo && main_s == gpu3ds::SCREEN_TOP) {
            gpu3ds::set_eye(1);
            gpu3ds::frame_target(main_s, 0xFF000000u, j.kept);
            draw_main(1);
            gpu3ds::set_eye(0);
        }
        // Draw the other screen once per job: with an in-between, only on the tick
        // frame. Its picture is the same for both frames, and citro3d doesn't swap a
        // screen that wasn't drawn, so it just holds through the in-between. That also
        // keeps the in-between frame cheap.
        // (A fade done at present time is drawn on both frames, since it changes
        // every frame.)
        const float other_lv = j.other_lv[frames == 2 ? f : 1];
        const bool other_fading = other_lv > 0.0f || j.other_lv[0] != j.other_lv[1];
        const bool other_now = j.other && (!(j.mid_ok && f == 0) || other_fading) && pass_on(PASS_OTHER);
        if (other_now) {
            const gpu3ds::Screen other_s = j.main_lower ? gpu3ds::SCREEN_TOP : gpu3ds::SCREEN_BOTTOM;
            const gpu3ds::Viewport ovp = viewport_for(other_s, ntr::SUB_W, ntr::SUB_H);
            // At 1.25x draw it sharp (overlay_sharpen) so every line of the pixel art keeps
            // an even width. The 2x pass goes first since it has its own target.
            const bool sharp = ovp.w != (float)ntr::SUB_W &&
                               gpu3ds::overlay_sharpen(ov_other, ntr::SUB_W, ntr::SUB_H);
            // Only redraw the bottom screen when it changed: if the transfer kept the
            // picture (same buffer number), the screen already shows it, so skip the
            // clear, quad and display transfer. GAME3DS_BOTTOM_EVERY=1 always draws.
            static const bool every = getenv("GAME3DS_BOTTOM_EVERY") &&
                                      atoi(getenv("GAME3DS_BOTTOM_EVERY")) != 0;
            const uint64_t key = other_s == gpu3ds::SCREEN_BOTTOM && !every
                                     ? gpu3ds::overlay_key(ov_other)
                                     : 0;
            // The minimap's background drawn by the GPU (sub_affine.h): the picture is just
            // the sprites, and the BG rotates with the camera, so its matrix is part of
            // what's shown.
            const bool aff = j.aff.on && other_s == gpu3ds::SCREEN_BOTTOM;
            uint64_t aff_key = 0;
            if (aff) {
                const uint32_t w[] = {(uint32_t)j.aff.refx, (uint32_t)j.aff.refy, (uint32_t)j.aff.pa,
                                      (uint32_t)j.aff.pb,   (uint32_t)j.aff.pc,   (uint32_t)j.aff.pd,
                                      j.aff.ver, j.aff.backdrop, (uint32_t)j.aff.wrap};
                aff_key = 1469598103934665603ull;
                for (uint32_t v : w) aff_key = (aff_key ^ v) * 1099511628211ull;
            }
            // (a picture under a fade is drawn every time: the fade is not in its key)
            const uint64_t shown = key && other_lv <= 0.0f
                                       ? (key ^ (sharp ? 1ull << 63 : 0) ^ aff_key) | (aff ? 1 : 0)
                                       : 0;
            // (under 3D a top screen here is flat: the same picture for each eye)
            const int eyes = stereo && other_s == gpu3ds::SCREEN_TOP ? 2 : 1;
            for (int eye = 0; eye < eyes; ++eye) {
                if (eye) gpu3ds::set_eye(1);
                if (eye || !gpu3ds::screen_holds(other_s, shown)) {
                    gpu3ds::frame_target(other_s, aff ? j.aff.backdrop : 0xFF000000u);
                    // the sprites over the BG: premultiplied (a 0 word adds
                    // nothing), which the sharp pass's bilinear keeps right
                    const gpu3ds::OverlayMode mode = aff ? gpu3ds::OVERLAY_PREMUL : gpu3ds::OVERLAY_OPAQUE;
                    if (aff) gpu3ds::affine_draw(j.aff, ovp, ntr::SUB_W, ntr::SUB_H);
                    if (sharp)
                        gpu3ds::overlay_draw_sharp(ovp, mode);
                    else
                        gpu3ds::overlay_draw(ov_other, mode, ovp);
                    // engine B's whole-screen fade, left out of its picture
                    // (game3ds_sub_fade_of), over all of it
                    gpu3ds::draw_fade(other_lv, j.other_white, ovp);
                    gpu3ds::screen_mark(other_s, shown);
                }
                // In the menus engine B is on the top screen (256x192 at 320x240, centred in
                // 400), so fill its bars as GAME3DS_BARS says. The picture and the sky
                // strips are both drawn without engine B's whole-screen fade, so the bars
                // take the same fade as the picture (else the sky shows beside a picture
                // still faded to black, as after the Nintendo logo).
                if (j.menu && other_s == gpu3ds::SCREEN_TOP && bars_mode() != 0)
                    fill_bars(other_s, ovp, ntr::SUB_W, ntr::SUB_H, 0.0f, (float)ntr::SUB_W, ov_other,
                              j.bar_dark, other_lv, j.other_white, ov_strips);
                if (other_s == gpu3ds::SCREEN_TOP) counter_draw(eye == 0);
            }
            gpu3ds::set_eye(0);
        }
        // The frame's deferred capture (CapJob), after its screens: its 3D and 2D
        // as drawn above, at 1:1 and without the fade. It is written into VRAM in
        // frame_end, before this frame's vblank wait when it was built ahead.
        if (j.cap.on && f + 1 == frames) {
            gpu3ds::set_fog(j.held.fog_on ? &j.held.fog : nullptr, j.held.wdepth);
            if (!j.kept || ov_main < 0 ||
                !gpu3ds::capture_queue(j.rec, rear_plane(j.held), ov_main,
                                       j.blend ? gpu3ds::OVERLAY_KEPT_BLEND : gpu3ds::OVERLAY_KEPT,
                                       cap_write, &j.cap))
                cap_write(nullptr, 0, &j.cap);
        }
        if (ahead && f == 0) {
            gpu3ds::ahead_flush();   // the transfers have run: the buffers are the game thread's
            if (!late) inputs_read();
        }
        gpu3ds::frame_end();
        seq_shot(mid);
    }
    if (one) gpu3ds::set_vblanks_override(0);
    if (late) inputs_read();
}

// ---- on core 2 ----
//
// The level loop's present runs alongside the next tick. card3ds_present(...,
// async) takes the frame (take_job; the triangle list is swapped out of gx.cpp)
// and wakes a worker on core 2 to present it while the game thread runs the
// next tick. Done in series, BoB's tick (~21 ms) and present (~12 ms) sat right
// on the 30 fps line. The picture shows one frame later; nothing else changes.
//
// Two fences keep the game thread off what the worker is still reading:
//   inputs  the worker has copied fb, engine B's picture, the menu layer and
//           the mask into textures. Call card3ds_wait_inputs() before writing
//           any of them next frame.
//   idle    the job is done. Needed before the next hand-off, before anything
//           else uses citro3d, before gx.cpp frees decoded textures, and
//           before the app is suspended.
//
// The worker runs at the game thread's priority on core 2: below the audio mix
// and above engine B's scan-out (which isn't needed until the next tick anyway;
// when it ran above, it paused the present for ~9 ms). The Old 3DS has no core
// 2, so there the worker goes on core 0 one priority above the game thread and
// the next tick runs in its waits. GAME3DS_PRESENT_ASYNC_OFF=1 makes every
// present synchronous.
//
// Timing: the worker times its job with the same zones and adds them to the
// report interval it was handed off in, so the [perf] present lines are one
// report behind the rest.
Thread g_worker;
LightEvent g_go, g_inputs_ev, g_done_ev;
volatile bool g_quit;
bool g_async_ok;                 // a worker exists
bool g_inflight;                 // a job was handed over and not yet seen done
bool worker_busy() { return g_inflight && !LightEvent_TryWait(&g_done_ev); }
// Two job slots: core 2 presents *g_run while the game thread fills the other
// one, so the take (recording and pairing, 2.5-7.7 ms a course tick) doesn't
// have to wait for core 2. Anything a take swaps out of a slot belongs to the
// job before last, which is finished.
Job g_slot[2];
Job *g_run = &g_slot[0];
int g_next;
u64 g_prep_start;
bool g_prep_on;
aptHookCookie g_apt_cookie;

struct Bucket {
    gpu3ds::PresentTiming pt;
    unsigned handed;
};
Bucket g_bucket[2];
unsigned g_interval;
bool g_async_used;               // an async job since the last final report
LightLock g_bucket_lock;
// game-thread costs of the scheme, per report interval
u64 g_handoff_ticks, g_wait_idle_ticks, g_wait_inputs_ticks;
unsigned g_jobs;

void merge(gpu3ds::PresentTiming &into, const gpu3ds::PresentTiming &pt)
{
    for (int i = 0; i < gpu3ds::P_COUNT; ++i) into.ticks[i] += pt.ticks[i];
    into.total += pt.total;
    into.frames += pt.frames;
    into.pace_frames += pt.pace_frames;
    for (int i = 0; i < 5; ++i) into.pace_vbl[i] += pt.pace_vbl[i];
    into.pace_span += pt.pace_span;
    if (pt.pace_worst > into.pace_worst) into.pace_worst = pt.pace_worst;
    into.tris_in += pt.tris_in;
    into.tris_drawn += pt.tris_drawn;
    into.draw_calls += pt.draw_calls;
    if (pt.tris_max > into.tris_max) into.tris_max = pt.tris_max;
    if (pt.draws_max > into.draws_max) into.draws_max = pt.draws_max;
}

void worker(void *)
{
    for (;;) {
        g_card3ds_worker = "waiting for a job";
        LightEvent_Wait(&g_go);
        __dmb();
        if (g_quit) break;
        g_card3ds_worker = "presenting";
        Job &j = *g_run;
        if (j.timed) gpu3ds::present_job_begin(j.prep_ticks);
        present_body(j, [] { __dmb(); LightEvent_Signal(&g_inputs_ev); });
        if (j.timed) {
            gpu3ds::present_timing_end();
            j.marks = gpu3ds::take_present_marks();
            const gpu3ds::PresentTiming pt = gpu3ds::take_present_timing();
            LightLock_Lock(&g_bucket_lock);
            merge(g_bucket[j.bucket].pt, pt);
            LightLock_Unlock(&g_bucket_lock);
        }
        __dmb();
        g_card3ds_worker = "done, signalling";
        LightEvent_Signal(&g_done_ev);
    }
}

void apt_hook(APT_HookType hook, void *)
{
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONEXIT)
        card3ds_wait_idle();
    // back from the HOME menu or sleep: a screen left as it was may not show
    // what it did, so the next frame draws both
    if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) gpu3ds::screens_forget();
}

void async_init()
{
    if (getenv("GAME3DS_PRESENT_ASYNC_OFF")) {
        printf("[present-async] off (GAME3DS_PRESENT_ASYNC_OFF)\n");
        return;
    }
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // core 2 at the game thread's priority; an Old 3DS's core 0 one above it
    const int core = n3ds ? 2 : 0;
    const s32 wprio = n3ds || prio <= 0x18 ? prio : prio - 1;
    LightEvent_Init(&g_go, RESET_ONESHOT);
    LightEvent_Init(&g_inputs_ev, RESET_STICKY);
    LightEvent_Init(&g_done_ev, RESET_STICKY);
    LightLock_Init(&g_bucket_lock);
    g_worker = threadCreate(worker, nullptr, 64 * 1024, wprio, core, false);
    if (!g_worker) {
        printf("[present-async] off (core %d thread refused)\n", core);
        return;
    }
    aptHook(&g_apt_cookie, apt_hook, nullptr);
    for (ntr::SubFramebuffer *&p : g_cap_pic)
        p = static_cast<ntr::SubFramebuffer *>(linearAlloc(sizeof(ntr::SubFramebuffer)));
    ntr::gx_set_capture_fence(capture_fence);
    g_async_ok = true;
    printf("[present-async] on: the level loop presents on core %d%s, one frame behind its tick\n",
           core, n3ds ? "" : " above the game thread (its waits are the next tick's)");
}

void async_shutdown()
{
    if (!g_async_ok) return;
    card3ds_wait_idle();
    capture_fence(0, 1);
    ntr::gx_set_capture_fence(nullptr);
    g_quit = true;
    __dmb();
    LightEvent_Signal(&g_go);
    threadJoin(g_worker, U64_MAX);
    threadFree(g_worker);
    aptUnhook(&g_apt_cookie);
    g_async_ok = false;
}

// settle 2 with GxGpuFrame::defer: take this frame's capture for its job (1),
// or not (0: ppu captures now); -1 withdraws a taken one.
int capture_defer(const ntr::GxGpuFrame *f)
{
    if (f->defer < 0) {
        g_cap_next.on = false;
        return 1;
    }
    static const bool off = getenv("GAME3DS_CAPTURE_DEFER_OFF") != nullptr;
    static const bool xform = getenv("GAME3DS_GPU_XFORM") != nullptr;
    if (off || !xform || !g_async_ok || !g_held.active || g_cap_next.on || !f->cap_dst ||
        f->cap_w <= 0 || f->cap_h <= 0 || f->cap_x + f->cap_w > ntr::SCREEN_W ||
        f->cap_y + f->cap_h > ntr::SCREEN_H)
        return 0;
    size_t nv = 0, nt = 0;
    ntr::gx_record_size(nv, nt);
    if (!gpu3ds::record_fits(nv, nt)) return 0;   // the job could not draw it
    g_cap_next.on = true;
    g_cap_next.dst = f->cap_dst;
    g_cap_next.x = f->cap_x;
    g_cap_next.y = f->cap_y;
    g_cap_next.w = f->cap_w;
    g_cap_next.h = f->cap_h;
    g_cap_next.seq = f->cap_seq;
    return 1;
}

}  // namespace

bool card3ds_slow_present_line(char *out, size_t n)
{
    // the job the last handoff waited for: the slot it left free (g_next)
    if (!g_async_ok) return false;
    const gpu3ds::PresentMarks &m = g_slot[g_next].marks;
    if (m.frames <= 0) return false;
    int w = snprintf(out, n, "[slow-present] the job waited for: %.1f ms;", m.end);
    // a frame built before its vblank: "built" is when the build ended and its
    // waits began (vblank, then present), "begun" when they ended
    for (int f = 0; f < m.frames && w > 0 && (size_t)w < n; ++f) {
        const char *name = m.frames == 2 ? (f ? "tick" : "mid") : "frame";
        if (m.built[f] > 0.0f)
            w += snprintf(out + w, n - (size_t)w, " %s built %.1f (queue %.1f), begun %.1f (vblank %.1f,"
                          " present %.1f; gpu before %.1f), submitted %.1f;",
                          name, m.built[f], m.queue_wait[f], m.begun[f], m.vblank_wait[f],
                          m.present_wait[f], m.gpu_before[f], m.submitted[f]);
        else
            w += snprintf(out + w, n - (size_t)w,
                          " %s begun %.1f (vblank %.1f, queue %.1f, present %.1f; gpu before %.1f),"
                          " submitted %.1f;",
                          name, m.begun[f], m.vblank_wait[f], m.queue_wait[f], m.present_wait[f],
                          m.gpu_before[f], m.submitted[f]);
    }
    return w > 0;
}

void card3ds_wait_inputs()
{
    if (!g_inflight) return;
    const u64 t0 = svcGetSystemTick();
    g_card3ds_wait = "the present worker's inputs (card3ds_wait_inputs)";
    LightEvent_Wait(&g_inputs_ev);
    g_card3ds_wait = nullptr;
    __dmb();
    g_wait_inputs_ticks += svcGetSystemTick() - t0;
}

void card3ds_wait_idle()
{
    if (!g_inflight) return;
    const u64 t0 = svcGetSystemTick();
    g_card3ds_wait = "the present worker's job (card3ds_wait_idle)";
    LightEvent_Wait(&g_done_ev);
    g_card3ds_wait = nullptr;
    __dmb();
    g_wait_idle_ticks += svcGetSystemTick() - t0;
    g_inflight = false;
}

void card3ds_present_timing_begin(bool async)
{
    if (async && g_async_ok) {
        g_prep_start = svcGetSystemTick();
        g_prep_on = true;
        return;
    }
    card3ds_wait_idle();
    gpu3ds::present_timing_begin();
}

void card3ds_picture_sigs(uint64_t fb_sig, uint64_t other_sig)
{
    g_sig_next[0] = fb_sig;
    g_sig_next[1] = other_sig;
}

void card3ds_other_strips(const uint32_t *strips)
{
    g_strips_next = strips;
}

void card3ds_other_affine(const SubAffine *aff)
{
    g_aff_next = aff && aff->on ? aff : nullptr;
}

void card3ds_other_fade(int evy, bool white)
{
    g_other_evy_next = evy;
    g_other_white_next = white;
}

void card3ds_present(const ntr::Framebuffer &fb, const uint32_t *other, int other_stride,
                     bool main_lower, bool async)
{
    {
        // The two lines every 120 frames are a profile's (GAME3DS_GX_PROFILE, in
        // hardware_env.txt); a player's log goes without them.
        static const bool xform = getenv("GAME3DS_GPU_XFORM") != nullptr;
        static const bool gt = getenv("GAME3DS_GX_PROFILE") != nullptr;
        const unsigned a = g_xform_n[0], b = g_xform_n[1], c = g_xform_n[2];
        if (xform && a + b + c >= 120) {
            if (gt)
                printf("[gpu-xform] kept frames: %u from the record, %u record not drawn, %u without one\n",
                       c, b, a);
            g_xform_n[0] -= a;
            g_xform_n[1] -= b;
            g_xform_n[2] -= c;
        }
        char line[640];
        if (xform && gpu3ds::rec_prof_line(line, sizeof line)) printf("%s\n", line);
        if (gt && gpu3ds::gpu_time_line(line, sizeof line)) printf("%s\n", line);
        if (gpu3ds::gpu_tag_line(line, sizeof line)) printf("%s\n", line);   // the pass profile
    }
    if (!async || !g_async_ok) {
        card3ds_wait_idle();
        gpu3ds::present_timing_prepared();
        Job j;
        take_job(j, fb, other, other_stride, main_lower);
        present_body(j, [] {});
        gpu3ds::present_timing_end();
        return;
    }
    const u64 t0 = svcGetSystemTick();
    // the next job, taken into the slot core 2 is not presenting, BEFORE the
    // wait for the one it is (g_slot)
    Job &j = g_slot[g_next];
    j.timed = g_prep_on;
    j.prep_ticks = g_prep_on ? t0 - g_prep_start : 0;
    g_prep_on = false;
    j.bucket = g_interval & 1;
    if (j.timed) ++g_bucket[j.bucket].handed;
    take_job(j, fb, other, other_stride, main_lower);
    if (j.kept) {
        // gx.cpp's list carries on in the job's old buffer; a list other
        // than gx.cpp's own (SM64DS_TEX_ONLY's selection) is copied.
        if (ntr::gx_take_triangles(j.tris) != j.held.tris)
            j.tris.assign(j.held.tris, j.held.tris + j.held.count);
        if (j.held.count > j.tris.size()) j.held.count = j.tris.size();
        j.held.tris = j.tris.data();
    }
    {
        const u64 w0 = svcGetSystemTick();
        card3ds_wait_idle();
        if (svcGetSystemTick() - w0 > SYSCLOCK_ARM11 / 1000) g_core2_behind = 60;
        else if (g_core2_behind) --g_core2_behind;
    }
    g_run = &j;
    g_next ^= 1;
    g_async_used = true;
    ++g_jobs;
    LightEvent_Clear(&g_inputs_ev);
    LightEvent_Clear(&g_done_ev);
    g_inflight = true;
    __dmb();
    LightEvent_Signal(&g_go);
    g_handoff_ticks += svcGetSystemTick() - t0;
}

gpu3ds::PresentTiming card3ds_take_present_timing(bool final, unsigned *expected, bool *async)
{
    *async = g_async_used;
    if (!g_async_used) {
        const gpu3ds::PresentTiming pt = gpu3ds::take_present_timing();
        *expected = pt.frames;
        return pt;
    }
    if (final) card3ds_wait_idle();
    const unsigned prev = (g_interval + 1) & 1, cur = g_interval & 1;
    LightLock_Lock(&g_bucket_lock);
    gpu3ds::PresentTiming pt = g_bucket[prev].pt;
    unsigned handed = g_bucket[prev].handed;
    g_bucket[prev] = Bucket{};
    if (final) {
        merge(pt, g_bucket[cur].pt);
        handed += g_bucket[cur].handed;
        g_bucket[cur] = Bucket{};
    }
    LightLock_Unlock(&g_bucket_lock);
    ++g_interval;
    if (final) g_async_used = false;
    *expected = handed;
    return pt;
}

void card3ds_async_report(double ms, unsigned frames)
{
    if (!g_jobs || !frames) return;
    printf("[perf] present_async: %u jobs, game thread per frame: handoff %.3f ms (of which "
           "wait_idle %.3f), wait_inputs %.3f ms\n",
           g_jobs, (double)g_handoff_ticks * ms / frames, (double)g_wait_idle_ticks * ms / frames,
           (double)g_wait_inputs_ticks * ms / frames);
    g_handoff_ticks = g_wait_idle_ticks = g_wait_inputs_ticks = 0;
    g_jobs = 0;
}

ntr::SubFramebuffer *card3ds_capture_picture(unsigned seq)
{
    const int slot = seq & 1;
    return seq && g_cap_pic[slot] && g_cap_pic_for[slot] == seq ? g_cap_pic[slot] : nullptr;
}

void card3ds_take_settle_ticks(unsigned long long out[2])
{
    out[0] = g_settle_ticks[0];
    out[1] = g_settle_ticks[1];
    g_settle_ticks[0] = g_settle_ticks[1] = 0;
    if (g_cap_jobs) {
        const double ms = 1000.0 / (double)SYSCLOCK_ARM11;
        printf("[perf] capture: %u written by the present, the fence waited %.2f ms each\n", g_cap_jobs,
               (double)g_cap_fence_ticks * ms / g_cap_jobs);
    }
    g_cap_jobs = 0;
    g_cap_fence_ticks = 0;
}

void card3ds_menu_bars(bool menu) { g_menu_frame = menu; }

bool card3ds_set_options(bool fps60, bool stereo, bool fsaa, Card3dsCounterArt counter_art)
{
    card3ds_wait_idle();
    const bool st = gpu3ds::set_stereo(stereo);
    const bool aa = gpu3ds::set_fsaa(fsaa && !st);
    if (fps60 != interp3ds_on()) {
        interp3ds_set_on(fps60);
        // the recorder keeps each id's factors only while they are paired
        ntr::gx_record_id_stats(interp3ds_wanted() ? 1 : 0);
    }
    g_counter_art = counter_art;
    g_ctr.t0 = 0;
    g_ctr.valid = false;
    printf("[options] 60 fps %s, 3D %s, anti-aliasing %s, FPS counter %s\n", interp3ds_on() ? "on" : "off",
           st ? "on" : "off", aa ? "on" : fsaa ? "waits (3D)" : "off", counter_art ? "on" : "off");
    return st;
}

float card3ds_stereo_slider()
{
    return gpu3ds::stereo_slider();
}

void card3ds_show_bottom(const uint32_t *px, int stride)
{
    card3ds_wait_idle();
    gpu3ds::frame_begin(0xFF000000u, gpu3ds::SCREEN_BOTTOM, false);
    const int slot = gpu3ds::overlay_upload(px, stride, 320, 240);
    gpu3ds::Viewport vp;
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.w = 320.0f;
    vp.h = 240.0f;
    gpu3ds::overlay_draw(slot, gpu3ds::OVERLAY_OPAQUE, vp);
    gpu3ds::frame_end();
}

void card3ds_options_closed()
{
    gpu3ds::screens_forget();
    g_ctr.t0 = 0;   // the pause is not a slow second
}

bool card3ds_screenshot(const char *path, bool bottom)
{
    card3ds_wait_idle();
    return gpu3ds::screenshot_bmp(path, bottom ? gpu3ds::SCREEN_BOTTOM : gpu3ds::SCREEN_TOP);
}

// ---- hal/gpu_raster.h ----
//
// The interface the rest of the HAL calls. The twod path (2D on the card) says
// "not ready" so callers keep the CPU 2D compositor; the menu layer is taken
// on any held frame; the fade is applied at present, limited by the brightness
// mask when the compositor sets one.
extern "C" {

void port_gpu_raster_configure(void) {}
int port_gpu_raster_active(void) { return g_up ? 1 : 0; }
int port_gpu_card_held(void) { return g_held.active ? 1 : 0; }
void port_gpu_raster_present_down(void) {}

void port_gpu_card_set_fade(int evy, int white)
{
    g_fade_evy = evy < 0 ? 0 : (evy > 16 ? 16 : evy);
    g_fade_white = white != 0;
}

void port_gpu_card_set_mask(const unsigned char *mask, int stride, int shown3d,
                            int clear_opaque)
{
    g_mask = mask;
    g_mask_stride = stride;
    g_mask_shown3d = shown3d != 0;
    g_mask_clear_opaque = clear_opaque != 0;
}

void *port_gpu_card_compose(const unsigned int *, int) { return 0; }
void *port_gpu_card_retained_view(int) { return 0; }
void *port_gpu_card_compose_inter(const unsigned int *, int) { return 0; }
int port_gpu_card_fade_recorded(void) { return g_fade_evy > 0 ? 1 : 0; }
void port_gpu_card_inter_fade(int, int) {}
void port_gpu_card_inter_fade_off(void) {}
double port_gpu_raster_readback_ms(void) { return 0.0; }

int port_gpu_twod_ready(void) { return 0; }
void port_gpu_twod_reset(void) {}
int port_gpu_twod_begin(const PortTwodFrame *, unsigned char *, int, const unsigned *) { return 0; }
void port_gpu_twod_rects(const unsigned *, int) {}
void port_gpu_twod_bm_rect(int, int, int, int) {}
int port_gpu_twod_live(void) { return 0; }
void port_gpu_twod_fold(unsigned *, int) {}
void port_gpu_twod_check(const unsigned *, int, const unsigned char *, int, int) {}

int port_gpu_layer_ready(void) { return (g_up && g_held.active && ntr::gx_card_overlay_active()) ? 1 : 0; }

unsigned *port_gpu_layer_begin(int w, int h, int stride)
{
    if (!port_gpu_layer_ready() || w < 1 || h < 1 || stride < w || h > ntr::SCREEN_H ||
        stride > ntr::SCREEN_W)
        return 0;
    for (int p = 0; p < 2; ++p)
        for (int y = 0; y < h; ++y)
            memset(&g_lay.px[p][(size_t)y * stride], 0, (size_t)w * sizeof(unsigned));
    g_lay.stride = stride;
    g_lay.live = false;
    return g_lay.px[0];
}

unsigned *port_gpu_layer2(void) { return g_lay.px[1]; }

int port_gpu_layer_end(int w, int h, int post)
{
    g_lay.live = false;
    if (!port_gpu_layer_ready()) return 0;
    // gpu_raster.cpp's rule: a plane-2 word over an opaque plane-1 pixel is
    // stale (plane 1 was written after it).
    g_lay.two = false;
    for (int y = 0; y < h; ++y) {
        const unsigned *r1 = &g_lay.px[0][(size_t)y * g_lay.stride];
        unsigned *r2 = &g_lay.px[1][(size_t)y * g_lay.stride];
        for (int x = 0; x < w; ++x) {
            if (!r2[x]) continue;
            if ((r1[x] >> 24) == 255u) r2[x] = 0; else g_lay.two = true;
        }
    }
    g_lay.w = w;
    g_lay.h = h;
    g_lay.post = post != 0;
    g_lay.live = true;
    ++g_n.layers;
    return 1;
}

}  // extern "C"
