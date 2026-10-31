// The 3DS host program: boot, the frame loop, input and presenting. This is the
// 3DS version of port/tests/walk_window.cpp on the PC.
//
// Each boot step is logged to sdmc:/sm64ds3ds/game3ds_early.log (early_io.cpp)
// so a hang or crash can be traced. Settings come from sdmc:/sm64ds3ds/env.txt.
// The PC's savestates, debug menu and other env-driven diagnostics aren't ported.
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include <3ds.h>

#include "ntr/gx.h"
#include "ntr/ppu.h"
#include "card3ds.h"
#include "console_tee.h"
#include "gpu3ds.h"
#include "interp3ds.h"
#include "phase_timers.h"

void game3ds_save_start(void);   // game/save_async.cpp
extern "C" void game3ds_watchdog_start(void);   // game/watchdog3ds.cpp
extern "C" void game3ds_lid_start(void);        // game/lid3ds.cpp

void phase_timers_init();                    // game/phase_timers.cpp
void phase_timers_report(unsigned frames);

extern "C" {
void game3ds_early_mark(const char *what);   // game/early_io.cpp
extern signed char data_0209f2f8;            // the current level

// walk_window.cpp's own declarations of the same names
void port_boot_rom_pre_main(void);
void port_boot_rom_main_head(void);
void port_boot_rom_game_init_head(void);
void port_boot_rom_game_init_tail(void);
int  port_rom_main_enabled(void);
void port_rom_main_run(void);
void _ZN4Heap18InitializeRootHeapEv(void);
void _ZN4Heap18InitializeGameHeapEjPS_(unsigned size, void *root);
unsigned _ZN22ExpandingHeapAllocator10MemoryLeftEv(void *alloc);
extern int game3ds_old_budget;   // early_io.cpp
void port_fs_stream_files_over(unsigned bytes);   // hal/fs_names.cpp
extern void *data_020a0ea0;
extern void *data_020a0e9c;
extern void *data_020a0eac_c;
extern int data_0209b3ec[12];
extern unsigned char data_0209f2d8;
extern short data_02092144[];
void hal_fill_model_vtable(void);
void hal_fill_shadow_vtable(void);
void hal_fill_mmc_vtable(void);
void hal_fill_modelanim2_vtable(void);
void port_ov002_patch(void);
void port_cross_patch(void);
void port_cutscene_states_seat(void);
void port_kuppa_cmd_seat(void);
void __sinit_ov002_02100560(void); void __sinit_ov002_02100938(void);
void __sinit_ov002_02100adc(void); void __sinit_ov002_02100c50(void);
void __sinit_ov002_02100d44(void); void __sinit_ov002_02100e50(void);
void __sinit_ov002_02100ec4(void); void __sinit_ov002_02100f84(void);
void __sinit_ov002_02101064(void); void __sinit_ov002_02101478(void);
void __sinit_ov002_021014e4(void); void __sinit_ov002_02101588(void);
void __sinit_ov002_02101738(void); void __sinit_ov002_02101894(void);
void __sinit_ov002_02101900(void); void __sinit_ov002_02101968(void);
void __sinit_ov002_021019d0(void); void __sinit_ov002_02106e40(void);
void __sinit_ov002_02107118(void); void __sinit_ov002_021071f4(void);
void __sinit_ov002_02107298(void); void __sinit_ov002_02107304(void);
void __sinit_ov002_02107370(void); void __sinit_ov002_02107f88(void);
void __sinit_ov002_0210804c(void); void __sinit_ov002_02108094(void);
int  port_scene_env_want(void);
int  port_title_entry_armed(void);
int  port_title_entry_run(void);
int  port_scene_run(void);
const void *port_scene_framebuffer(void);
int  port_scene_begin(void *hwnd, int zoom);
void port_scene_tick(int frame, int tick_game);
extern int (*port_interlude_frame_hook)(int frame);
// hal/level_change.cpp: 1 (the default here) = the pause menu's quit crosses
// to the title in this process (the Game Over crossing); 0 = main_smoke.cpp's relaunch
extern int port_level_quit_crosses;
int  port_scene_finish(int frames_run);
void port_rom_frame_begin(const char *loop);
int  port_rom_frame_checked(int host, const char *reader);
void port_rom_frame_phase6(void);
void port_rom_frame_report(void);
int  port_title_entry_should_stop(void);
int  port_title_entry_commit(void);
// input (walk_window.cpp's scene_host_input_frame writes the same words)
extern char data_0209f49c[];   /* Ctrl: held */
extern char data_0209f49e[];   /* Ctrl: pressed this frame */
extern int data_020a0e58[];    /* PadData: u16 held, u16 pressed */
extern int data_020a0e5a[];
void port_host_keys_set(unsigned short raw);
void hal_menu_pad_seam(void);
void port_touch_force_press(int x, int y, int frames);
void game3ds_input_read(unsigned *raw_keys, int *touch_down, int *tx, int *ty);
void game3ds_cpad(int *x, int *y);
void game3ds_cstick(int *x, int *y);
// the ROM's own atan2, the one CheckInput builds the stick record's angle with
short _ZN4cstd5atan2E5Fix12IiES1_(int y, int x);
unsigned *hal_sub_screen_stacked_image(const unsigned *top);
const unsigned *hal_sub_screen_b_direct(void);
int  game3ds_main_loop(void);
int  port_title_entry_taken(void);
unsigned short port_input_probe_bits(int frame);
void port_input_probe_star_trigger(int frame);
void port_input_probe_trace_msg(int frame);
void hal_sub_screen_init(void *hwnd, int zoom);

// the level boot and frame (walk_window.cpp main, :13004-14122 and the loop)
extern int data_0209fc48;
extern unsigned char data_0209d660;
extern unsigned char data_0209f21c;
extern int data_0209f350[];
extern int data_0209214c[];
extern unsigned char data_02075650[], data_02075670[], data_02075690[];
extern unsigned char data_020a0e40[];
extern unsigned char data_0209caa0[];
extern void *data_0209f394[];
extern unsigned char data_0209f250;
extern void *data_0209f318;
extern int data_0208ee44;
extern int data_0209d50c;
extern unsigned short data_0209d534[];
extern unsigned char data_0209d4e8[];
extern int data_0209d51c;
extern unsigned char data_0209d4f0[4];
extern int data_0209f498[];
extern unsigned char data_0209f4a0[], data_0209f4a2[], data_0209f4a4[], data_0209f4a6[],
                     data_0209f4a8[], data_0209f4a9[], data_0209f4ac[];
extern unsigned char data_020a0de8[];   // TouchInfo[4]: touched, edge, x, y
void *port_stage_create(void);
void port_stage_a2_seat(void);
void *port_stage_a_boot(void *mc, int spawn_entrances);
void *port_stage_object(void);
void _ZN5Stage10LoadSkyboxEv(void *stage);
void _ZN5Stage7LoadFogEv(void *stage);
void _ZN5Stage10CheckInputEv(void);
void port_course_seat(void);
void hal_sub_camera_zone_live(void);
void hal_sub_camera_input(void);
void hal_sub_screen_frame_begin(void);
void hal_sub_screen_present(unsigned *dst, int w, int h);
int  hal_sub_screen_stacked(void);
void port_message_composite_engine_a(void *fb);
int  port_message_composite_signature(unsigned long long *sig);
void port_message_composite_memo(int on);
void func_0201ffcc(void);
void func_0203bc7c(void);
void func_02023498(void);
void func_020190b8(void);
void func_ov001_020aaf40(void);
void port_frame_phase2(void);
void port_frame_phase5(void);
int  port_scene_fade_pending(int *scene_id);
int  port_scene_request_release(const char *why);
void _ZN8dScene_c14StartSceneFadeEjjt(unsigned scene, unsigned param, unsigned short color);
extern unsigned short data_02092664;   // Scene::SetSceneToSpawn's pending id (0x187: none)
extern unsigned char data_02092660;    // Scene's "already spawned" latch
extern int data_020a4b6c[];           // the scene tree: {head node, ...}
extern int data_0209b454[];           // the persistent freeze-request word
extern unsigned int data_0209b464;    // this frame's copy of it
void port_scene_fade_clear(void);
int  port_fader_blend_state(int *evy, int *to_white);
int  port_fader_entrance_wipe_armed(void);
void port_fader_start_color(int frames, int to_end, unsigned short color);
void port_fader_wipe_render(void);
int  port_level_change_pending(void);
int  port_level_change_poll(void);
int  port_level_change_due(void);
int  port_level_hold_live(void);
int  port_level_hold_white(void);
void port_intro_bit_edge(void);
void port_actor_frame_begin(void);
void port_actor_frame(int level_camera);
void port_actor_frame_end(int frame, const char *loop);
unsigned port_stage_render_calls(void);
unsigned port_stage_behavior_calls(void);
void port_frame_clock_tick(void);
void port_stage_render_skybox(void *stage);
void port_stage_advance_anims(void *stage);
void port_stage_render_model(void *stage);
void port_stage_render_model_transparent(void *stage);
void _ZN11ShadowModel9RenderAllEv(void);
void port_cylinder_clsn_process(void);
void hal_render_player_world(void *player);
int  port_particle_phase5_begin(void);
void port_particle_phase5_end(void);
void port_gpu_twod_reset(void);
void sdat_host_tick(void);
}

namespace port { void comms_publish_pad(unsigned held); }   // hal/comms_conductor.cpp

namespace ntr {
void rt_irq_boot_state();
void rt_scanout_frame();
void rt_timer0_advance(unsigned ticks);
void gx_swap_apply();
}

#define STEP(what) do { game3ds_early_mark(what); printf("%s\n", what); } while (0)

// One frame of input, stored the way walk_window's scene_host_input_frame does.
// `raw` is the DS KEYINPUT word; the game's Ctrl word is the inverse of the
// ROM's mode-0 remap: punch <- A, jump <- B, crouch <- R, run <- Y,
// camera snap <- L, look <- X, plus D-pad, Start and Select.
static void apply_input(int frame)
{
    unsigned raw = 0;
    int down = 0, tx = 0, ty = 0;
    game3ds_input_read(&raw, &down, &tx, &ty);
    raw |= port_input_probe_bits(frame);
    unsigned short btn = (unsigned short)(raw & 0x00fc);
    if (raw & 0x0001) btn |= 0x0001;
    if (raw & 0x0002) btn |= 0x0002;
    if (raw & 0x0100) btn |= 0x0400;
    if (raw & 0x0800) btn |= 0x0800;
    if (raw & 0x0200) btn |= 0x4000;
    if (raw & 0x0400) btn |= 0x8000;
    static unsigned short btn_was, raw_was;
    port_host_keys_set((unsigned short)raw);
    *(unsigned short *)(data_0209f49c + 0) = btn;
    *(unsigned short *)(data_0209f49e + 0) = (unsigned short)(btn & ~btn_was);
    const unsigned short edge = (unsigned short)(raw & ~raw_was);
    *(unsigned short *)((char *)data_020a0e58 + 0) = (unsigned short)raw;
    *(unsigned short *)((char *)data_020a0e58 + 2) = edge;
    *(unsigned short *)((char *)data_020a0e5a + 0) = edge;
    hal_menu_pad_seam();
    btn_was = btn;
    raw_was = (unsigned short)raw;
    if (down)
        port_touch_force_press(tx, ty, 1);   // consumed by this frame's poll
}

