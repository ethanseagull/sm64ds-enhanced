#pragma once
#include <3ds.h>

// Contiguous slices of the level loop's existing t0..t1 tick boundary.
enum class TickPhase {
    input, requests, actor, post_actor, fallback_render, frame_end,
    player_world, particles, wipe, gx, message, sub_screen, count
};
void phase_timers_tick_begin(u64 tick);
void phase_timers_tick_mark(TickPhase phase);
void phase_timers_tick_end(u64 tick);

// Slow frame log. A level frame longer than GAME3DS_SLOW_MS (250 by default,
// 0 = off) prints a "[slow]" line showing where the time went: outside the
// loop (HOME, sleep, APT), the tick and its biggest parts, the hand-off to core
// 2 (waiting for the last present), and what runs after (sound, swap). The
// title loop's (level -1) has no tick breakdown. Call after the frame's work,
// before the [perf] report resets the HAL counters.
bool phase_timers_slow_frame(int frame, int level, bool level_changed, u64 outside, u64 tick,
                             u64 handoff, u64 after);
// The title loop's: its frame's HAL phases from here (level -1 in the line)
void phase_timers_frame_begin();

// game/sub_async.cpp: engine B's scan-out on core 2. Join before anything
// can change engine B (the top of each frame).
void game3ds_sub_join(void);
// The level loop's join before the message compositor: an Old 3DS only
// collects a finished scan and never waits (sub_async.cpp no_wait)
void game3ds_sub_join_tick(void);
// At a scene change: join, then the scan-out's caches go back to the heap
void game3ds_sub_release(void);
// A dry frame (main_game.cpp's frame skip): its engine B scan-outs make no picture.
void game3ds_sub_set_dry(bool dry);
// The title loop's tick starts (the half rate's measure, sub_async.cpp)
void game3ds_sub_tick_begin(void);
void game3ds_sub_async_report(double ms);
// The newest async scan-out's pixels (SUB_W per row), or null if g_sub has the
// newest picture. game3ds_sub_materialize copies it into g_sub for readers that
// need g_sub; game3ds_sub_scanned notes an inline scan.
const uint32_t *game3ds_sub_latest(void);
// that picture as a number: equal numbers, equal pictures; 0 not known
unsigned long long game3ds_sub_latest_sig(void);
// a picture's number when it is the newest one (an async buffer, or the one an
// inline scan wrote), else 0
unsigned long long game3ds_sub_sig_of(const uint32_t *picture);
void game3ds_sub_materialize(void);
// an inline scan of fb is about to run / has run (phase_timers.cpp's wrap)
void game3ds_sub_scan_before(void);
void game3ds_sub_scanned(const void *fb);
// The sky strips made next to that picture (64x192, linear: DS x -32..-1 then
// 256..287), or null if there are none.
const uint32_t *game3ds_sub_strips(const uint32_t *picture);
// That picture's affine BG, left to the GPU (src/sub_affine.h: the picture
// holds the sprites alone), or null (card3ds_other_affine).
struct SubAffine;
const SubAffine *game3ds_sub_affine_of(const uint32_t *picture);
// That picture's whole-screen fade, left to the present (BLDY's evy, 0 none;
// *white its direction; card3ds_other_fade).
int game3ds_sub_fade_of(const uint32_t *picture, int *white);
// main_game.cpp: GAME3DS_SUB_WATCH's scene-loop checkpoints (-1: the base).
void game3ds_scene_watch(int at);

// A scene interlude runs inside the level loop's requests slice. Its elapsed
// time belongs there, but its actor calls are not extra level frames.
class PhaseTimersInterlude {
public:
    PhaseTimersInterlude();
    ~PhaseTimersInterlude();
    PhaseTimersInterlude(const PhaseTimersInterlude &) = delete;
    PhaseTimersInterlude &operator=(const PhaseTimersInterlude &) = delete;
};
