// Where port_scene_tick's time goes. The HAL's frame phases are each timed by
// a linker --wrap (CMakeLists.txt), so the HAL itself is untouched: calls from
// other objects go through __wrap_<name>, which times __real_<name>. Calls
// inside the same object aren't wrapped.
//
// Two levels, printed as two lines: the phases port_scene_tick calls, and some
// calls inside them. Times nest like the HAL does, so a settle inside the
// present counts in both.
#include <stdio.h>
#include <stdlib.h>

#include <3ds.h>
#include "phase_timers.h"
#include "ntr/gx.h"
#include "ntr/ppu_audit.h"

void card3ds_wait_inputs();   // src/card3ds.cpp

// Each row: timer, wrapped symbol, return type, parameters and argument names.
// Mangled C++ names are wrapped as C symbols with ABI-equivalent types.
#define PHASES(X) PHASES_TIMED(X) X(actor_frame, port_actor_frame, (int a), (a)) X(sound, sdat_host_tick, (void), ())

// Wrapped by WRAP below; sound has its own wrapper (it also takes the mix lock).
#define PHASES_TIMED(X)                                                                \
    X(frame_begin, hal_sub_screen_frame_begin, (void), ())                              \
    X(phase2, port_frame_phase2, (void), ())                                            \
    X(logic, port_actor_tick, (void), ())                                               \
    X(scanout, _ZN3ntr16rt_scanout_frameEv, (void), ())                                 \
    X(render_walk, port_actor_render, (void), ())                                       \
    X(phase5, port_frame_phase5, (void), ())                                            \
    X(clear, _ZN3ntr13gx_clear_fillERNS_11FramebufferE, (void *fb), (fb))                \
    X(gx_render, _ZN3ntr9gx_renderERNS_11FramebufferE, (void *fb), (fb))                 \
    X(msg2d, port_message_composite_engine_a, (void *fb), (fb))  /* engine A's 2D */      \
    X(sub_present, hal_sub_screen_present, (unsigned *d, int w, int h), (d, w, h))      \
    X(scene_pass, port_actor_scene_pass, (void), ())                                    \
    X(frame_end, port_actor_frame_end, (int f, const char *l), (f, l))

// scanout_sub has its own wrapper below (it joins the core 2 scan-out first),
// and so has settle (it also counts its reasons).
#define INSIDE(X)                                                                      \
    INSIDE_TIMED_A(X) X(settle, _ZN3ntr22gx_card_settle_becauseEPKc, (const char *why), (why)) \
    INSIDE_TIMED(X) X(scanout_sub, _ZN3ntr15ppu_scanout_subERNS_14SubFramebufferE, (void *s), (s))
#define INSIDE_TIMED_A(X)                                                              \
    X(vram_publish, _ZN3ntr16ppu_vram_publishEv, (void), ())                            \
    X(capture, _ZN3ntr19ppu_display_captureEPKmii, (const void *d, int w, int h), (d, w, h)) \
    X(oam_load, _ZN3OAM4LoadEv, (void), ())
#define INSIDE_TIMED(X)                                                                \
    X(menu, hal_menu_present, (unsigned *d, int w, int h, int st), (d, w, h, st))       \
    X(compose_sub, _ZN3ntr15ppu_compose_subERKNS_14SubFramebufferEPmiiiiii,             \
      (const void *s, void *d, int a, int b, int c, int e, int f, int g), (s, d, a, b, c, e, f, g)) \
    X(fade, port_engine_a_fade, (unsigned *p, int w, int h), (p, w, h))