// If sdmc:/sm64ds3ds/autotap exists, tap the middle of the bottom screen at
// frame 120. A quick unattended check that touch reaches the game.
static void autotap(int frame)
{
    static int on = -1;
    if (on < 0) {
        FILE *f = fopen("sdmc:/sm64ds3ds/autotap", "r");
        on = f != nullptr;
        if (f) fclose(f);
    }
    if (on && frame == 120) {
        STEP("input: autotap (128, 96)");
        port_touch_force_press(128, 96, 2);
    }
    // GAME3DS_TAP=frame:x:y[,frame:x:y...]: scripted taps at DS touch-screen
    // pixels, for a test with no stylus (the course-clear menu wants a touch).
    static const char *taps = getenv("GAME3DS_TAP");
    for (const char *p = taps; p && *p;) {
        int f = 0, x = 0, y = 0, n = 0;
        if (sscanf(p, "%d:%d:%d%n", &f, &x, &y, &n) != 3) break;
        if (f == frame) {
            printf("[tap] f%d (%d, %d)\n", frame, x, y);
            port_touch_force_press(x, y, 2);
        }
        p += n;
        if (*p == ',') ++p;
    }
}


// An env.txt was found: a test run (the regression runner or a hardware run),
// which also gets the title's and the exit's screenshots. A player's console
// has none.
static bool g_test_run;

// The frame reports below come every 120 frames on a test run, every 1800 on
// a player's console: a long session's log stays small (it was ~5 MB an
// hour) and still has the frame rate and the heap from before a crash.
static unsigned report_every(void) { return g_test_run ? 120 : 1800; }

// Frame timing in the 3DS's own clock (svcGetSystemTick): the game tick vs the
// present (citro3d submit plus the GPU and vblank waits). "kept" frames stayed
// on the GPU path; the rest were settled or refused. Printed every
// report_every() frames.
//
// The level loop presents on a worker thread, so "present" here is only the
// game thread's part, and the [perf] present lines are one report behind.
extern "C" u32 __ctru_heap_size, __ctru_linear_heap_size;
extern "C" char *fake_heap_start, *fake_heap_end;
extern "C" void alloc_census_dump(int frame) __attribute__((weak));

struct FrameClock {
    u64 tick, present;
    unsigned frames, kept;
    void report(int frame, bool final = false)
    {
        if (!frames && !final) return;
        const double ms = 1000.0 / (double)SYSCLOCK_ARM11;
        unsigned long long st[2];
        card3ds_take_settle_ticks(st);
        if (frames)
            printf("[perf] f%d: %u frames (%u kept on the card), tick %.2f ms (settles: readback "
                   "%.2f, resolve %.2f), present %.2f ms\n",
                   frame, frames, kept, (double)tick * ms / frames, (double)st[0] * ms / frames,
                   (double)st[1] * ms / frames, (double)present * ms / frames);
        if (frames) {
            // Heap high-water mark and linear heap left; the Old 3DS's 64 MB is tight.
            // The arena is fake_heap_start..end; the main thread's stack comes off the
            // start of __ctru_heap_size.
            const struct mallinfo mi = mallinfo();
            printf("[mem] f%d: heap arena %u KB of %lu KB (in use %u KB), linear free %lu KB of %lu KB\n",
                   frame, (unsigned)(mi.arena / 1024),
                   (unsigned long)((fake_heap_end - fake_heap_start) / 1024),
                   (unsigned)(mi.uordblks / 1024), (unsigned long)(linearSpaceFree() / 1024),
                   (unsigned long)(__ctru_linear_heap_size / 1024));
            if (alloc_census_dump) alloc_census_dump(frame);
        }
        unsigned expected = 0;
        bool async = false;
        const gpu3ds::PresentTiming pt = card3ds_take_present_timing(final, &expected, &async);
        if (pt.frames) {
            static const char *const names[] = {
                "other", "prepare", "targets", "effects", "pace_wait", "vblank_wait", "gpu_queue_wait", "begin_cpu",
                "geometry", "textures", "draw_submit", "overlay_submit", "overlay_swizzle",
                "overlay_flush", "vbo_flush", "frame_submit"
            };
            static_assert(sizeof names / sizeof *names == gpu3ds::P_COUNT, "present zones");
            u64 sum = 0;
            for (u64 ticks : pt.ticks) sum += ticks;
            printf("[perf] present: %u frames, total %.3f ms, outside %.3f ms, "
                   "gap_ticks %lld, expected_frames %u\n", pt.frames,
                   (double)pt.total * ms / pt.frames,
                   async || !frames ? 0.0 : ((double)present - (double)pt.total) * ms / frames,
                   (long long)pt.total - (long long)sum, async ? expected : frames);
            printf("[perf] present_parts:");
            for (int i = 0; i < gpu3ds::P_COUNT; ++i)
                printf(" %s %.3f", names[i], (double)pt.ticks[i] * ms / pt.frames);
            printf("\n");
            printf("[perf] gpu: per frame tris in %.0f drawn %.0f (max %u), draw calls %.0f "
                   "(max %u)\n", (double)pt.tris_in / pt.frames, (double)pt.tris_drawn / pt.frames,
                   pt.tris_max, (double)pt.draw_calls / pt.frames, pt.draws_max);
        }
        if (pt.pace_frames) {
            printf("[perf] pace: %u frames, %.1f fps, vblanks 1:%u 2:%u 3:%u 4:%u 5+:%u, "
                   "worst %.1f ms\n", pt.pace_frames,
                   1000.0 * pt.pace_frames / ((double)pt.pace_span * ms), pt.pace_vbl[0],
                   pt.pace_vbl[1], pt.pace_vbl[2], pt.pace_vbl[3], pt.pace_vbl[4],
                   (double)pt.pace_worst * ms);
        }
        card3ds_async_report(ms, frames);
        if (!frames) return;
        phase_timers_report(frames);
        tick = present = 0;
        frames = kept = 0;
    }
};

// Frame pacing comes from the game: data_0208ee44 is vblanks per tick, set by
// each scene (2 for 3D levels, 1 for most minigames). Anything outside 1..4
// means nothing has set it yet, so treat it as 2.
static int frame_divider(void)
{
    const int d = data_0208ee44;
    const int v = (d >= 1 && d <= 4) ? d : 2;
    static int said;
    if (v != said) {
        said = v;
        printf("[pace] %d vblank(s) per game tick (data_0208ee44 = %d)\n", v, d);
    }
    return v;
}

// The other screen. The HAL's stacked layout puts each engine where POWCNT1
// bit 15 says (engine A on the bottom on the title, for example). Engine B's
// half goes to the card for whichever screen engine A isn't on; engine A's 3D
// stays on the GPU. Null until the sub engine has been scanned out once.
const ntr::StackLayout *hal_screen_layout();
//
// On a plain layout engine B's half is just g_sub (hal_sub_screen_b_direct), so
// the card reads it directly, or the newest worker scan-out, and the whole
// stacked compose (~2.5 ms on hardware) is skipped. GAME3DS_SUB_DIRECT_CHECK=1
// composes anyway and counts differing pixels.
static const uint32_t *engine_b_picture(const ntr::Framebuffer &fb, bool *main_lower,
                                        int *stride)
{
    const ntr::StackLayout &lay = *hal_screen_layout();
    *main_lower = lay.main_lower != 0;
    *stride = ntr::SUB_W;
    static const bool check = getenv("GAME3DS_SUB_DIRECT_CHECK") != nullptr;
    if (const unsigned *direct = hal_sub_screen_b_direct()) {
        const uint32_t *latest = game3ds_sub_latest();
        const uint32_t *pic = latest ? latest : (const uint32_t *)direct;
        if (!check) return pic;
        game3ds_sub_materialize();
        const unsigned *img = hal_sub_screen_stacked_image(&fb.px[0][0]);
        static unsigned frames, diff;
        if (img) {
            const unsigned *b = img + (size_t)(lay.main_lower ? lay.top_y : lay.bottom_y) * lay.w +
                                lay.pan_x0;
            for (int y = 0; y < ntr::SUB_H; ++y)
                for (int x = 0; x < ntr::SUB_W; ++x)
                    diff += b[(size_t)y * lay.w + x] != pic[y * ntr::SUB_W + x];
        }
        if (++frames % 120 == 0) {
            printf("[sub-direct] %u frames: %u pixels differ from the stacked compose\n", frames,
                   diff);
            diff = 0;
        }
        return pic;
    }
    game3ds_sub_materialize();
    const unsigned *img = hal_sub_screen_stacked_image(&fb.px[0][0]);
    if (!img || lay.pan_x0 < 0 || lay.pan_x0 + ntr::SUB_W > lay.w) return nullptr;
    const int b_y = lay.main_lower ? lay.top_y : lay.bottom_y;
    *stride = lay.w;
    return img + (size_t)b_y * lay.w + lay.pan_x0;
}

