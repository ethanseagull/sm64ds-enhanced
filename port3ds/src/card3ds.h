// The 3DS "card": the GPU backend the PC port's HAL talks to.
//
// On PC the card is hal/gpu_raster.cpp (Direct3D 11). It registers a backend
// with ntr/gx.cpp and exports hal/gpu_raster.h's C interface, which the rest of
// the HAL calls. card3ds.cpp is the same interface on top of citro3d (gpu3ds),
// and gpu_raster.cpp / gpu_present.cpp aren't built.
//
// Frame protocol (from ntr/gx.h):
//   gx_card_present_allow(1); gx_clear_fill(fb)   fb becomes a transparent
//                                                 overlay, the card says "keep"
//   ... render walk ..., gx_render(fb)            the card keeps the 3D
//   ... HAL 2D compositor writes fb ...           only its own pixels
//   card3ds_present(fb)                           3D, then the overlay, then the
//                                                 fade
// A frame the card didn't keep is presented as the CPU picture in fb.
#pragma once

#include <stdint.h>

#include "gpu3ds.h"

namespace ntr { struct Framebuffer; struct SubFramebuffer; }

// Bring up gpu3ds and register the backend with ntr/gx.cpp.
bool card3ds_init();
void card3ds_shutdown();

// Composite and present one frame from fb on the top screen, or the bottom one
// when main_lower (engine A on the lower screen). `other`, if given, is the
// other engine's finished 256x192 picture (0xAARRGGBB, other_stride a row),
// shown on the other screen.
//
// With `async` (the level loop) the frame is handed to a worker and presented
// while the caller runs its next tick. The caller must call
// card3ds_wait_inputs() before writing fb, the menu layer, the brightness mask
// or engine B's picture again, and card3ds_wait_idle() before leaving the
// loop. Without a worker, or async false, it presents before returning.
void card3ds_present(const ntr::Framebuffer &fb, const uint32_t *other = nullptr,
                     int other_stride = 0, bool main_lower = false, bool async = false);
// IDs for the next present's two pictures (fb, other) from whoever made them:
// equal numbers mean equal pictures, 0 means unknown (always sent). A picture
// whose texture already holds that number isn't sent again. Used once.
void card3ds_picture_sigs(uint64_t fb_sig, uint64_t other_sig);
// Sky strips for the next present's `other` picture (64x192, linear: DS x
// -32..-1 then 256..287), drawn in a menu's side bars instead of the edge
// column. Null for the old bars. Used once.
void card3ds_other_strips(const uint32_t *strips);
// The next present's `other` picture is sprites only, and this describes its
// affine BG (sub_affine.h): the GPU draws the rotated BG under the sprites.
// Null to use the picture as is. Used once.
struct SubAffine;
void card3ds_other_affine(const SubAffine *aff);
// The next present's `other` picture was scanned without engine B's
// whole-screen fade (game3ds_sub_fade_of): evy 0..16 towards white or black.
// The present draws it, smoothed to 60 fps. 0 for none. Used once.
void card3ds_other_fade(int evy, bool white);
void card3ds_wait_inputs();
// For a slow tick's [slow] line: the timeline of the core 2 job its hand-off
// waited for, as one "[slow-present]" line. False without a core 2 present or
// before any job has run.
bool card3ds_slow_present_line(char *out, size_t n);
void card3ds_wait_idle();
// The picture of capture `seq` (ntr::ppu_capture_shown) as engine B's scan-out
// would draw it, when the card made that capture and still holds it; null
// otherwise. Holds until the capture two after it.
ntr::SubFramebuffer *card3ds_capture_picture(unsigned seq);

// Present timing. Begin before preparing a frame for card3ds_present, with the
// same `async`. take returns the interval to report; with async presents
// that's the previous one (its job is surely done), or both with `final`
// (after waiting for the last job). `expected` is the frames handed off in it,
// `async` whether it came from the worker.
void card3ds_present_timing_begin(bool async);
gpu3ds::PresentTiming card3ds_take_present_timing(bool final, unsigned *expected, bool *async);
// The game thread's side of the async present ([perf] present_async), per
// frame over `frames`; resets.
void card3ds_async_report(double ms, unsigned frames);

// Write a screen as last presented to a BMP.
bool card3ds_screenshot(const char *path, bool bottom = false);
// This frame is a menu's (title loop, star select, the title after quitting),
// so fill the top screen's side bars as GAME3DS_BARS says. Set before the
// frame's present.
void card3ds_menu_bars(bool menu);

// Time spent in settles since the last call, in system ticks: [0] the GPU
// readback (draw, wait, copy, untile), [1] the resolve under the overlay.
void card3ds_take_settle_ticks(unsigned long long out[2]);

// Options menu (options3ds.cpp). card3ds_set_options waits for the present to
// be idle, then sets in-between frames, stereo 3D, anti-aliasing (only asked
// for with 3D off) and the FPS counter: `counter_art` draws the counter picture
// for a given fps (premultiplied, max 128x32), shown bottom-left of the top
// screen; null for no counter. Returns whether 3D is on (false if it was asked
// for but its target couldn't be made).
typedef void (*Card3dsCounterArt)(unsigned fps, uint32_t *px, int stride, int *w, int *h);
bool card3ds_set_options(bool fps60, bool stereo, bool fsaa, Card3dsCounterArt counter_art);
// The 3D slider, 0..1 (GAME3DS_STEREO_SLIDER=x in env.txt stands in for it).
float card3ds_stereo_slider();
// The paused game under the menu: one 320x240 picture on the bottom screen
// only (the top keeps its frame), after the present is idle.
// card3ds_options_closed: the next present draws both screens again.
void card3ds_show_bottom(const uint32_t *px, int stride);
void card3ds_options_closed();