namespace {

#define ENUM(name, sym, params, args) T_##name,
enum Timer { PHASES(ENUM) INSIDE(ENUM) T_COUNT };
#undef ENUM
#define ONE(name, sym, params, args) +1
constexpr int kPhases = 0 PHASES(ONE);
#undef ONE

#define NAME(name, sym, params, args) #name,
const char *const kNames[] = { PHASES(NAME) INSIDE(NAME) };
#undef NAME

u64 g_ticks[T_COUNT];

enum ActorPart { A_cleanup, A_init, A_behavior, A_render, A_scene_tree, A_count };
const char *const kActorNames[] = {"cleanup", "init", "behavior", "render", "scene_tree"};
struct ActorTiming {
    u64 ticks[A_count] = {};
    unsigned calls[A_count] = {};
    unsigned frames = 0, nested = 0, unexpected = 0;
} g_actor;
unsigned g_actor_depth, g_walk_depth;
bool g_in_render;   // inside the actor render walk ([perf] gx, below)
unsigned g_interlude_depth;

const char *const kTickNames[] = {"input", "requests", "actor", "post_actor",
    "fallback_render", "frame_end", "player_world", "particles", "wipe", "gx", "message", "sub_screen"};
constexpr unsigned kTickCount = static_cast<unsigned>(TickPhase::count);
static_assert(sizeof kTickNames / sizeof *kTickNames == kTickCount, "tick phase names");
struct TickTiming {
    u64 ticks[kTickCount] = {}, total = 0;
    unsigned frames = 0, bad_order = 0;
} g_tick;
u64 g_tick_start, g_tick_last;
unsigned g_tick_next;
bool g_tick_live;
// This frame alone, for phase_timers_slow_frame: its tick slices, and the HAL
// counters as they stood when it began.
u64 g_frame_tick[kTickCount];
u64 g_frame_hal0[T_COUNT];

struct Scope {
    Timer t;
    u64 t0;
    bool active;
    explicit Scope(Timer t) : t(t), t0(g_interlude_depth ? 0 : svcGetSystemTick()), active(!g_interlude_depth) {}
    ~Scope() { if (active) g_ticks[t] += svcGetSystemTick() - t0; }
};

// What a wrapped phase waits for before running: only gx_clear_fill, the first
// write to a frame's picture. Last frame's present on core 2 must have copied
// its inputs by then. The scene loop's tick is a single HAL call, so its fence
// is here; the level loop already waited, and a second wait is free.
//
// With GAME3DS_SUB_WATCH=1 in the scene loop each phase is also a checkpoint
// for main_game.cpp's engine B watch, with the end of hal_sub_screen_present
// as the base.
constexpr int scene_at(Timer t)
{
    return t == T_frame_begin ? 1 : t == T_phase2 ? 2 : t == T_logic ? 3 : t == T_phase5 ? 7
         : t == T_scanout ? 8 : t == T_clear ? 9 : t == T_gx_render ? 10 : t == T_msg2d ? 11
         : t == T_sub_present ? 12 : -1;
}
//
// The message compositor is also where last frame's engine B scan-out gets
// joined, since the ROM's vblank code at its head publishes engine B's
// registers and OAM. This is the scene loop's join (the level loop already
// joined), so the scan runs alongside the title / file select tick. Old 3DS
// only collects a finished scan here and never waits.
//
// Watchdog trail (watchdog3ds.cpp): the phase the game thread is in, the last
// one it finished, and any wait it's in. Plain stores; the watchdog only reads
// them once the game thread has stopped.
const char *volatile g_phase_in;
const char *volatile g_phase_done = "boot";
const char *volatile g_phase_wait;
const unsigned *volatile g_phase_sp;   // the stack pointer it entered with
template <Timer T> inline void before()
{
    g_phase_in = kNames[T];
    g_phase_sp = static_cast<const unsigned *>(__builtin_frame_address(0));
    if (scene_at(T) >= 0) game3ds_scene_watch(scene_at(T));
    if (T == T_clear) {
        g_phase_wait = "the last present's inputs (card3ds_wait_inputs)";
        card3ds_wait_inputs();
        g_phase_wait = nullptr;
    }
    if (T == T_msg2d) {
        g_phase_wait = "engine B's scan-out (game3ds_sub_join_tick)";
        game3ds_sub_join_tick();
        g_phase_wait = nullptr;
    }
}
template <Timer T> inline void after()
{
    if (T == T_sub_present) game3ds_scene_watch(-1);
    g_phase_done = kNames[T];
    g_phase_in = nullptr;
}

} // namespace