// GAME3DS_SUB_WATCH=1: find where in a level frame engine B changes. Hashes
// engine B's registers, OAM, palette and VRAM when the scan-out is handed off
// and at each checkpoint after, and reports per region where it first changed.
// The scan-out's join only has to happen before the first such point.
namespace {
struct WatchRegion {
    const char *name;
    uintptr_t addr;
    size_t len;
};
const WatchRegion kWatch[] = {
    {"regs_b", 0x04001000, 0x70},      {"vramcnt", 0x04000240, 0x10},
    {"powcnt", 0x04000304, 0x4},       {"oam_b", 0x07000400, 0x400},
    {"pal_b", 0x05000400, 0x400},      {"bg_b", 0x06200000, 0x20000},
    {"obj_b", 0x06600000, 0x20000},    {"bank_c", 0x06840000, 0x20000},
    {"bank_d", 0x06860000, 0x20000},   {"bank_h", 0x06898000, 0x8000},
    {"bank_i", 0x068a0000, 0x4000},
};
constexpr int kWatchN = sizeof kWatch / sizeof *kWatch;
const char *const kWatchAt[] = {"tail", "buttons", "zone_live", "sub_begin", "pad", "input",
                                "requests", "actor", "player_world", "particles", "wipe", "gx",
                                "message"};
// The scene loop's checkpoints (title, file select): the HAL's phases inside
// port_scene_tick, from the wraps in phase_timers.cpp (game3ds_scene_watch).
const char *const kWatchAtScene[] = {"tail", "frame_begin", "phase2", "logic", "actor",
                                     "behavior", "render_walk", "phase5", "scanout", "clear",
                                     "gx_render", "msg2d", "sub_present"};
const char *const *g_watch_names = kWatchAt;
uint8_t g_watch_regs[0x70];
unsigned g_watch_logged;
constexpr int kWatchAtN = sizeof kWatchAt / sizeof *kWatchAt;
static_assert(sizeof kWatchAtScene / sizeof *kWatchAtScene == kWatchAtN, "scene checkpoints");
uint32_t g_watch_base[kWatchN];
bool g_watch_moved[kWatchN];
unsigned g_watch_count[kWatchN][kWatchAtN];
unsigned g_watch_frames;

uint32_t watch_hash(const WatchRegion &r)
{
    const uint32_t *p = (const uint32_t *)r.addr;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < r.len / 4; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

bool sub_watch_on()
{
    static const bool on = getenv("GAME3DS_SUB_WATCH") != nullptr;
    return on;
}

// at = -1: the top of the frame (a new base); else the checkpoint index
void sub_watch(int at)
{
    if (!sub_watch_on()) return;
    if (at < 0) {
        for (int i = 0; i < kWatchN; ++i) {
            g_watch_base[i] = watch_hash(kWatch[i]);
            g_watch_moved[i] = false;
        }
        memcpy(g_watch_regs, (const void *)0x04001000, sizeof g_watch_regs);
        return;
    }
    for (int i = 0; i < kWatchN; ++i) {
        if (g_watch_moved[i]) continue;
        if (watch_hash(kWatch[i]) != g_watch_base[i]) {
            g_watch_moved[i] = true;
            ++g_watch_count[i][at];
            if (i == 0 && at != kWatchAtN - 1 && g_watch_logged < 60) {
                ++g_watch_logged;
                printf("[sub-watch] regs_b changed by %s:", g_watch_names[at]);
                const uint16_t *now = (const uint16_t *)0x04001000;
                const uint16_t *was = (const uint16_t *)g_watch_regs;
                for (int w = 0; w < 0x38; ++w)
                    if (now[w] != was[w]) printf(" +%02x %04x->%04x", w * 2, was[w], now[w]);
                printf("\n");
            }
        }
    }
    if (at == kWatchAtN - 1 && ++g_watch_frames == 120) {
        for (int i = 0; i < kWatchN; ++i) {
            printf("[sub-watch] %-8s first changed at:", kWatch[i].name);
            for (int k = 0; k < kWatchAtN; ++k)
                printf(" %s %u", g_watch_names[k], g_watch_count[i][k]);
            printf("\n");
        }
        memset(g_watch_count, 0, sizeof g_watch_count);
        g_watch_frames = 0;
    }
}
bool g_scene_watch;   // scene_window_run is up: phase_timers.cpp's wraps watch
}  // namespace

// Checkpoints for the scene loop, called from the phase wraps
// (phase_timers.cpp). -1 is the base, taken right after the hand-off.
void game3ds_scene_watch(int at)
{
    if (g_scene_watch) sub_watch(at);
}

// gx.cpp frees decoded textures here, and the present on core 2 may still be
// uploading one, so let it finish first. It's a link wrap so every caller
// outside gx.cpp goes through it.
#ifdef NTR_WIDE_3DS
// Widen the object cull for the widescreen build. Camera::Render sets up the
// ROM's object clipper from the 4:3 aspect every frame, so actors in the extra
// side space got culled. hal_camera_widen_frustum rescales it to the real
// aspect, like the PC does after Camera::Render. On 3DS the Camera keeps GCC's
// vtable, so this is done as a link wrap instead. Not used on 4:3 scenes.
extern "C" void hal_camera_widen_frustum(void);
extern "C" int __real__ZN6Camera6RenderEv(void *self);
extern "C" int __wrap__ZN6Camera6RenderEv(void *self)
{
    const int r = __real__ZN6Camera6RenderEv(self);
    if (ntr::widescreen && !ntr::present_native())
        hal_camera_widen_frustum();
    return r;
}
#endif

// Whole display lists for gx.cpp's list memo. The ROM's two list senders
// (func_01ffde98 and func_0205a358) DMA lists to the FIFO in 0x1D8-byte
// chunks, which can start mid-command, so these tell gx.cpp about the whole
// list first. Link wraps.
extern "C" void __real_func_01ffde98(int ch, int src, int bytes);
extern "C" void __wrap_func_01ffde98(int ch, int src, int bytes)
{
    if (bytes > 0)
        ntr::gx_list_begin(reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(src)),
                           bytes / 4);
    __real_func_01ffde98(ch, src, bytes);
}
extern "C" void __real_func_0205a358(int ch, int src, int bytes, void (*cb)(int), int arg);
extern "C" void __wrap_func_0205a358(int ch, int src, int bytes, void (*cb)(int), int arg)
{
    if (bytes > 0)
        ntr::gx_list_begin(reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(src)),
                           bytes / 4);
    __real_func_0205a358(ch, src, bytes, cb, arg);
}

extern "C" void __real__ZN3ntr22gx_invalidate_texturesEv(void);
extern "C" void __wrap__ZN3ntr22gx_invalidate_texturesEv(void)
{
    card3ds_wait_idle();
    __real__ZN3ntr22gx_invalidate_texturesEv();
}

// At the end of a scene (the title, each level change) drop engine B's scan
// caches, and on Old 3DS also gx.cpp's decoded textures and the decoded sound
// waves. Otherwise each scene leaves 1-2 MB behind. Waves still playing are
// kept (sd_waves_trim).
unsigned long sd_waves_trim(void);           // hal/sdat/sdat.cpp
extern "C" int out3ds_sound_lock(void);      // game/out_3ds.cpp
extern "C" void out3ds_sound_unlock(int locked);
static void scene_caches_release()
{
    game3ds_sub_release();
    if (game3ds_old_budget) {
        ntr::gx_invalidate_textures();   // waits for the present (the wrap above)
        const int locked = out3ds_sound_lock();
        const unsigned long waves = sd_waves_trim();
        out3ds_sound_unlock(locked);
        printf("[mem] the decoded textures dropped, and %lu KB of decoded waves\n", waves >> 10);
    }
}

// The same release inside a level, when the heap runs low. The caches only
// empty at a scene change, and a long stay filled the Old 3DS's 32 MB heap
// (2026-10-29: 2.5 MB of new waves and textures in the castle basement, then a
// Toad's star jingle could not be decoded). Checked twice a second, between
// frames; the next frames decode what they draw again.
static void heap_valve(int frame)
{
    static int last = -1;   // the last release's frame: at most one each 10 s
    if (frame < last) last = -1;   // a new level loop counts from 0
    if (frame % 30 || (last >= 0 && frame - last < 600)) return;
    const size_t heap = (size_t)(fake_heap_end - fake_heap_start);
    const size_t used = mallinfo().uordblks;
    if (heap - used >= (size_t)(3u << 20) && linearSpaceFree() >= (512u << 10)) return;
    printf("[mem] f%d: low (heap %u KB of %u KB in use, linear free %lu KB): releasing the caches\n",
           frame, (unsigned)(used >> 10), (unsigned)(heap >> 10),
           (unsigned long)(linearSpaceFree() >> 10));
    last = frame;
    card3ds_wait_idle();
    scene_caches_release();
    printf("[mem] f%d: now %u KB in use\n", frame, (unsigned)(mallinfo().uordblks >> 10));
}

static bool g_cpu_raster;

// Painting/star select and other scene crossings block the outer level loop.
// They need the same input, presenting and APT polling as the title. Don't
// advance the ROM frame clock here; this is still inside one level tick.
//
// crossing_layout: called when a scene is about to spawn (and with -1 when the
// level comes back) to give the scene the screen layout it would have had
// from a fresh start. Without it the title after save and quit kept the
// level's wide layout and its bottom screen wasn't scaled to fit.
extern "C" int port_scene_layout_native(int scene);   // hal/scene_boot.cpp
extern "C" void (*port_crossing_scene_layout_hook)(int scene);
static void crossing_layout(int scene)
{
    const bool native = scene >= 0 && port_scene_layout_native(scene) != 0;
    ntr::set_present_native(native);
    printf("[cross] layout for %s: %s field\n", scene >= 0 ? "the scene" : "the level",
           native ? "the DS's own 4:3" : "the wide");
    // Coming back to the level: free what the crossing's scenes left behind (about
    // 5 MB after save and quit back into level 1).
    if (scene < 0) scene_caches_release();
}

extern "C" int port_message_composite_dry;   // hal/message_compositor.cpp
extern "C" int port_scene_live_id(void);       // hal/scene_boot.cpp: the scene path's id, or -1

// The running scene's id. port_scene_live_id knows the scenes whose behavior goes
// through dScene_c's own, and a minigame's doesn't (it answers -1 there); the
// scene tree's head is the scene's node, ActorBase::sceneNode at +0x14, with the
// actor id at +0x0c.
static int scene_now(void)
{
    const int live = port_scene_live_id();
    if (live >= 0) return live;
    const uintptr_t head = (uintptr_t)(unsigned)data_020a4b6c[0];
    return head ? *(const unsigned short *)(head - 0x14 + 0x0c) : -1;
}

// A minigame keeps the DS's speed when its frame takes longer than its tick, as
// DryPace below does for a level. The minigames tick every vblank, and a full
// 2D board composed on the CPU can take two or three, which played them in slow
// motion. So a tick that starts more than half a tick behind the game's clock
// runs the game and makes no picture (the screens keep the last one), up to
// three in a row. Only the minigames and their menu (scene 5, 361-390): the
// title and the other menus are left as they were. GAME3DS_SCENE_DRY=0: off.
//
// Not while the display capture is in use (Bounce and Pounce, the trampolines,
// which draw their 3D on each screen in turn and carry it to the other through
// VRAM): a tick without a picture leaves a screen showing the other's frame.
struct ScenePace {
    u64 due = 0;
    int run = 0;
    unsigned dry = 0, ticks = 0;   // since the last report
    unsigned long long caps = 0;   // the display captures performed, last tick
    int capturing = 0;             // ticks since the last one, counting down

    bool decide(u64 now, int vblanks)
    {
        static const bool off = getenv("GAME3DS_SCENE_DRY") && getenv("GAME3DS_SCENE_DRY")[0] == '0';
        const int scene = scene_now();
        const bool minigame = scene == 5 || (scene >= 361 && scene <= 390);
        const u64 period = (u64)vblanks * SYSCLOCK_ARM11 * 10000 / 598261;
        unsigned long long performed, refused, hash, pre;
        ntr::ppu_capture_counters(performed, refused, hash, pre);
        if (performed != caps) capturing = 120;
        else if (capturing) --capturing;
        caps = performed;
        // a load or a pause (half a second behind): the clock starts again from here
        if (!due || now > due + 30 * period || now + 30 * period < due) due = now;
        const bool d = !off && minigame && !capturing && run < 3 && now > due + period / 2;
        due += period;
        run = d ? run + 1 : 0;
        ++ticks;
        dry += d;
        return d;
    }
};
static ScenePace g_scene_pace;

// The report's line: how many ticks were dry, and the game's speed in ticks a
// second (60 is the DS's for a minigame)
static void scene_pace_report(int frame)
{
    static u64 last;
    const u64 now = svcGetSystemTick();
    ScenePace &p = g_scene_pace;
    if (last && p.dry)
        printf("[scene-dry] f%d: %u of %u ticks dry, %.1f ticks a second (scene %d)\n", frame, p.dry,
               p.ticks, p.ticks * (double)SYSCLOCK_ARM11 / (double)(now - last), scene_now());
    last = now;
    p.dry = p.ticks = 0;
}

static bool scene_dry_begin(u64 now)
{
    const bool dry = g_scene_pace.decide(now, frame_divider());
    ntr::gx_frame_dry(dry);
    port_message_composite_dry = dry;
    game3ds_sub_set_dry(dry);
    return dry;
}

