// GPU renderer for the DS geometry engine's output.
//
// ntr/gx.cpp decodes the game's command stream into screen-space triangles
// (ntr::GxTriangle), and this draws them with citro3d using the same rules as
// gx.cpp's CPU rasterizer: culling by winding and POLYGON_ATTR bits 6-7,
// alpha-0 texels discarded, vertex colour times texture, translucent polygons
// in a second pass after the opaque ones, and the DS's depth test with its tie
// rule. It also handles shadow volumes, toon/highlight, fog and the 2D overlays.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sub_affine.h"

namespace ntr { struct GxTriangle; struct GxFog; struct GxRecord; struct GxRecMatrix; struct GxRecVertex; }

namespace gpu3ds {

// Where the DS's 256x192 picture lands on the screen, in screen pixels. The
// default fills the top screen's height at 4:3 (320x240, centred).
struct Viewport {
    float x = 40, y = 0, w = 320, h = 240;
};

bool init();
void shutdown();

// The two screens: top 400x240 and bottom 320x240. The bottom target is made
// the first time something draws there (until then the console may be using it).
enum Screen { SCREEN_TOP = 0, SCREEN_BOTTOM = 1 };
void screen_size(Screen s, int *w, int *h);

// A frame is frame_begin, draws, frame_end (which presents). Draws go to the
// screen named at frame_begin or the last frame_target (which clears it first).
// Every screen drawn in the frame gets presented.
//
// FSAA (options menu, or GAME3DS_FSAA in env.txt, =2 for 2x both ways; off by
// default): a frame begun with `scene` on the top screen draws into a target 2x
// wide (or 2x both ways) until resolve(), which averages it down into the
// normal 400x240 target. Anything drawn after that (2D, fades) is drawn at 1x.
// resolve() does nothing on other frames.
void frame_begin(uint32_t clear_argb, Screen s = SCREEN_TOP, bool scene = false);
void frame_target(Screen s, uint32_t clear_argb, bool scene = false);
void resolve();
// The next frame_begin keeps this frame's overlay uploads, so the second frame
// of a job (in-between then tick) can redraw them without uploading again.
void frame_keep_overlays();
// Whether FSAA is on (its target made and wanted).
bool fsaa_on();
// Between frames: FSAA 2x across on or off; its target (1 MB + 0.8 MB of
// VRAM) is made the first time and kept. Returns fsaa_on().
bool set_fsaa(bool on);
// Stereo 3D, toggled between frames. Turning it on makes the right eye's
// target the first time and enables 3D (gfxSet3D). While on, the top screen is
// drawn once per eye (set_eye(1) for the right, set_eye(0) for the left). The
// record's 3D geometry is shifted by the slider amount and the 2D sits at screen
// depth. No FSAA with 3D on.
bool set_stereo(bool on);
bool stereo_on();
void set_eye(int eye);
float stereo_slider();
// citro3d's GPU drawing time per frame over the last 120 frames, once each
// (false when no new one is ready). For the game thread to print.
bool gpu_time_line(char *out, size_t n);
void draw(const ntr::GxTriangle *tris, size_t count, const Viewport &vp = Viewport());
// Like draw(), but from gx.cpp's record instead of its triangles: the raw
// vertices, transformed, clipped and culled on the GPU (shaders/gx_rec.v.pica)
// with the same passes and state. Returns false (draws nothing) if the record
// doesn't fit, and the caller draws the triangles instead. `mtx` replaces
// rec.mtx and `verts` replaces rec.v (interpolation's blended values).
bool draw_record(const ntr::GxRecord &rec, const Viewport &vp = Viewport(),
                 const ntr::GxRecMatrix *mtx = nullptr, const ntr::GxRecVertex *verts = nullptr);
// Whether draw_record can take a record of `verts` vertices and `tris`
// triangles (its buffers and 16-bit indices), asked before taking one.
bool record_fits(size_t verts, size_t tris);
// GPU3DS_REC_PROF=1: draw_record's time by part over the last 120 frames,
// once each (false when no new one is ready). For the game thread to print.
bool rec_prof_line(char *out, size_t n);
void frame_end();
// Frames built ahead. A 1-vblank frame is built while the GPU is still drawing
// the previous one: frame_begin doesn't wait for the queue, and the frame's
// queue entries are held until ahead_flush, which waits for the last frame,
// runs the transfers and hands the rest to the GPU. frame_end calls it if the
// caller didn't. When ahead_ready is true, callers should do their
// overlay_transfer calls after frame_begin. GAME3DS_AHEAD=0 disables it.
bool ahead_ready();
void ahead_flush();

// The 2D picture over (or instead of) the 3D. px is 0xAARRGGBB, stride pixels
// a row, w x h used (max 512x256), drawn into the same rectangle as the 3D.
//   OVERLAY_OPAQUE      the whole picture as is (the CPU drew the 3D into it)
//   OVERLAY_KEPT        ntr/gx.h's kept-frame rule: alpha FF draws in front of
//                       the 3D, FE only where no 3D drew, 00 not at all
//   OVERLAY_PREMUL      premultiplied: c + dst * (1 - a) (the menu layer)
//   OVERLAY_KEPT_BLEND  OVERLAY_KEPT, with alpha below FE blended like PREMUL
// Each call uses its own texture (up to kMaxOverlays a frame) because the GPU
// only reads them when the frame is submitted.
enum OverlayMode { OVERLAY_OPAQUE = 0, OVERLAY_KEPT = 1, OVERLAY_PREMUL = 2, OVERLAY_KEPT_BLEND = 3 };
constexpr int kMaxOverlays = 4;
void draw_overlay(const uint32_t *px, int stride, int w, int h, OverlayMode mode,
                  const Viewport &vp = Viewport());
// draw_overlay split in two, for a present that reads all its inputs first and
// draws later (the core 2 present): overlay_upload swizzles px into the next
// overlay texture and returns its slot (-1 if nothing to draw or no slots
// left), overlay_draw draws a slot. Both after frame_begin.
int overlay_upload(const uint32_t *px, int stride, int w, int h);
void overlay_draw(int slot, OverlayMode mode, const Viewport &vp = Viewport());
// Leaving a screen as it is. citro3d doesn't transfer or swap a target that
// wasn't drawn, so the screen keeps showing its last picture. overlay_key
// turns a transferred slot into a number (slot and write count, 0 otherwise).
// screen_mark records what a screen was drawn with; frame_target,
// screens_forget (after HOME or sleep) and a key of 0 clear it. screen_holds
// says the screen still shows `key`.
uint64_t overlay_key(int slot);
bool screen_holds(Screen s, uint64_t key);
void screen_mark(Screen s, uint64_t key);
void screens_forget();
// Sharp scaling for a picture shown at a non-integer scale: overlay_sharpen
// (before frame_target for that screen; draws the slot at 2x into its own
// texture, only if the slot changed), then overlay_draw_sharp (bilinear into
// vp). False means it's unavailable (GAME3DS_SHARP=0 or no VRAM), so draw the
// slot normally.
bool overlay_sharpen(int slot, int w, int h);
void overlay_draw_sharp(const Viewport &vp, OverlayMode mode = OVERLAY_OPAQUE);
// Draw part of a slot's picture into vp: columns u0..u1 and rows v0..v1
// (u1 < u0 mirrors). Used for the menu pillarbox fill.
void overlay_draw_part(int slot, OverlayMode mode, const Viewport &vp, float u0, float v0,
                       float u1, float v1);
// A small picture (up to 128x32) in its own texture, drawn premultiplied into
// vp: the FPS counter. Re-uploaded only when `version` changes. Inside a frame.
void hud_draw(const uint32_t *px, int stride, int w, int h, unsigned version, const Viewport &vp);
// Engine B's affine BG (sub_affine.h): its picture (re-uploaded when a.ver
// changes) drawn premultiplied over the screen picture vp shows, each pixel
// the texel the DS's matrix points at (nearest; repeated or transparent past
// the map, per a.wrap). Inside a frame.
void affine_draw(const SubAffine &a, const Viewport &vp, int src_w, int src_h);
// Same as overlay_upload but through the GPU's transfer engine, for a picture
// in linear memory: 16-byte aligned, up to 512 wide (multiple of 8), height a
// multiple of 8. Otherwise returns -1 and the caller uses overlay_upload. Call
// before frame_begin (after transfers_begin); it waits for the copy, so px can
// be reused once it returns. In a frame built ahead, call it after frame_begin
// instead; px is free once ahead_flush returns. GPU3DS_NOTRANSFER=1 refuses
// all of them.
// `sig` identifies the picture (equal numbers = equal pictures, 0 unknown). If
// the slot already holds that picture, nothing is sent. GPU3DS_OVERLAY_HASH=1
// compares by hashing instead, GPU3DS_OVERLAY_ALWAYS=1 always sends,
// GPU3DS_OVERLAY_VERIFY=1 hashes too and counts disagreements.
void transfers_begin();
int overlay_transfer(const uint32_t *px, int stride, int w, int h, uint64_t sig = 0);
void overlay_transfers_free();

// The DS rear plane (CLEAR_COLOR, fogged) over vp only; call right after
// frame_begin, which clears the whole target black.
void draw_rear_plane(uint32_t argb, const Viewport &vp);

// The DS brightness fade over everything drawn so far: evy 0..16 towards black
// or white, same math as the PC compositor's cp_fade. evy can be fractional
// for the smoothed fades.
void draw_fade(float evy, bool white, const Viewport &vp = Viewport());

// The same fade, limited by the compositor's brightness mask (see
// gpu_raster.h). Per pixel: bit 0 fades it if the 3D shows there, bit 1 if the
// backdrop does, bit 2 fades the 2D pixel there, bits 3/4 mark a 2D pixel in
// front of / behind the 3D. Whether 3D shows is read from depth, so the main
// target must still hold this frame's 3D. Mask is w x h, `stride` bytes a row.
void draw_fade_masked(int evy, bool white, const uint8_t *mask, int stride, int w, int h,
                      bool shown3d, bool clear_opaque, const Viewport &vp);
// draw_fade_masked in two steps (like overlay_upload): upload the mask (false
// if there's none, then fade_masked_draw fades everything), then the fade.
bool fade_mask_upload(const uint8_t *mask, int stride, int w, int h, bool shown3d,
                      bool clear_opaque);
void fade_masked_draw(int evy, bool white, const Viewport &vp);

// Outside a frame: draw the triangles into an offscreen SCREEN_W x SCREEN_H
// picture (no 2D, no fade), wait for the GPU and read it back. 0xAARRGGBB, top
// row first, alpha is coverage. Valid until the next call; null if the target
// couldn't be made. Only [x0, x1) x [y0, y1) is made: readback_area makes more
// of the same picture without drawing it again (both clip to the picture).
const uint32_t *readback(const ntr::GxTriangle *tris, size_t count, int *stride,
                         int x0 = 0, int y0 = 0, int x1 = 0x7FFF, int y1 = 0x7FFF);
const uint32_t *readback_area(int x0, int y0, int x1, int y1);
// The same picture as readback's from gx.cpp's record (draw_record), for a lean
// frame whose triangles are not made; null if the record can't be drawn.
const uint32_t *readback_record(const ntr::GxRecord &rec, int *stride, int x0 = 0, int y0 = 0,
                                int x1 = 0x7FFF, int y1 = 0x7FFF);
// THE DISPLAY CAPTURE AS THE FRAME IS PRESENTED (card3ds' deferred capture).
// Inside a frame, after its screens: the DS picture drawn again at 1:1 into
// readback's target (the rear plane, the record, the 2D overlay slot in `mode`,
// no fade) and its copy to linear memory queued. frame_end waits for it
// (before its vblank wait in a frame built ahead) and calls done(lin, rows,
// arg) with that copy: RGBA8, picture column x is buffer row x of `rows`
// entries, picture row y is entry rows - 1 - y. capture_finish does the same
// for one still queued (nothing when none is). False: nothing was queued.
bool capture_queue(const ntr::GxRecord &rec, uint32_t rear_argb, int ov_slot, OverlayMode mode,
                   void (*done)(const uint32_t *lin, int rows, void *arg), void *arg);
void capture_finish();

// Toon table for this frame's mode-2 polygons: shade 0 none, 1 toon (entry
// replaces the vertex colour), 2 highlight (vertex red modulates, entry is
// added). rgb is 32 x 3 entries, 0..255. Call after frame_begin; frame_end
// clears it.
void set_toon(int shade, const float *rgb);

// This frame's fog (copied), or null. wdepth says whether the DS density comes
// from z or w. Fogged polygons are blended towards the fog colour per vertex
// (gx.cpp does it per pixel; per vertex is close enough). Call after
// frame_begin; frame_end clears it.
void set_fog(const ntr::GxFog *fog, bool wdepth);

// Frame pacing: each frame_begin waits until n vblanks have passed since the
// last frame began (none if the frame is already late). 1 = 60 fps.
void set_vblanks_per_frame(int n);
// Overrides it for the frames begun from here on (0 = no override): the
// interpolated level loop presents two frames a tick, one vblank each.
void set_vblanks_override(int n);
int vblanks_per_frame();

// Exclusive wall-clock zones for preparing and presenting a frame, in system
// ticks. Queue wait includes the previous submission's GPU work; this doesn't
// measure GPU time itself.
enum PresentZone {
    P_OTHER, P_PREPARE, P_TARGETS, P_EFFECTS, P_PACE_WAIT, P_VBLANK_WAIT, P_GPU_QUEUE_WAIT, P_BEGIN_CPU,
    P_GEOMETRY, P_TEXTURES, P_DRAW_SUBMIT, P_OVERLAY_SUBMIT,
    P_OVERLAY_SWIZZLE, P_OVERLAY_FLUSH, P_VBO_FLUSH, P_FRAME_SUBMIT, P_COUNT
};
// The zone the present is in now (a PresentZone), for the watchdog's report
// (game/watchdog3ds.cpp) from another thread.
int present_zone();
struct PresentTiming {
    uint64_t ticks[P_COUNT] = {};
    uint64_t total = 0;
    unsigned frames = 0;
    // Pacing: time between present_timing_end calls, i.e. the displayed frame
    // time. pace_vbl[n] counts frames that took n+1 vblanks (last bucket is 5+). A
    // 30 fps level should have everything in pace_vbl[1].
    unsigned pace_frames = 0;
    unsigned pace_vbl[5] = {};
    uint64_t pace_span = 0, pace_worst = 0;
    // The 3D the presented frames carried (Stats summed at present_timing_end):
    // a slow window is told apart from a heavy one by these.
    uint64_t tris_in = 0, tris_drawn = 0, draw_calls = 0;
    unsigned tris_max = 0, draws_max = 0;
};
void present_timing_begin();
void present_timing_prepared();
void present_timing_end();
// For a present that runs on a different thread than its preparation (the core
// 2 present): open the interval there, charging the game thread's prep time
// (`prep_ticks`) to P_PREPARE and the total.
void present_job_begin(uint64_t prep_ticks);
PresentTiming take_present_timing();  // consume the completed interval

// Present timeline of one timed job, in ms from its start, for the [slow]
// line: when each frame began (after its waits) and was submitted, the waits
// inside the begin, and the GPU time citro3d reported. For frames built before
// their vblank, `built` is when the build ended and `begun` is right before
// submit.
struct PresentMarks {
    int frames = 0;
    float begun[2] = {}, submitted[2] = {}, built[2] = {};
    float vblank_wait[2] = {}, queue_wait[2] = {}, present_wait[2] = {};
    float gpu_before[2] = {};
    float end = 0;
};
PresentMarks take_present_marks();

// GPU time by tag (for GAME3DS_GPU_PASS_PROFILE): frames from here on carry
// `tag` (0..7), and each frame's GPU time is filed under it. gpu_tag_line
// prints the GPU ms per tag every 1200 tagged frames.
void set_frame_tag(int tag, const char *name);
bool gpu_tag_line(char *out, size_t n);

// A new job's record: draw_record uploads it again (it keeps the last upload
// for the job's second frame when that frame draws the same vertices).
void record_upload_forget();

// Drop every cached texture (call when gx_texture_generation() changes).
void textures_invalidate();

// Write a screen as last presented to a 24-bit BMP (400x240 or 320x240).
bool screenshot_bmp(const char *path, Screen s = SCREEN_TOP);

struct Stats {
    unsigned tris_in, tris_drawn, tris_culled, draw_calls, tex_uploads, tex_cached;
    unsigned z_clamped;   // vertex z outside [0,1] (see draw)
};
const Stats &stats();

}  // namespace gpu3ds