extern "C" {
void port_compa_zones_report(void);   // hal/message_compositor.cpp (3ds/patches)
extern char data_020a4ba8[], data_020a4b88[], data_020a4b78[], data_020a4b98[], data_020a4b6c[];
extern int data_02099f24;

#define WRAP(name, sym, params, args)                                                  \
    void __real_##sym params;                                                          \
    void __wrap_##sym params                                                           \
    {                                                                                  \
        before<T_##name>();                                                            \
        { Scope timer_(T_##name); __real_##sym args; }                                 \
        after<T_##name>();                                                             \
    }
PHASES_TIMED(WRAP)
INSIDE_TIMED_A(WRAP)
INSIDE_TIMED(WRAP)
#undef WRAP

// A settle reads the 3D picture back, so the frame isn't presented from the
// card. Count each reason gx.cpp gives (printed as "[perf] settles"). The
// reasons are string literals, so the pointer is the key.
struct SettleWhy {
    const char *why;
    unsigned n;
} g_settle_why[8];
void __real__ZN3ntr22gx_card_settle_becauseEPKc(const char *why);
void __wrap__ZN3ntr22gx_card_settle_becauseEPKc(const char *why)
{
    if (!g_interlude_depth && ntr::gx_card_overlay_active())
        for (SettleWhy &s : g_settle_why)
            if (s.why == why || !s.why) {
                s.why = why;
                ++s.n;
                break;
            }
    Scope timer_(T_settle);
    __real__ZN3ntr22gx_card_settle_becauseEPKc(why);
}

void __real__ZN3ntr15ppu_scanout_subERNS_14SubFramebufferE(void *s);
void __wrap__ZN3ntr15ppu_scanout_subERNS_14SubFramebufferE(void *s)
{
    game3ds_sub_join();
    Scope timer_(T_scanout_sub);
    game3ds_sub_scan_before();
    __real__ZN3ntr15ppu_scanout_subERNS_14SubFramebufferE(s);
    game3ds_sub_scanned(s);
}

void __real_port_actor_frame(int level_camera);
void __wrap_port_actor_frame(int level_camera)
{
    if (g_interlude_depth) { __real_port_actor_frame(level_camera); return; }
    game3ds_scene_watch(4);
    const bool outer = g_actor_depth++ == 0;
    const u64 start = svcGetSystemTick();
    if (!outer) ++g_actor.nested;
    __real_port_actor_frame(level_camera);
    if (outer) {
        g_ticks[T_actor_frame] += svcGetSystemTick() - start;
        ++g_actor.frames;
    }
    --g_actor_depth;
}

void *__real_func_02043fdc(void *list);
void *__real_func_020441cc(void *list);
static void *time_walk(void *list, bool tree)
{
    // Nested calls remain inside the enclosing walk. Calls made outside the
    // native actor frame (e.g. teardown) are deliberately excluded.
    if (!g_actor_depth || g_walk_depth) return tree ? __real_func_020441cc(list) : __real_func_02043fdc(list);
    int part = -1;
    if (!tree && list == data_020a4ba8 && data_02099f24 == 4) part = A_cleanup;
    if (!tree && list == data_020a4b88 && data_02099f24 == 2) part = A_init;
    if (!tree && list == data_020a4b78 && data_02099f24 == 3) part = A_behavior;
    if (!tree && list == data_020a4b98 && data_02099f24 == 5) part = A_render;
    if (tree && list == data_020a4b6c && data_02099f24 == 1) part = A_scene_tree;
    if (part < 0) ++g_actor.unexpected;
    if (part == A_behavior) game3ds_scene_watch(5);
    if (part == A_render) game3ds_scene_watch(6);
    ++g_walk_depth;
    g_in_render = part == A_render;
    const u64 start = svcGetSystemTick();
    void *result = tree ? __real_func_020441cc(list) : __real_func_02043fdc(list);
    const u64 elapsed = svcGetSystemTick() - start;
    g_in_render = false;
    --g_walk_depth;
    if (part >= 0) {
        g_actor.ticks[part] += elapsed;
        ++g_actor.calls[part];
    }
    return result;
}
// Geometry inside the render walk ([perf] gx). The ROM draws models by
// streaming display lists to the geometry FIFO and by single stores to the
// command ports (matrix loads etc.). Both are counted during the actor render
// walk; with GAME3DS_GX_PROFILE=1 the lists are also timed (one in 8). Stores
// are never timed since a clock read costs more than the store.
struct GxCount {
    u64 fifo_ticks;
    unsigned fifo_calls, fifo_words, io_calls, io_geom;
} g_gx;
bool gx_profile()
{
    static const bool on = getenv("GAME3DS_GX_PROFILE") != nullptr;
    return on;
}
void __real__ZN3ntr15gx_write_fifo_nEPKmi(const unsigned long *src, int n);
void __wrap__ZN3ntr15gx_write_fifo_nEPKmi(const unsigned long *src, int n)
{
    if (!g_in_render) { __real__ZN3ntr15gx_write_fifo_nEPKmi(src, n); return; }
    ++g_gx.fifo_calls;
    g_gx.fifo_words += (unsigned)n;
    // timed one list in 8 (each clock read is a system call), counted 8 times:
    // an estimate, close over the ~400 lists of a frame
    if (!gx_profile() || (g_gx.fifo_calls & 7)) { __real__ZN3ntr15gx_write_fifo_nEPKmi(src, n); return; }
    const u64 t0 = svcGetSystemTick();
    __real__ZN3ntr15gx_write_fifo_nEPKmi(src, n);
    g_gx.fifo_ticks += (svcGetSystemTick() - t0) * 8;
}
// Fast path for word stores to the geometry ports. io_write checks a store
// against the PPU audit, IPC, hooks, IF, the divider and sqrt before getting to
// geometry, and the file select does ~7000 of these a frame. For
// 0x04000400..0x040005FF none of those checks can match, so do what io_write
// would end up doing directly: latch the value and call gx_write_fifo /
// gx_write_port. Other widths, or SM64DS_PPU_AUDIT, still use io_write.
// GAME3DS_GEOM_STORE_SLOW=1 sends everything through io_write.
bool geom_store_fast()
{
    static const bool on = !getenv("GAME3DS_GEOM_STORE_SLOW") && !ntr::ppu_audit_on();
    return on;
}
void __real__ZN3ntr8io_writeEmyj(unsigned long addr, unsigned long long value, unsigned width);
void __wrap__ZN3ntr8io_writeEmyj(unsigned long addr, unsigned long long value, unsigned width)
{
    const bool geom = addr - 0x04000400u < 0x200u;
    if (g_in_render) {
        ++g_gx.io_calls;
        if (geom) ++g_gx.io_geom;
    }
    if (geom && width == 4 && geom_store_fast()) {
        *reinterpret_cast<volatile uint32_t *>(addr) = static_cast<uint32_t>(value);
        if (addr == 0x04000400u)
            ntr::gx_write_fifo(static_cast<uint32_t>(value));
        else if (addr >= 0x04000440u)
            ntr::gx_write_port(addr, static_cast<uint32_t>(value));
        return;
    }
    __real__ZN3ntr8io_writeEmyj(addr, value, width);
}

void *__wrap_func_02043fdc(void *list) { return time_walk(list, false); }
void *__wrap_func_020441cc(void *list) { return time_walk(list, true); }

// The game thread's sound frame holds game/out_3ds.cpp's mix lock: the mix
// thread renders (and runs the sequencer) between ticks, never during one.
int out3ds_sound_lock(void);
void out3ds_sound_unlock(int locked);
void __real_sdat_host_tick(void);
void __wrap_sdat_host_tick(void)
{
    Scope timer_(T_sound);
    const int locked = out3ds_sound_lock();
    __real_sdat_host_tick();
    out3ds_sound_unlock(locked);
}
}

PhaseTimersInterlude::PhaseTimersInterlude() { ++g_interlude_depth; }
PhaseTimersInterlude::~PhaseTimersInterlude() { --g_interlude_depth; }

void phase_timers_tick_begin(u64 tick)
{
    if (g_tick_live) ++g_tick.bad_order;
    g_tick_live = true;
    g_tick_start = g_tick_last = tick;
    g_tick_next = 0;
    for (u64 &value : g_frame_tick) value = 0;
    for (int i = 0; i < T_COUNT; ++i) g_frame_hal0[i] = g_ticks[i];
}

// A loop without tick slices (the title's): this frame's HAL phases start here
void phase_timers_frame_begin()
{
    for (u64 &value : g_frame_tick) value = 0;
    for (int i = 0; i < T_COUNT; ++i) g_frame_hal0[i] = g_ticks[i];
}

void phase_timers_tick_mark(TickPhase phase)
{
    const u64 now = svcGetSystemTick();
    const unsigned index = static_cast<unsigned>(phase);
    if (!g_tick_live || index != g_tick_next || index >= kTickCount - 1) {
        ++g_tick.bad_order;
        return;
    }
    g_tick.ticks[index] += now - g_tick_last;
    g_frame_tick[index] = now - g_tick_last;
    g_tick_last = now;
    ++g_tick_next;
    g_phase_done = kTickNames[index];
}

// The watchdog's trail (above), read from its own thread.
extern "C" void game3ds_phase_trail(const char **in, const char **done, const char **wait,
                                    const unsigned **sp)
{
    *in = g_phase_in;
    *done = g_phase_done;
    *wait = g_phase_wait;
    *sp = g_phase_sp;
}

void phase_timers_tick_end(u64 tick)
{
    if (!g_tick_live || g_tick_next != kTickCount - 1) ++g_tick.bad_order;
    g_tick.ticks[kTickCount - 1] += tick - g_tick_last;
    g_frame_tick[kTickCount - 1] = tick - g_tick_last;
    g_tick.total += tick - g_tick_start;
    ++g_tick.frames;
    g_tick_live = false;
}

// If sdmc:/sm64ds3ds/zones exists, also turn on the HAL's finer timers: engine
// A compositor zones (SM64DS_COMPA_ZONES) and the menu recipe cost
// (SM64DS_MENU_TRACE). Call before the scene's first frame.
void phase_timers_init()
{
    if (FILE *f = fopen("sdmc:/sm64ds3ds/zones", "r")) {
        fclose(f);
        setenv("SM64DS_COMPA_ZONES", "1", 1);
        setenv("SM64DS_MENU_TRACE", "1", 1);
        printf("[perf] zones: the compositor and menu clocks are on\n");
    }
}

// The largest entries of `ticks` over 1 ms, biggest first, at most four.
static void print_top(const char *const *names, const u64 *ticks, int n, double ms)
{
    bool used[T_COUNT > kTickCount ? T_COUNT : kTickCount] = {};
    for (int k = 0; k < 4; ++k) {
        int best = -1;
        for (int i = 0; i < n; ++i)
            if (!used[i] && ticks[i] * ms >= 1.0 && (best < 0 || ticks[i] > ticks[best]))
                best = i;
        if (best < 0) break;
        used[best] = true;
        printf(" %s %.0f", names[best], ticks[best] * ms);
    }
}

bool phase_timers_slow_frame(int frame, int level, bool level_changed, u64 outside, u64 tick,
                             u64 handoff, u64 after)
{
    static double limit = -1;
    if (limit < 0) {
        const char *e = getenv("GAME3DS_SLOW_MS");
        limit = e && *e ? atof(e) : 250.0;
    }
    const double ms = 1000.0 / (double)SYSCLOCK_ARM11;
    const double total = (outside + tick + handoff + after) * ms;
    if (limit <= 0 || total < limit) return false;
    u64 hal[T_COUNT];
    for (int i = 0; i < T_COUNT; ++i) hal[i] = g_ticks[i] - g_frame_hal0[i];
    if (level < 0)   // the title loop
        printf("[slow] f%d title: %.0f ms = outside the loop %.0f, tick %.0f [", frame, total, outside * ms,
               tick * ms);
    else
        printf("[slow] f%d level %d%s: %.0f ms = outside the loop %.0f, tick %.0f [", frame, level,
               level_changed ? " (level change)" : "", total, outside * ms, tick * ms);
    print_top(kTickNames, g_frame_tick, kTickCount, ms);
    printf(" ] hal [");
    print_top(kNames, hal, T_COUNT, ms);
    printf(" ], handoff %.0f, after %.0f\n", handoff * ms, after * ms);
    return true;
}

static void print_range(const char *label, int from, int to, double ms)
{
    printf("[perf]   %s:", label);
    for (int i = from; i < to; ++i) {
        printf(" %s %.2f", kNames[i], (double)g_ticks[i] * ms);
        g_ticks[i] = 0;
    }
    printf("\n");
}

// Per-frame means since the last call, then the counters reset.
void phase_timers_report(unsigned frames)
{
    if (!frames) return;
    const double ms = 1000.0 / (double)SYSCLOCK_ARM11 / frames;
    u64 actor_sum = 0;
    for (u64 value : g_actor.ticks) actor_sum += value;
    const long long actor_other = (long long)g_ticks[T_actor_frame] - (long long)actor_sum;
    printf("[perf] actor: %u frames, calls %u, total %.3f ms, other %.3f ms, gap_ticks %lld, nested_frames %u, unexpected_walks %u\n",
           frames, g_actor.frames, g_ticks[T_actor_frame] * ms, actor_other * ms,
           (long long)g_ticks[T_actor_frame] - (long long)actor_sum - actor_other, g_actor.nested, g_actor.unexpected);
    printf("[perf] actor_parts:");
    for (int i = 0; i < A_count; ++i) printf(" %s %.3f", kActorNames[i], g_actor.ticks[i] * ms);
    printf("\n[perf] actor_calls:");
    for (int i = 0; i < A_count; ++i) printf(" %s %u", kActorNames[i], g_actor.calls[i]);
    printf("\n");
    printf("[perf] gx: in the render walk per frame: fifo lists %.1f (%.0f words) %.3f ms%s, "
           "io stores %.0f (%.0f geometry)\n",
           (double)g_gx.fifo_calls / frames, (double)g_gx.fifo_words / frames,
           g_gx.fifo_ticks * ms, gx_profile() ? "" : " (untimed: GAME3DS_GX_PROFILE=1)",
           (double)g_gx.io_calls / frames, (double)g_gx.io_geom / frames);
    {   // ntr/gx.h's list memo
        uint32_t lists, hits, words, bad;
        ntr::gx_list_memo_stats(lists, hits, words, bad, true);
        if (lists)
            printf("[perf] list memo per frame: %.1f lists, %.1f replayed (%.0f words); "
                   "%u verify mismatches\n",
                   (double)lists / frames, (double)hits / frames, (double)words / frames, bad);
        if (getenv("GAME3DS_LIST_MEMO_WHY")) ntr::gx_list_memo_report();
    }
    {   // the geometry commands the whole frame ran, the most frequent first
        uint32_t n[256], ports, fifo, swap, resets;
        ntr::gx_debug_commands(n, ports, fifo, swap, resets, true);
        printf("[perf] gx_cmds per frame:");
        for (int k = 0; k < 10; ++k) {
            int best = -1;
            for (int c = 0; c < 256; ++c)
                if (n[c] && (best < 0 || n[c] > n[best])) best = c;
            if (best < 0) break;
            printf(" %02x:%.0f", best, (double)n[best] / frames);
            n[best] = 0;
        }
        printf("\n");
    }
    {   // step 5's sizing: what a GPU-side transform would record a frame
        unsigned sum[4], max[4];
        const int n = ntr::gx_step5_counts(sum, max);
        if (n > 0)
            printf("[perf] gx_record per frame (max): verts %.0f (%u) tris %.0f (%u) "
                   "runs %.0f (%u) matrix ids %.0f (%u), %d frames\n",
                   (double)sum[0] / n, max[0], (double)sum[1] / n, max[1], (double)sum[2] / n,
                   max[2], (double)sum[3] / n, max[3], n);
    }
    g_gx = {};
    g_actor = {};
    if (g_tick.frames) {
        u64 sum = 0;
        for (u64 value : g_tick.ticks) sum += value;
        printf("[perf] tick: %u frames, total %.3f ms, gap_ticks %lld, expected_frames %u, bad_order %u\n",
               g_tick.frames, g_tick.total * ms, (long long)g_tick.total - (long long)sum, frames, g_tick.bad_order);
        printf("[perf] tick_parts:");
        for (unsigned i = 0; i < kTickCount; ++i) printf(" %s %.3f", kTickNames[i], g_tick.ticks[i] * ms);
        printf("\n");
        g_tick = {};
    }
    print_range("phases", 0, kPhases, ms);
    print_range("inside", kPhases, T_COUNT, ms);
    if (g_settle_why[0].why) {
        printf("[perf] settles:");
        for (SettleWhy &s : g_settle_why) {
            if (!s.why) break;
            printf(" %s%s %u", &s == g_settle_why ? "" : ";", s.why, s.n);
            s = {};
        }
        printf("\n");
    }
    game3ds_sub_async_report(ms);
    port_compa_zones_report();
}