static int interlude_frame(int frame)
{
    if (!game3ds_main_loop()) return 1;
    // a scene inside a dry level tick (DryPace) is drawn and shown, unless it's
    // a minigame behind its clock (ScenePace)
    const bool dry = scene_dry_begin(svcGetSystemTick());
    card3ds_wait_idle();   // the level loop's last frame may still be presenting on core 2
    card3ds_menu_bars(true);   // a scene's frame (the star select, the quit's title)
    PhaseTimersInterlude timers;
    apply_input(frame);
    autotap(frame);
    ntr::gx_card_present_allow(!g_cpu_raster);
    game3ds_sub_join();
    port_scene_tick(frame, 1);
    if (dry) return 0;
    const auto *fb = static_cast<const ntr::Framebuffer *>(port_scene_framebuffer());
    bool main_lower = false;
    int sub_stride = ntr::SUB_W;
    const uint32_t *sub = engine_b_picture(*fb, &main_lower, &sub_stride);
    gpu3ds::set_vblanks_per_frame(frame_divider());
    card3ds_other_strips(sub ? game3ds_sub_strips(sub) : nullptr);
    card3ds_other_affine(sub ? game3ds_sub_affine_of(sub) : nullptr);
    {
        int white = 0;
        const int evy = sub ? game3ds_sub_fade_of(sub, &white) : 0;
        card3ds_other_fade(evy, white != 0);   // engine B's fade, at 60 fps
    }
    // The outer requests timer includes this whole interlude. Presentation
    // zones remain off here so its displays do not inflate level frame counts.
    card3ds_present(*fb, sub, sub_stride, main_lower);
    return 0;
}

// hal/scene_boot.cpp's scn_fb (redirected here by a gccfix rule) lives in
// linear memory, so the present can hand it to the GPU's transfer engine
// instead of swizzling it on the CPU (~3.5 ms a frame on the file select).
extern "C" ntr::Framebuffer *game3ds_scene_fb(void)
{
    static ntr::Framebuffer *const fb = [] {
        void *m = linearAlloc(sizeof(ntr::Framebuffer));
        return m ? new (m) ntr::Framebuffer() : new ntr::Framebuffer();
    }();
    return fb;
}

// The scene loop (title and menus): one game tick per frame. The 3D goes
// through the card (see ntr/gx.h and card3ds.cpp): the frame is offered to the
// card before the tick, gx_render hands it triangles instead of rasterizing,
// the HAL's 2D compositor only writes its own pixels into fb, and
// card3ds_present draws the 3D, the 2D and the fade. Frames the card refuses
// are presented as the CPU picture.
//
// The present runs on core 2 alongside the next tick, like in the level loop.
// Done one after the other they missed every vblank and the menus ran at
// 30 fps. GAME3DS_SCENE_ASYNC_OFF=1 presents before the next tick instead.
//
// GAME3DS_SPAN=a:b (or GAME3DS_SPAN_LEVEL=a:b) marks frames a and b with two
// empty calls, for tools/span_sample_azahar.py.
extern "C" __attribute__((noinline)) void game3ds_span_begin(int frame) { asm volatile("" ::"r"(frame)); }
extern "C" __attribute__((noinline)) void game3ds_span_end(int frame) { asm volatile("" ::"r"(frame)); }
static void span_mark(const char *var, int frame)
{
    const char *e = getenv(var);
    int a = -1, b = -1;
    if (!e || sscanf(e, "%d:%d", &a, &b) != 2) return;
    if (frame == a) game3ds_span_begin(frame);
    if (frame == b) game3ds_span_end(frame);
}

static void boot_lap(const char *what, bool start = false);   // below
// Show the DS's own Nintendo logo before the title (GAME3DS_BOOT_SPLASH=1, on
// by default): the ROM's BootScene draws it, plays its sound and then asks for
// the title itself. Only replaces the default title; SM64DS_SCENE and level
// boots skip it.
static bool g_boot_splash;   // the logo is the scene running (scene_window_run)
extern "C" int game3ds_initial_scene(int requested)
{
    const char *splash = getenv("GAME3DS_BOOT_SPLASH");
    if (requested == 1 && !getenv("SM64DS_SCENE") && splash && atoi(splash)) {
        printf("[boot-splash] the DS's Nintendo logo (BootScene, 0), then the title (1)\n");
        g_boot_splash = true;
        return 0;
    }
    return requested;
}

static int scene_window_run(int frames)
{
    const int rc = port_scene_begin(nullptr, 1);
    if (rc)
        return rc;
    if (!card3ds_init()) {
        STEP("scene: FAIL card3ds_init");
        return 4;
    }
    console_tee_quiet(true);   // the bottom screen is the game's now
    // A/B: with sdmc:/sm64ds3ds/cpuraster present, never offer a frame to
    // the card (gx.cpp's CPU rasteriser draws every frame, as before).
    bool cpu_raster = false;
    if (FILE *f = fopen("sdmc:/sm64ds3ds/cpuraster", "r")) {
        cpu_raster = true;
        fclose(f);
        STEP("scene: cpuraster: the card is not offered the frames");
    }
    g_cpu_raster = cpu_raster;
    const bool async = getenv("GAME3DS_SCENE_ASYNC_OFF") == nullptr;
    if (!async) printf("[present-async] the scene loop presents in line (GAME3DS_SCENE_ASYNC_OFF)\n");
    // Last frame's engine B scan-out is joined at the message compositor
    // (phase_timers.cpp), same as the level loop. Joining here at the top of the
    // tick waited ~14.5 ms a frame. GAME3DS_SCENE_SUB_EARLY=1 joins here again.
    const bool sub_early = getenv("GAME3DS_SCENE_SUB_EARLY") != nullptr;
    if (sub_early) printf("[sub-async] the scene loop joins at the top of the tick (GAME3DS_SCENE_SUB_EARLY)\n");
    phase_timers_init();
    port_rom_frame_begin("title loop (3DS window)");
    FrameClock clk = {};
    int frame = 0;
    g_scene_watch = sub_watch_on();
    g_watch_names = kWatchAtScene;
    // Compose the boot logo once. Both its screens are full-screen BGs, so engine
    // A's composite redrew every cell each frame (22 ms on hardware) for a still
    // picture. Reuse the pause menu's memo until the first scene request.
    bool splash_memo = g_boot_splash && !(getenv("GAME3DS_COMPA_MEMO") && getenv("GAME3DS_COMPA_MEMO")[0] == '0');
    if (splash_memo) port_message_composite_memo(1);
    // And for the rest of the title loop: the head screen's composite was half the
    // tick and mostly unchanged. GAME3DS_TITLE_MEMO=0 turns it off after the logo.
    const bool title_memo = !(getenv("GAME3DS_COMPA_MEMO") && getenv("GAME3DS_COMPA_MEMO")[0] == '0') &&
                            !(getenv("GAME3DS_TITLE_MEMO") && getenv("GAME3DS_TITLE_MEMO")[0] == '0');
    if (title_memo) port_message_composite_memo(1);
    u64 t_end = svcGetSystemTick();   // the last frame's end, for the [slow] line
    unsigned layout_for = 0xffffu;
    for (; frame < frames && game3ds_main_loop(); ++frame) {
        // The scene about to spawn gets its own field, as in a crossing: the
        // minigame menu's minigames in a 3D room are wide, the rest 4:3.
        if (data_02092660 == 0 && data_02092664 != 0x187 && data_02092664 != layout_for) {
            layout_for = data_02092664;
            crossing_layout((int)layout_for);
        }
        if (splash_memo && data_02092664 != 0x187) {
            if (!title_memo) port_message_composite_memo(0);
            splash_memo = g_boot_splash = false;
            printf("[boot-splash] f%d: the logo asked for scene %u; the memo %s\n", frame,
                   (unsigned)data_02092664, title_memo ? "stays on" : "off");
        }
        card3ds_menu_bars(true);
        sub_watch(0);
        span_mark("GAME3DS_SPAN", frame);
        apply_input(frame);
        autotap(frame);
        const u64 t0 = svcGetSystemTick();
        phase_timers_frame_begin();
        game3ds_sub_tick_begin();
        ntr::gx_card_present_allow(!cpu_raster);
        if (sub_early) game3ds_sub_join();
        const bool dry = scene_dry_begin(t0);
        port_scene_tick(port_rom_frame_checked(frame, "title-entry-tick"), 1);
        port_rom_frame_phase6();
        const u64 t1 = svcGetSystemTick();
        if (dry) {   // the game ran; the screens keep the last frame
            clk.tick += t1 - t0;
            t_end = t1;
            if (++clk.frames == report_every()) {
                clk.report(frame + 1);
                scene_pace_report(frame + 1);
            }
            if (port_title_entry_should_stop()) {
                ++frame;
                break;
            }
            continue;
        }
        card3ds_present_timing_begin(async);
        if (ntr::gx_card_overlay_active()) ++clk.kept;
        const auto *fb = static_cast<const ntr::Framebuffer *>(port_scene_framebuffer());
        bool main_lower = false;
        int sub_stride = ntr::SUB_W;
        const uint32_t *sub = engine_b_picture(*fb, &main_lower, &sub_stride);
        gpu3ds::set_vblanks_per_frame(frame_divider());
        card3ds_other_strips(sub ? game3ds_sub_strips(sub) : nullptr);
        card3ds_other_affine(sub ? game3ds_sub_affine_of(sub) : nullptr);
        {
            int white = 0;
            const int evy = sub ? game3ds_sub_fade_of(sub, &white) : 0;
            card3ds_other_fade(evy, white != 0);   // engine B's fade, at 60 fps
        }
        card3ds_present(*fb, sub, sub_stride, main_lower, async);
        const u64 t2 = svcGetSystemTick();
        if (phase_timers_slow_frame(frame, -1, false, t0 - t_end, t1 - t0, t2 - t1, 0)) {
            char line[400];
            if (card3ds_slow_present_line(line, sizeof line)) printf("%s\n", line);
        }
        t_end = t2;
        clk.tick += t1 - t0;
        clk.present += t2 - t1;
        if (++clk.frames == report_every()) {
            clk.report(frame + 1);
            scene_pace_report(frame + 1);
        }
        if (port_title_entry_should_stop()) {
            ++frame;
            break;
        }
    }
    boot_lap("the title loop's last frame", true);
    clk.report(frame, true);   // waits for the last core 2 present
    card3ds_wait_idle();
    // the last tick's scan-out, before the scene is torn down; the title's and
    // menus' big maps leave about 5 MB in the scan-out's caches too
    scene_caches_release();
    g_scene_watch = false;
    g_watch_names = kWatchAt;
    memset(g_watch_count, 0, sizeof g_watch_count);
    g_watch_frames = 0;
    boot_lap("the title's last report and joins");
    if (g_test_run) {
        card3ds_screenshot("sdmc:/sm64ds3ds/game3ds_title_top.bmp");
        card3ds_screenshot("sdmc:/sm64ds3ds/game3ds_title_bottom.bmp", true);
    }
    boot_lap("the title screenshots");
    port_rom_frame_report();
    const int scene_rc = port_scene_finish(frame);
    boot_lap("port_scene_finish");
    if (port_title_entry_commit())
        return 0;
    return scene_rc;
}


static int boot_stage2(void);

// The level loop ended on a quit row (save and quit): main_smoke.cpp restarts
// the .3dsx at its title instead of waiting for START.
static bool g_quit_to_title;
extern "C" int game3ds_quit_to_title(void)
{
    return g_quit_to_title;
}

// Analog movement from the Circle Pad. The game already supports analog
// movement through the DS's touch-screen stick: CheckInput fills a record with
// a magnitude, direction and angle, and the game scales the speed from that.
// So this fills the same fields from the Circle Pad whenever it's pushed past
// the dead zone. Directions within 15 degrees of an axis snap to it, like the
// PC port. The options menu's ANALOG switch (or GAME3DS_ANALOG=0) turns it off.
// GAME3DS_CPAD_DEAD / GAME3DS_CPAD_FULL set the dead zone and full-push radius
// (defaults 18 and 145).
//
// Remember that this frame's touch-stick record came from the Circle Pad, so
// the minimap doesn't draw its touch circle (below).
static bool g_cpad_on_stick;

extern "C" int game3ds_options_analog(void);   // game/options3ds.cpp

static void analog_stick_publish(void)
{
    g_cpad_on_stick = false;
    static int read = 0, dead = 18, full = 145;
    if (!read) {
        read = 1;
        const char *e;
        if ((e = getenv("GAME3DS_CPAD_DEAD")) != nullptr) dead = atoi(e);
        if ((e = getenv("GAME3DS_CPAD_FULL")) != nullptr) full = atoi(e);
        if (full <= dead + 1) full = dead + 2;
        printf("[input] analog movement from the Circle Pad: dead zone %d, full push %d\n", dead, full);
    }
    if (!game3ds_options_analog()) return;
    int sx = 0, sy = 0;
    game3ds_cpad(&sx, &sy);
    double len = sqrt((double)sx * sx + (double)sy * sy);
    if (len <= (double)dead) return;
    // the notches: within 15 degrees of an axis, onto it (same length)
    const double deg = atan2(fabs((double)sy), fabs((double)sx)) * (180.0 / 3.14159265358979323846);
    if (deg <= 15.05) {
        sx = (int)(sx < 0 ? -len : len);
        sy = 0;
    } else if (deg >= 74.95) {
        sy = (int)(sy < 0 ? -len : len);
        sx = 0;
    }
    if (len > (double)full) len = full;
    int mag = (int)(4096.0 * (len - dead) / (double)(full - dead));
    if (mag > 0x1000) mag = 0x1000;
    if (mag < 1) mag = 1;
    const double l = sqrt((double)sx * sx + (double)sy * sy);
    const int dxs = sx, dys = -sy;   // touch-screen axes: y down
    const int o = (int)data_0209f250 * 0x18;   // this console's slot
    *(short *)(data_0209f4a0 + o) = (short)mag;
    *(short *)(data_0209f4a2 + o) = (short)((double)dxs * mag / l);
    *(short *)(data_0209f4a4 + o) = (short)((double)dys * mag / l);
    *(short *)(data_0209f4a6 + o) = _ZN4cstd5atan2E5Fix12IiES1_(dxs, dys);
    data_0209f4ac[o] = 1;   // "on the stick: read the deflection"
    g_cpad_on_stick = true;
    static const bool trace = getenv("GAME3DS_ANALOG_TRACE") != nullptr;
    static int n;
    if (trace && (n++ % 30) == 0)
        printf("[input] analog: pad %d,%d -> mag %d angle %d\n", sx, sy, mag,
               (int)*(short *)(data_0209f4a6 + o));
}

// No touch circle for the Circle Pad. The `touching` byte that tells the player
// about the stick also makes the minimap draw a circle under the stylus. So
// while the Circle Pad drives the record, the minimap alone sees it as 0. A
// real stylus still shows its circle.
extern "C" int __real__ZN7Minimap8BehaviorEv(void *self);
extern "C" int __wrap__ZN7Minimap8BehaviorEv(void *self)
{
    if (!g_cpad_on_stick)
        return __real__ZN7Minimap8BehaviorEv(self);
    unsigned char &touching = data_0209f4ac[(int)data_020a0e40[0] * 0x18];
    const unsigned char was = touching;
    touching = 0;
    const int rc = __real__ZN7Minimap8BehaviorEv(self);
    touching = was;
    return rc;
}

// The C-stick as the camera buttons. The bottom screen's camera arrows go
// through Stage::CheckCameraInput, which hit-tests the stylus record and sets
// the rotate bits under the game's own checks. While the C-stick is pushed
// left or right, this shows that one function a stylus held on the matching
// arrow and then puts the real record back, so everything else (including
// Mario's touch stick) sees the real input. The press is held until the game
// takes it. A real stylus wins.
// GAME3DS_CSTICK_CAMERA=0 disables it, GAME3DS_CSTICK_SWAP=1 swaps directions,
// GAME3DS_CSTICK_TRACE=1 logs what happened.
extern "C" void __real__ZN5Stage16CheckCameraInputEv(void);
extern "C" void __wrap__ZN5Stage16CheckCameraInputEv(void)
{
    static int on = -1, swap = 0;
    if (on < 0) {
        const char *e = getenv("GAME3DS_CSTICK_CAMERA");
        on = !(e && atoi(e) == 0);
        swap = getenv("GAME3DS_CSTICK_SWAP") != nullptr;
    }
    static int was;   // the direction last presented: -1, 0, 1
    int cx = 0, cy = 0;
    game3ds_cstick(&cx, &cy);
    // past 40 to engage, back under 24 to release (no chatter at the edge)
    int dir = cx <= -40 ? -1 : cx >= 40 ? 1 : 0;
    if (!dir && was && (was < 0 ? cx <= -24 : cx >= 24)) dir = was;
    if (!on || !dir || data_020a0de8[0]) {
        was = 0;
        __real__ZN5Stage16CheckCameraInputEv();
        return;
    }
    const int arrow = swap ? -dir : dir;
    const bool taken = ((const unsigned char *)data_0209f350)[0] == 1 && dir == was;
    unsigned char real[4];
    memcpy(real, data_020a0de8, 4);
    data_020a0de8[0] = 1;
    data_020a0de8[1] = taken ? 0 : 1;              // the press edge
    data_020a0de8[2] = arrow < 0 ? 0xbc : 0xe8;    // the 0x200 / 0x100 arrow
    data_020a0de8[3] = 0xaa;
    __real__ZN5Stage16CheckCameraInputEv();
    memcpy(data_020a0de8, real, 4);
    was = dir;
    static const bool trace = getenv("GAME3DS_CSTICK_TRACE") != nullptr;
    static int n;
    if (trace && (n++ % 30) == 0) {
        const char *cam = (const char *)data_0209f318;
        printf("[input] cstick %d: arrow x %d edge %d -> gate +0x154 %08x, Ctrl held %04x pressed %04x, "
               "taken %d\n",
               cx, arrow < 0 ? 0xbc : 0xe8, taken ? 0 : 1, cam ? *(const unsigned *)(cam + 0x154) : 0u,
               *(const unsigned short *)((const char *)data_0209f498 + 4),
               *(const unsigned short *)((const char *)data_0209f498 + 6),
               ((const unsigned char *)data_0209f350)[0]);
    }
}

// Stage::CheckInput fills the Ctrl block (one 0x18-byte record per player).
// The port keeps those fields as separate objects, so copy them out.
// hal/stage_frame.cpp calls this right after Stage::Behavior so the player
// reads this frame's input.
extern "C" void port_frame_ctrl_publish(void)
{
    const char *q = (const char *)data_0209f498;
    int np = (int)data_0209f21c;
    if (np < 1) np = 1;
    if (np > 4) np = 4;
    for (int pi = 0; pi < np; ++pi) {
        const char *r = q + pi * 0x18;
        const int o = pi * 0x18;
        *(short *)(data_0209f49c + o) = *(const short *)(r + 0x04);
        *(short *)(data_0209f49e + o) = *(const short *)(r + 0x06);
        *(short *)(data_0209f4a0 + o) = *(const short *)(r + 0x08);
        *(short *)(data_0209f4a2 + o) = *(const short *)(r + 0x0a);
        *(short *)(data_0209f4a4 + o) = *(const short *)(r + 0x0c);
        *(short *)(data_0209f4a6 + o) = *(const short *)(r + 0x0e);
        data_0209f4a8[o] = (unsigned char)r[0x10];
        data_0209f4a9[o] = (unsigned char)r[0x11];
        data_0209f4ac[o] = (unsigned char)r[0x14];
    }
    analog_stick_publish();
}

// The pad half of a level frame, after the touch poll: the ROM's pad read
// (phase 0x15), phase 2, the pad record (phase 0x17), and the Ctrl words the
// HAL reads before CheckInput republishes them. Single player only.
static void level_pad_phases(unsigned short raw, unsigned short btn, unsigned short *btn_was)
{
    // the ROM's pad read (func_0201ffcc) takes the keys from the link layer,
    // where the host publishes this frame's DS pad (walk_window.cpp :15391)
    port::comms_publish_pad(raw);
    data_0209d50c = 0x15;
    func_0201ffcc();
    data_0209d50c = 2;
    port_frame_phase2();
    data_0209d50c = 0x17;
    func_0203bc7c();
    for (int i = 0; i < 4; ++i)
        *(unsigned short *)((char *)data_020a0e5a + i * 4) =
            *(unsigned short *)((char *)data_020a0e58 + i * 4 + 2);
    hal_menu_pad_seam();
    data_0209d50c = 0x17;
    const unsigned short v =
        *(unsigned short *)((char *)data_020a0e58 + ((unsigned)data_020a0e40[0] << 2));
    data_0209d534[data_0209d4e8[0]] = v;
    data_0209d51c = v;
    data_0209d4e8[0] = (unsigned char)((data_0209d4e8[0] + 1) & 0x1f);
    func_02023498();
    *(unsigned short *)(data_0209f49c + 0) = btn;
    *(unsigned short *)(data_0209f49e + 0) = (unsigned short)(btn & ~*btn_was);
    *btn_was = btn;
}

// Read the DS buttons from hid into the two words the game uses: `raw` in
// KEYINPUT layout and the Ctrl word as the return value. Touches go into the
// HAL's poll. SM64DS_PROBE_INPUT (hal/input_probe.cpp) adds scripted presses
// for tests.
static unsigned short g_raw_keys;   // read_buttons' KEYINPUT-layout word, active high

static unsigned short read_buttons(int frame)
{
    unsigned raw = 0;
    int down = 0, tx = 0, ty = 0;
    game3ds_input_read(&raw, &down, &tx, &ty);
    raw |= port_input_probe_bits(frame);
    unsigned short btn = (unsigned short)(raw & 0x00fc);
    if (raw & 0x0001) btn |= 0x0001;
    if (raw & 0x0002) btn |= 0x0002;
    if (raw & 0x0100) btn |= 0x0400;
    if (raw & 0x0800) btn |= 0x0800;
    if (raw & 0x0200) btn |= 0x4000;
    if (raw & 0x0400) btn |= 0x8000;
    port_host_keys_set((unsigned short)raw);
    g_raw_keys = (unsigned short)raw;
    if (down)
        port_touch_force_press(tx, ty, 1);
    return btn;
}

// The adventure, after the title hands off: the real boot with the entrance's
// spawns and the game's camera. The ROM's actor frame runs the logic and draws
// the level; the host draws the player and particles, then composes and
// presents like the title.
//
// boot_lap logs the time and file loads of each step of the file select to
// level load, which used to take almost 10 s.
extern "C" {
extern unsigned long port_fs_loads, port_fs_load_miss, port_fs_bytes;
extern double port_fs_ms;
}
// game/files_pack.cpp: every fopen the game makes (and how many the pack served)
extern unsigned long g_fopen_n, g_fopen_packed;
extern u64 g_fopen_ticks;

static void boot_lap(const char *what, bool start)
{
    static u64 t_start, t_last, open_ticks;
    static unsigned long loads, miss, bytes, opens, packed;
    static double fs_ms;
    const u64 now = svcGetSystemTick();
    if (start || !t_start) {
        t_start = t_last = now;
        loads = port_fs_loads;
        miss = port_fs_load_miss;
        bytes = port_fs_bytes;
        fs_ms = port_fs_ms;
        opens = g_fopen_n;
        packed = g_fopen_packed;
        open_ticks = g_fopen_ticks;
    }
    const double k = 1000.0 / (double)SYSCLOCK_ARM11;
    game3ds_early_mark(what);   // written at once: a boot that dies here is placed
    printf("[boot-time] %s: +%.1f ms (%.1f in all); fopen %lu (%lu from the pack; %.1f ms in "
           "fopen + fclose); file loads %lu (%lu from the card), %lu KB, %.1f ms in Load\n", what,
           (double)(now - t_last) * k, (double)(now - t_start) * k, g_fopen_n - opens,
           g_fopen_packed - packed, (double)(g_fopen_ticks - open_ticks) * k, port_fs_loads - loads,
           port_fs_load_miss - miss, (port_fs_bytes - bytes) / 1024, port_fs_ms - fs_ms);
    t_last = now;
    loads = port_fs_loads;
    miss = port_fs_load_miss;
    bytes = port_fs_bytes;
    fs_ms = port_fs_ms;
    opens = g_fopen_n;
    packed = g_fopen_packed;
    open_ticks = g_fopen_ticks;
}

// Dry ticks: keep the game at full speed when frames can't keep up. The game
// advances one step per tick, so a level running at 24 fps on Old 3DS also
// plays at 80% speed. A dry tick runs all the game logic, sound and register
// updates but skips building a picture (no GX raster, no engine A compose, no
// engine B scan, nothing presented), so the screens keep the last frame and
// the next tick catches up.
//
// A tick is dry when it starts more than half a period behind the game clock,
// never two in a row. GAME3DS_DRY=0 never, 1 when behind (the Old 3DS default
// with 60 fps off), 2 every other tick, 3 every tick (both for testing).
extern "C" int port_message_composite_dry;   // hal/message_compositor.cpp
struct DryPace {
    int mode = -1;
    u64 due = 0;
    bool last = false;
    unsigned dry = 0, ticks = 0;   // since the last report
    u64 t_dry = 0, t_shown = 0;    // their ticks (t0..t1), the report's split

    bool decide(u64 now, int vblanks)
    {
        if (mode < 0) {
            const char *e = getenv("GAME3DS_DRY");
            mode = e ? atoi(e) : (game3ds_old_budget ? 1 : 0);
            printf("[dry] %s\n", mode == 3 ? "every tick (GAME3DS_DRY=3: a profile)"
                                 : mode == 2 ? "every other tick (GAME3DS_DRY=2)"
                                 : mode ? "a tick behind the game's clock makes no picture"
                                        : "off");
        }
        // a tick of two vblanks at 59.83 Hz
        const u64 period = (u64)vblanks * SYSCLOCK_ARM11 * 10000 / 598261;
        // a load or a pause: the clock starts again from here
        if (!due || now > due + 4 * period || now + 4 * period < due) due = now;
        bool d = false;
        if (mode == 2) d = !last;
        else if (mode == 3) d = true;   // a profile of dry ticks alone (nothing is shown)
        else if (mode == 1 && !interp3ds_on()) d = !last && now > due + period / 2;
        due += period;
        last = d;
        ++ticks;
        dry += d;
        return d;
    }
};

static int level_window_run(int frames)
{
    STEP("level: boot");
    boot_lap("level boot begins");
    if (!boot_stage2())
        return 2;
    boot_lap("boot_stage2 (game init, game heap)");
    char *stage = (char *)port_stage_create();
    if (!stage) {
        STEP("level: FAIL no Stage");
        return 2;
    }
    boot_lap("port_stage_create");
    void *mc = stage + 0x91c;
    port_stage_a2_seat();
    if (!port_stage_a_boot(mc, 1)) {
        STEP("level: FAIL port_stage_a_boot");
        return 2;
    }
    boot_lap("port_stage_a_boot (the level's load and spawns)");
    auto local_player = [](void) -> void * {
        int me = (int)data_0209f250;
        if (me < 0 || me > 3 || !data_0209f394[me]) me = 0;
        return data_0209f394[me];
    };
    void *player = local_player();
    if (!player) {
        STEP("level: FAIL the entrance spawned no player");
        return 3;
    }
    // Load the fog first, in the same order as Stage::InitResources. Without it
    // the two Fog records Stage::RenderFog uses stayed zero and nothing fogged.
    _ZN5Stage7LoadFogEv(stage);
    _ZN5Stage10LoadSkyboxEv(stage);
    boot_lap("LoadFog + LoadSkybox");
    data_020a0e40[0] = 0;
    if (data_0209fc48 == 0) {
        data_0209caa0[2] |= 0x80;   // no cutscene script: the opening is not running
    } else {
        printf("[level] a cutscene script is running (%p): the opening plays\n",
               (void *)(size_t)data_0209fc48);
    }
    data_0209d660 = 0;
    data_0209f21c = 1;
    data_0209f350[0] = 0;
    {
        static unsigned short zero_btn_map[32];
        ((unsigned short **)data_0209214c)[0] = (unsigned short *)data_02075650;
        ((unsigned short **)data_0209214c)[1] = (unsigned short *)data_02075670;
        ((unsigned short **)data_0209214c)[2] = (unsigned short *)data_02075690;
        ((unsigned short **)data_0209214c)[3] = zero_btn_map;
    }
    if (!data_0209f318) {
        STEP("level: FAIL the entrance spawned no camera");
        return 5;
    }
    port_course_seat();
    printf("[level] player %p at (%d, %d, %d), camera %p\n", player,
           *(int *)((char *)player + 0x5c) >> 12, *(int *)((char *)player + 0x60) >> 12,
           *(int *)((char *)player + 0x64) >> 12, data_0209f318);
    ntr::rt_irq_boot_state();
    // The widescreen build (ntr/ppu.h NTR_WIDE_3DS): the level uses the whole 5:3
    // frame (a scene before it may have asked for the DS's own 4:3, centred).
    if (ntr::widescreen)
        ntr::set_present_native(false);
    boot_lap("course seat, irq state");
    STEP("level: frame loop");

    // In linear memory, so the card's present can hand it to the GPU's transfer
    // engine instead of swizzling it on the CPU (gpu3ds::overlay_transfer).
    static ntr::Framebuffer *const fb_mem = [] {
        void *m = linearAlloc(sizeof(ntr::Framebuffer));
        return m ? new (m) ntr::Framebuffer() : new ntr::Framebuffer();
    }();
    ntr::Framebuffer &fb = *fb_mem;
    // the same A/B as the title loop's: sdmc:/sm64ds3ds/cpuraster keeps the
    // card out of every level frame too
    bool cpu_raster = false;
    if (FILE *f = fopen("sdmc:/sm64ds3ds/cpuraster", "r")) {
        cpu_raster = true;
        fclose(f);
        STEP("level: cpuraster: the card is not offered the frames");
    }
    g_cpu_raster = cpu_raster;
    // Compose the pause menu once: a frame with the same engine A inputs as the
    // last menu frame reuses its result. GAME3DS_COMPA_MEMO=0 composes every frame.
    {
        const char *e = getenv("GAME3DS_COMPA_MEMO");
        port_message_composite_memo(!(e && e[0] == '0'));
    }
    port_rom_frame_begin("level loop (3DS window)");
    FrameClock clk = {};
    DryPace pace;
    unsigned short btn_was = 0;
    int frame = 0;
    u64 t_end = svcGetSystemTick();   // the last frame's end, for the [slow] line
    for (; frame < frames && game3ds_main_loop(); ++frame) {
        span_mark("GAME3DS_SPAN_LEVEL", frame);
        card3ds_menu_bars(false);   // the game world (an interlude sets it again)
        // Save and quit. The pause menu's quit and the level-clear Save and Quit ask
        // for scene 1 (the title) through StartSceneFade. The old fallback
        // (GAME3DS_QUIT_RELAUNCH=1) lets the fade finish, ends the session and has
        // main_smoke.cpp relaunch the .3dsx. By default port_level_quit_crosses lets
        // the ROM tear down its Stage instead, and hal/level_change.cpp's crossing
        // runs the title in this process.
        if (data_02092664 == 1 && !port_level_change_pending() && !port_level_quit_crosses) {
            static int quit_seen = -1;
            if (quit_seen < 0) {
                quit_seen = frame;
                printf("[quit] f%d the ROM asked for scene 1 (the title): a quit row of the "
                       "pause or level-clear menu (any save is already written)\n", frame);
            }
            int evy = 0, tw = 0;
            const bool covered = port_fader_blend_state(&evy, &tw) && evy >= 16;
            if (covered || frame - quit_seen >= 90) {
                printf("[quit] f%d the fade has %s (evy %d after %d frames): ending the session, "
                       "back to the title\n", frame, covered ? "covered" : "NOT covered, ceiling reached",
                       evy, frame - quit_seen);
                port_scene_request_release("save and quit: the 3DS restarts the game at its title");
                g_quit_to_title = true;
                break;
            }
        }
        const u64 t0 = svcGetSystemTick();
        phase_timers_tick_begin(t0);
        const unsigned short btn = read_buttons(frame);
        sub_watch(1);
        autotap(frame);   // GAME3DS_TAP's scripted taps
        hal_sub_camera_zone_live();
        sub_watch(2);
        hal_sub_screen_frame_begin();
        sub_watch(3);
        level_pad_phases(g_raw_keys, btn, &btn_was);
        sub_watch(4);
        hal_sub_camera_input();
        phase_timers_tick_mark(TickPhase::input);
        sub_watch(5);

        // a level change (a door, a painting, a star): the ROM asked, the
        // HAL swaps the level, and the host picks up the new Stage and Player
        int game_ticked = 1;
        bool level_changed = false;
        int scene_fade = 0, scene_id = -1;
        if (port_scene_fade_pending(&scene_id)) {
            int evy = 0, tw = 0;
            scene_fade = (port_fader_blend_state(&evy, &tw) && evy >= 16) ? 2 : 1;
        }
        // The swap frees and reloads what a present might still be reading, so drain
        // only on the frame the poll actually swaps. Draining through the whole exit
        // fade made each course exit fade at ~20 fps.
        if (port_level_change_pending() && scene_fade != 1 && port_level_change_due()) {
            card3ds_wait_idle();
            scene_caches_release();   // joins; the last level's pictures go
        } else if (!port_level_change_pending()) {
            heap_valve(frame);
        }
        if (port_level_change_pending() && scene_fade != 1 && port_level_change_poll()) {
            if (scene_fade == 2) {
                if (!port_fader_entrance_wipe_armed())
                    port_fader_start_color(16, 0, 0x7fff);
                port_scene_fade_clear();
            }
            player = local_player();
            if (!player) {
                STEP("level: FAIL the new level spawned no player");
                card3ds_wait_idle();
                return 3;
            }
            if (char *ns = (char *)port_stage_object()) {
                stage = ns;
                mc = stage + 0x91c;
            }
            _ZN5Stage7LoadFogEv(stage);
            _ZN5Stage10LoadSkyboxEv(stage);
            printf("[level] f%d: level changed, player %p\n", frame, player);
            // the new level's first record has no previous (without this the
            // pairing's 85% floor caught it: "few pairs" once a change)
            interp3ds_reset();
            game_ticked = 0;
            level_changed = true;
        }

        phase_timers_tick_mark(TickPhase::requests);
        sub_watch(6);
        port_intro_bit_edge();
        const int frozen = port_level_hold_live();
        port_actor_frame_begin();
        const unsigned ren_mark = port_stage_render_calls();
        ntr::gx_reset();
        // a dry tick (DryPace): the game runs, no picture is made
        const bool dry = pace.decide(t0, frame_divider());
        ntr::gx_frame_dry(dry);
        port_message_composite_dry = dry;
        game3ds_sub_set_dry(dry);
        ntr::gx_set_light(0, -0.4f, -0.6f, -0.7f, 0x7FFF);
        ntr::gx_enable_lights(0x1);
        if (frozen) {
            game_ticked = 0;
        } else {
            // SM64DS_STAR_TRIGGER=<frame>: collect a star (hal/input_probe.cpp),
            // for a test of the star sequence and the level change after it
            port_input_probe_star_trigger(frame);
            // GAME3DS_TEST_VOID=<frame>: drop the player to y -31000 so the game's own
            // fall-out check kills him and changes level (used by the stress suite).
            // GAME3DS_TEST_POS=<frame>:<x>:<y>:<z>[:<angle>]: put the player at a spot,
            // facing angle (0x4000 is +x), for scripted tests.
            static int pos_at = -2, pos_x, pos_y, pos_z, pos_ang = -1;
            if (pos_at == -2) {
                pos_at = -1;
                if (const char *e = getenv("GAME3DS_TEST_POS"))
                    if (sscanf(e, "%d:%d:%d:%d:%i", &pos_at, &pos_x, &pos_y, &pos_z, &pos_ang) < 4) pos_at = -1;
            }
            if (frame == pos_at && player) {
                *(int *)((char *)player + 0x5c) = pos_x << 12;
                *(int *)((char *)player + 0x60) = pos_y << 12;
                *(int *)((char *)player + 0x64) = pos_z << 12;
                if (pos_ang >= 0)   // mAngleY and mPrevAngleY
                    *(short *)((char *)player + 0x8e) = *(short *)((char *)player + 0x94) = (short)pos_ang;
                printf("[test] f%d: player put at (%d, %d, %d)\n", frame, pos_x, pos_y, pos_z);
            }
            static const int void_at = getenv("GAME3DS_TEST_VOID") ? atoi(getenv("GAME3DS_TEST_VOID")) : -1;
            if (frame == void_at && player) {
                *(int *)((char *)player + 0x60) = -31000 << 12;
                printf("[test] f%d: player dropped to y -31000\n", frame);
            }
            // GAME3DS_TEST_QUIT=<frame>[,<frame>...]: do what the pause menu's quit does
            // (ask for the title and freeze). Can be a list, since an in-process quit
            // comes back to this loop.
            bool quit_now = false;
            if (const char *q = getenv("GAME3DS_TEST_QUIT"))
                for (const char *c = q; *c; ) {
                    if (atoi(c) == frame) quit_now = true;
                    while (*c && *c != ',') ++c;
                    if (*c == ',') ++c;
                }
            if (quit_now) {
                _ZN8dScene_c14StartSceneFadeEjjt(1, 0, 0);
                data_0209b454[0] |= 0x40000000;
                data_0209b464 = (unsigned)data_0209b454[0];
                printf("[test] f%d: the pause menu's quit (StartSceneFade(1, 0, 0))\n", frame);
            }
            // GAME3DS_TEST_CRASH=oom|abort|fill: at frame 60, trigger an impossible new,
            // an abort(), or allocate 64 KB until it fails, to test crash_handler.cpp.
            if (frame == 60)
                if (const char *k = getenv("GAME3DS_TEST_CRASH")) {
                    printf("[test] f%d: GAME3DS_TEST_CRASH=%s\n", frame, k);
                    if (!strcmp(k, "abort")) abort();
                    if (!strcmp(k, "fill"))
                        for (;;) static_cast<volatile char *>(::operator new(65536))[0] = 1;
                    volatile size_t huge = 0x7ff00000u;
                    static_cast<volatile char *>(::operator new(huge))[0] = 1;
                }
            port_actor_frame(1);
        }
        phase_timers_tick_mark(TickPhase::actor);
        sub_watch(7);
        {
            // Stage::Behavior did not run (a frozen frame): its CheckInput
            // and the publish still do, as the ROM's input phase would
            static unsigned beh_seen;
            const unsigned beh_now = port_stage_behavior_calls();
            if (beh_now == beh_seen) {
                _ZN5Stage10CheckInputEv();
                port_frame_ctrl_publish();
            }
            beh_seen = beh_now;
        }
        {   // GAME3DS_LEVEL_TRACE: every 30 frames, the player and the input
            // chain (Ctrl held, stick magnitude/angle, the pad mirror, KEYINPUT)
            static int on = -1;
            if (on < 0) on = getenv("GAME3DS_LEVEL_TRACE") ? 1 : 0;
            if (on && frame % 30 == 0) {
                const char *c = (const char *)player;
                printf("[ltrace] f%d frozen %d beh %u pos (%d,%d,%d) state %p held %04x "
                       "mag %d ang %04x pad %04x keys %04x script %p stage13 %02x\n",
                       frame, frozen, port_stage_behavior_calls(), *(const int *)(c + 0x5c) >> 12,
                       *(const int *)(c + 0x60) >> 12, *(const int *)(c + 0x64) >> 12,
                       *(void *const *)(c + 0x370), *(unsigned short *)data_0209f49c,
                       *(short *)data_0209f4a0, *(unsigned short *)data_0209f4a2,
                       *(unsigned short *)data_020a0e58,
                       (unsigned)*(volatile unsigned short *)0x04000130,
                       (void *)(size_t)data_0209fc48, stage[0x13]);
            }
        }
        if (game_ticked) {
            port_frame_clock_tick();
            ntr::rt_timer0_advance((unsigned)data_0208ee44);
        }
        ntr::rt_scanout_frame();
        port_input_probe_trace_msg(frame);   // SM64DS_TRACE_STAR / message traces
        phase_timers_tick_mark(TickPhase::post_actor);

        // the level, when the ROM's frame did not draw it
        if (port_stage_render_calls() == ren_mark) {
            port_stage_render_skybox(stage);
            port_stage_advance_anims(stage);
            port_stage_render_model(stage);
            _ZN11ShadowModel9RenderAllEv();
            port_stage_render_model_transparent(stage);
            port_cylinder_clsn_process();
            func_ov001_020aaf40();
        }
        phase_timers_tick_mark(TickPhase::fallback_render);
        port_actor_frame_end(port_rom_frame_checked(frame, "level-frame-end"), "level");
        phase_timers_tick_mark(TickPhase::frame_end);
        hal_render_player_world(player);
        phase_timers_tick_mark(TickPhase::player_world);
        sub_watch(8);
        if (port_particle_phase5_begin()) {
            port_frame_phase5();
            port_particle_phase5_end();
        }
        phase_timers_tick_mark(TickPhase::particles);
        sub_watch(9);
        // This frame's 2D writes start here, so the last present on core 2 must have
        // copied its inputs by now.
        card3ds_wait_inputs();
        port_fader_wipe_render();
        phase_timers_tick_mark(TickPhase::wipe);
        sub_watch(10);

        if (!dry) {
            ntr::gx_card_present_allow(data_0209f2d8 != 1 && !cpu_raster);
            port_gpu_twod_reset();
            ntr::gx_clear_fill(fb);
            ntr::gx_render(fb);
        }
        phase_timers_tick_mark(TickPhase::gx);
        sub_watch(11);
        // The ROM's vblank code at the compositor's head publishes engine B's
        // registers and OAM, so last frame's scan-out must be done. On Old 3DS the
        // worker only scans during the game thread's waits, so there we collect it if
        // it's done and never wait.
        game3ds_sub_join_tick();
        port_message_composite_engine_a(&fb);
        phase_timers_tick_mark(TickPhase::message);
        sub_watch(12);
        hal_sub_screen_present(&fb.px[0][0], ntr::active_w, ntr::active_h);
        sub_watch(-1);
        if (!dry && port_level_hold_live()) {
            const unsigned hold = port_level_hold_white() ? 0xFFFFFFFFu : 0xFF000000u;
            for (int y = 0; y < ntr::active_h; ++y)
                for (int x = 0; x < ntr::active_w; ++x)
                    fb.px[y][x] = hold;
        }
        const u64 t1 = svcGetSystemTick();
        phase_timers_tick_end(t1);
        if (!dry) {   // a dry tick presents nothing: the screens keep the last frame
            card3ds_present_timing_begin(true);
            if (ntr::gx_card_overlay_active()) ++clk.kept;
            bool main_lower = false;
            int sub_stride = ntr::SUB_W;
            const uint32_t *sub = engine_b_picture(fb, &main_lower, &sub_stride);
            {   // The two pictures as numbers, so the card sends a picture only when
                // Picture IDs, so the card only re-sends a picture when it changed. Engine A's
                // comes from the compositor's spans (only trusted on card-held frames in the
                // stacked layout without a hold; otherwise 0, always sent). Engine B's comes
                // from the bottom-screen worker's buffer.
                unsigned long long cs = 0;
                uint64_t main_sig = 0;
                if (port_message_composite_signature(&cs) && ntr::gx_card_overlay_active() &&
                    hal_sub_screen_stacked() && !port_level_hold_live())
                    main_sig = (cs ^ (uint64_t)ntr::active_w << 48 ^ (uint64_t)ntr::active_h << 32) | 1;
                const uint64_t sub_sig = game3ds_sub_sig_of(sub);
                card3ds_picture_sigs(main_sig, sub_sig);
            }
            gpu3ds::set_vblanks_per_frame(frame_divider());
            // the minimap's BG left to the GPU (src/sub_affine.h)
            card3ds_other_affine(sub ? game3ds_sub_affine_of(sub) : nullptr);
            {
                int white = 0;
                const int evy = sub ? game3ds_sub_fade_of(sub, &white) : 0;
                card3ds_other_fade(evy, white != 0);   // engine B's fade, at 60 fps
            }
            card3ds_present(fb, sub, sub_stride, main_lower, true);
        }
        const u64 t2 = svcGetSystemTick();

        sdat_host_tick();
        port_rom_frame_phase6();
        func_020190b8();
        ntr::gx_swap_apply();
        sub_watch(0);
        data_0209d4f0[0] = 1;
        data_0209d4f0[0] = 0;

        const u64 t3 = svcGetSystemTick();
        if (phase_timers_slow_frame(frame, data_0209f2f8, level_changed, t0 - t_end,
                                    t1 - t0, t2 - t1, t3 - t2)) {
            // and the core-2 job its handoff waited for, frame by frame
            char line[400];
            if (card3ds_slow_present_line(line, sizeof line)) printf("%s\n", line);
        }
        t_end = t3;
        clk.tick += t1 - t0;
        clk.present += t2 - t1;
        (dry ? pace.t_dry : pace.t_shown) += t1 - t0;
        if (++clk.frames == report_every()) {
            if (pace.mode > 0) {
                const double k = 1000.0 / (double)SYSCLOCK_ARM11;
                const unsigned shown = pace.ticks - pace.dry;
                printf("[dry] f%d: %u of %u ticks dry; a dry tick %.2f ms, a shown one %.2f\n",
                       frame + 1, pace.dry, pace.ticks, pace.dry ? pace.t_dry * k / pace.dry : 0.0,
                       shown ? pace.t_shown * k / shown : 0.0);
            }
            pace.dry = pace.ticks = 0;
            pace.t_dry = pace.t_shown = 0;
            clk.report(frame + 1);
        }
    }
    clk.report(frame, true);   // waits for the last core 2 present
    port_rom_frame_report();
    printf("[level] %d frames; player at (%d, %d, %d)\n", frame,
           *(int *)((char *)player + 0x5c) >> 12, *(int *)((char *)player + 0x60) >> 12,
           *(int *)((char *)player + 0x64) >> 12);
    return 0;
}

static int boot_stage1(void)
{
    STEP("boot: rom pre-main");
    port_boot_rom_pre_main();
    STEP("boot: root heap");
    _ZN4Heap18InitializeRootHeapEv();
    if (!data_020a0ea0) {
        STEP("boot: FAIL no root heap");
        return 0;
    }
    // walk_window installs its host frame pump here (step 1b of the ROM's
    // wait); the 3DS one comes with the frame loop, stage 3.
    STEP("boot: rom main head");
    if (port_rom_main_enabled())
        port_rom_main_run();
    else
        port_boot_rom_main_head();
    for (int i = 0; i < 12; ++i)
        data_0209b3ec[i] = 0;
    data_0209b3ec[0] = data_0209b3ec[4] = data_0209b3ec[8] = 0x1000;
    STEP("boot: host vtable fills");
    hal_fill_model_vtable();
    hal_fill_shadow_vtable();
    hal_fill_mmc_vtable();
    hal_fill_modelanim2_vtable();
    STEP("boot: ov002 patches + static initialisers");
    port_ov002_patch();
    port_cross_patch();
    __sinit_ov002_02100560(); __sinit_ov002_02100938();
    __sinit_ov002_02100adc(); __sinit_ov002_02100c50();
    __sinit_ov002_02100d44(); __sinit_ov002_02100e50();
    __sinit_ov002_02100ec4(); __sinit_ov002_02100f84();
    __sinit_ov002_02101064(); __sinit_ov002_02101478();
    __sinit_ov002_021014e4(); __sinit_ov002_02101588();
    __sinit_ov002_02101738(); __sinit_ov002_02101894();
    __sinit_ov002_02101900(); __sinit_ov002_02101968();
    __sinit_ov002_021019d0(); __sinit_ov002_02106e40();
    __sinit_ov002_02107118(); __sinit_ov002_021071f4();
    __sinit_ov002_02107298(); __sinit_ov002_02107304();
    port_cutscene_states_seat();
    port_kuppa_cmd_seat();
    __sinit_ov002_02107370(); __sinit_ov002_02107f88();
    __sinit_ov002_0210804c(); __sinit_ov002_02108094();
    return 1;
}

static int boot_stage2(void)
{
    data_02092144[0] = 8 << 8;
    STEP("boot: game init head");
    port_boot_rom_game_init_head();
    STEP("boot: game heap");
    _ZN4Heap18InitializeGameHeapEjPS_(0x3b000, 0);
    if (!data_020a0eac_c) {
        STEP("boot: FAIL no game heap");
        return 0;
    }
    printf("[heap] game heap %p, 0x%x bytes, %u free\n", data_020a0eac_c, 0x3b000u,
           _ZN22ExpandingHeapAllocator10MemoryLeftEv(
               *(void **)((char *)data_020a0eac_c + 0x14)));
    if (data_020a0e9c)
        printf("[heap] root heap %p, %u free\n", data_020a0e9c,
               _ZN22ExpandingHeapAllocator10MemoryLeftEv(
                   *(void **)((char *)data_020a0e9c + 0x14)));
    STEP("boot: game init tail");
    port_boot_rom_game_init_tail();
    data_0209f2d8 = 0;
    return 1;
}

// The 3DS has no environment, but the HAL reads settings from it (getenv,
// mostly cached on first read). sdmc:/sm64ds3ds/env.txt fills it: KEY=VALUE
// per line, # for comments.
//
// Without the file you get the release settings: the GPU vertex path and
// in-between frames (60 fps), and no profilers (a hardware run that wants
// them uses port3ds/tools/hardware_env.txt). Any env.txt, even an empty one,
// replaces all of them (the regression runner always writes one).
void game3ds_lz_selftest();   // game/files_pack.cpp
extern "C" void game3ds_options_load(void);   // game/options3ds.cpp
static void load_env_file(void)
{
    FILE *f = fopen("sdmc:/sm64ds3ds/env.txt", "r");
    if (!f) {
        static const char *const release[][2] = {
            {"GAME3DS_BOOT_SPLASH", "1"},
            // 60 fps (the in-between frames) on a New 3DS; an Old 3DS runs at
            // the game's own 30 unless the options menu turns 60 on
            {"GAME3DS_GPU_XFORM", "1"}, {"GAME3DS_INTERP", game3ds_old_budget ? "0" : "1"},
        };
        printf("[env] no sdmc:/sm64ds3ds/env.txt: the release settings\n");
        for (const auto &kv : release) {
            setenv(kv[0], kv[1], 0);
            printf("[env] %s=%s\n", kv[0], kv[1]);
        }
        return;
    }
    g_test_run = true;
    static char line[4096];   // a long SM64DS_TOUCH_PROBE list fits
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        char *end = v + strlen(v);
        while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        setenv(p, v, 1);
        printf("[env] %s=%s\n", p, v);
    }
    fclose(f);
}

int sm64ds_smoke_main(void)
{
    game3ds_early_mark("main");
    // Print which build wrote this log: the top screen's tier and when this file
    // was compiled.
#if defined(NTR_WIDE_3DS) && defined(__OPTIMIZE_SIZE__)
    printf("[build] wide (400x240 top screen), main_game.cpp compiled %s %s, the decomp at -Os\n",
           __DATE__, __TIME__);
#elif defined(NTR_WIDE_3DS)
    printf("[build] wide (400x240 top screen), main_game.cpp compiled %s %s\n", __DATE__, __TIME__);
#else
    printf("[build] 4:3 (320x240 top screen), main_game.cpp compiled %s %s\n", __DATE__, __TIME__);
#endif
    // Use the DS's two screens: the menus in their own layout and the sub engine
    // on the bottom screen. Both settings are cached on first read, so set them
    // before anything runs. The PC's improved minimap doesn't fit the DS bottom
    // screen and also hid the camera buttons, so it's off (env.txt can turn it
    // back on).
    setenv("SM64DS_ONE_SCREEN_MENUS", "0", 1);
    setenv("SM64DS_DUAL_SCREEN", "1", 1);
    setenv("SM64DS_IMPROVED_MINIMAP", "0", 1);
    load_env_file();
    // Use engine A's composite memo on every held frame, not just menus. A course
    // HUD is unchanged most frames, and comparing inputs is cheaper than
    // composing (msg2d went from ~5 ms to ~1 ms in most levels). env.txt can
    // override it.
    setenv("SM64DS_COMPA_MEMO_CELLS", "0", 0);
    if (game3ds_old_budget) {
        // Old 3DS (64 MB): the DS arena at 4 MB, the size of the DS's own main RAM,
        // and the sound archive read from the card by range instead of being kept in
        // memory (4.3 MB).
        setenv("SM64DS_HOST_ARENA_MB", "4", 0);
        port_fs_stream_files_over(1u << 20);
        // Engine B's BG pictures take 7 bytes a pixel, so the 512x512 maps on the
        // title and level 1 used ~4.6 MB and ran the Old 3DS out of heap. BGs over
        // 256x256 are sampled directly instead. It's slower, but on Old 3DS the scan
        // only runs in the game thread's waits.
        setenv("GAME3DS_SUB_PLANE_MAX_PX", "65536", 0);
        printf("[mem] Old 3DS budget: DS arena %s MB, files of 1 MB or more read by range, "
               "engine B's BG pictures up to %s pixels\n",
               getenv("SM64DS_HOST_ARENA_MB"), getenv("GAME3DS_SUB_PLANE_MAX_PX"));
    }
    game3ds_options_load();   // the options menu's choices over env.txt's
    game3ds_lz_selftest();   // GAME3DS_LZ_SELFTEST=1 only (game/files_pack.cpp)
    // Save and quit goes back to the title in this process (hal/level_change.cpp
    // handles it like the Game Over crossing). GAME3DS_QUIT_RELAUNCH=1 relaunches
    // the .3dsx instead.
    port_level_quit_crosses = getenv("GAME3DS_QUIT_RELAUNCH") ? 0 : 1;
    // main_smoke.cpp boots the game with the console quiet; this shows the
    // boot's text on the bottom screen again, for a boot that needs watching
    if (getenv("GAME3DS_BOOT_CONSOLE")) console_tee_quiet(false);
    port_interlude_frame_hook = interlude_frame;
    port_crossing_scene_layout_hook = crossing_layout;
    // The save chip's file (ntr/backup.cpp): beside the game data on the SD
    // card, not wherever the homebrew's working directory happens to be.
    setenv("SM64DS_SAVE_PATH", "sdmc:/sm64ds3ds/save/sm64ds.sav", 0);
    // After setting the path: starting the save writer opens the file, which
    // reads the path once. Before this, a run without SM64DS_SAVE_PATH opened
    // ./save/ and showed three empty files.
    game3ds_save_start();   // before the first card access
    game3ds_watchdog_start();   // a frame not ended in 4 s: the "[hang]" report
    game3ds_lid_start();        // the cartridge's lid sounds around the 3DS's sleep
    if (!boot_stage1())
        return 2;
    STEP("boot: stage 1 done");

    // Same order as walk_window: run a scene (the title by default) before the
    // level bring-up, while the root heap is still current. Stage 2 (game init,
    // the game heap) belongs to the level path.
    const int scene = port_scene_env_want();
    printf("[game3ds] default scene %d\n", scene);
    // GAME3DS_FRAMES / GAME3DS_LEVEL_FRAMES cap the two runs (a test); unset,
    // they run until the player picks a file, or closes the app.
    const char *fr = getenv("GAME3DS_FRAMES");
    const char *lf = getenv("GAME3DS_LEVEL_FRAMES");
    const int level_frames = lf ? atoi(lf) : 0x7fffffff;
    int rc;
    if (scene < 0) {
        // A level was named (SM64DS_LEVEL): boot straight into it, no title.
        STEP("level: direct boot");
        hal_sub_screen_init(nullptr, 1);
        if (!card3ds_init()) {
            STEP("level: FAIL card3ds_init");
            return 4;
        }
        console_tee_quiet(true);
        rc = level_window_run(level_frames);
        printf("[game3ds] level run rc=%d\n", rc);
    } else {
        STEP("scene: window run");
        rc = scene_window_run(fr ? atoi(fr) : 0x7fffffff);
        boot_lap("the title scene's end (its last frame to its return)");
        printf("[game3ds] scene run rc=%d\n", rc);
        if (rc == 0 && port_title_entry_taken()) {
            rc = level_window_run(level_frames);
            printf("[game3ds] level run rc=%d\n", rc);
        }
    }
    if (g_test_run) {
        card3ds_screenshot("sdmc:/sm64ds3ds/game3ds_top.bmp");
        card3ds_screenshot("sdmc:/sm64ds3ds/game3ds_bottom.bmp", true);
    }
    card3ds_shutdown();
    return rc;
}
