#include "gpu3ds.h"

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "ntr/gx.h"

#include "gx_rec_shader_shbin.h"
#include "gx_shader_shbin.h"

extern "C" {
// citro3d's context (libcitro3d.a 1.7.1), whose first member is the queue C3D_Init
// binds (GX_BindQueue); its queue callback is citro3d's onQueueFinish
extern gxCmdQueue_s __C3D_Context;
}

namespace gpu3ds {
namespace {

constexpr int kTopW = 400, kTopH = 240, kBotW = 320;
constexpr size_t kMaxVerts = 3 * 16384;

constexpr u32 kDisplayTransferFlags =
    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

struct Vtx {
    float x, y, z, w;
    float s, t;
    float r, g, b, a;
    float fs, ft;          // the fog ramp's coordinate (fog_stage)
};

// Textures are keyed by gx.cpp's tex_id, not the pointer: the decode cache
// reuses addresses, so a pointer alone can return a stale texture. Without a
// backend registered (view_model) tex_id is 0 and the pointer is the key.
struct TexKey {
    uint32_t id;
    const uint32_t *px;
    int w, h;
    bool operator==(const TexKey &o) const {
        return id == o.id && px == o.px && w == o.w && h == o.h;
    }
};
struct TexKeyHash {
    size_t operator()(const TexKey &k) const {
        return (size_t)k.id * 2654435761u ^ (size_t)k.px ^ ((size_t)k.w << 20) ^ ((size_t)k.h << 8);
    }
};

TexKey tex_key(const ntr::GxTriangle &t)
{
    return TexKey{t.tex_id, t.tex_id ? nullptr : t.tex, t.tw, t.th};
}

bool g_up;
C3D_RenderTarget *g_top;
C3D_RenderTarget *g_bot;      // made on first use: tests keep the console there
C3D_Mtx g_proj_bot;
C3D_Tex g_toon_tex;          // 32 x 8, every row the frame's toon table
bool g_toon_up;
float g_toon_rgb[32 * 3];    // what it holds (set_toon writes it only when it changes)
// Fog: this frame's registers and the ramp texture (unit 2). Texel i holds
// visibility 1 - min(i, 128) / 128, so s = (density + 0.5) / 256 samples the
// vertex's own density.
ntr::GxFog g_fog;
bool g_fog_on;
bool g_fog_w;
C3D_Tex g_fog_tex;           // 256 x 8
bool g_fog_up;
// draw_fade_masked's verdicts, kMaskW x 256: two, written in turn, so a frame
// built ahead writes the one the GPU is not reading
C3D_Tex g_mask_tex[2];
bool g_mask_up[2];
int g_mask_cur;
int g_toon_shade;            // this frame's: 0 none, 1 toon, 2 highlight
int g_tgt_h = kTopH;      // the drawn target's screen height (its buffer width)
int g_tgt_w = kTopW;      // and its screen width
// its buffer pixels per screen pixel (2 on the FSAA target): across the
// screen (the buffer's height) and down it (the buffer's width)
int g_tgt_kx = 1, g_tgt_ky = 1;

// FSAA: the top screen's 3D renders at 2x width (GAME3DS_FSAA=1, 800x240) or
// 2x both ways (=2, 800x480) into g_top_ss, and resolve() draws it back onto
// g_top as one bilinear quad. 2x width costs half of 2x2 and still smooths
// most edges.
int g_ss_ky = 1;          // 2 with GAME3DS_FSAA=2
int g_ss_w = kTopH, g_ss_h = kTopW * 2;   // the target, tilted as g_top (set at init)
int g_ss_tex_w = 256;     // the texture's width (the target's g_ss_w of it)
C3D_Tex g_ss_tex;
C3D_RenderTarget *g_top_ss;
void *g_ss_depth;
bool g_ss_active;         // this frame draws on g_top_ss until resolve()
bool g_ss_want;           // the options menu's anti-aliasing switch (set_fsaa), or GAME3DS_FSAA
// Stereo 3D: the right eye's target (made the first time 3D is on), whether
// we draw once per eye, which eye frame_target is drawing, the slider, and the
// record shader's eye uniform (see shaders/gx_rec.v.pica).
C3D_RenderTarget *g_top_r;
bool g_stereo;
int g_eye;
float g_slider;
float g_eye_vec[4];
int g_rloc_eye = -1;
bool g_keep_overlays;     // frame_keep_overlays() was called for the next frame
// what each screen shows, as screen_mark named it (0: unknown)
volatile uint64_t g_screen_key[2];

// citro3d's GPU drawing time of each frame (C3D_GetDrawingTime, read as the
// next one begins), published every 120 frames for gpu_time_line.
struct GpuTime {
    float sum, max;
    unsigned frames;
    unsigned vbl[4];   // vblanks between this frame's begin and the last's: 1, 2, 3, 4+
    unsigned waits;    // frames that waited for the last one's present (wait_present)
    unsigned ov_kept, ov_sent;   // overlay_transfer: pictures kept as they were, sent
    unsigned ov_bad;             // GPU3DS_OVERLAY_VERIFY: keeps the hash disagreed with
    unsigned bot_held, bot_drawn;   // the other screen left as it was (screen_holds), drawn
    unsigned fade_one, fade_three;  // fade_masked_draw: frames in one pass, in three
    // Frames built ahead (ahead_flush): count, how many had to flush early, and
    // how long the build waited on the last frame's queue.
    unsigned ahead, ahead_early;
    double ahead_wait;
    // their GPU work from the hand-over to the queue's end, where it ended
    // before the frame's vblank; and how many were still on the GPU then
    double ahead_gpu;
    unsigned ahead_gpu_n, ahead_gpu_late;
};
GpuTime g_gt, g_gt_ready;
volatile bool g_gt_has;
// set_frame_tag / gpu_tag_line: the GPU time a frame by tag
struct TagTime {
    double sum[8];
    unsigned n[8];
    unsigned frames;
};
TagTime g_tag, g_tag_ready;
volatile bool g_tag_has;
bool g_tag_on;
int g_tag_cur, g_tag_prev;
const char *g_tag_name[8];
DVLB_s *g_dvlb;
shaderProgram_s g_prog;
int g_loc_proj;
C3D_Mtx g_proj;
const C3D_Mtx *g_cur_proj = &g_proj;   // the drawn target's (frame_target)

// Step 5 record path (draw_record, shaders/gx_rec.v.pica): its own program,
// vertex layout and buffers. Vertices are ntr::GxRecVertex as-is (x, y, z,
// matrix id with bit 15 for perspective, colour bytes, texcoords), triangles
// are 16-bit indices. kRing matrices stay resident in the shader, so a draw
// can only span fewer than kRing matrix ids.
static_assert(sizeof(ntr::GxRecVertex) == 20, "the record's vertex layout");
constexpr int kRing = 20;
constexpr size_t kRecVerts = 16384, kRecIdx = 3 * 24576;
DVLB_s *g_rec_dvlb;
shaderProgram_s g_rec_prog;
bool g_rec_up;
int g_rloc_proj, g_rloc_texscale, g_rloc_colk, g_rloc_colh, g_rloc_fogk, g_rloc_ring;
C3D_AttrInfo g_attr_main, g_attr_rec;
C3D_BufInfo g_buf_main, g_buf_rec;
// ONE linear block a frame, indices first and the vertices after them, so a
// single cache flush covers both.
uint8_t *g_rblock;
constexpr size_t kRBlock =
    ((kRecIdx * sizeof(uint16_t) + 63) & ~(size_t)63) + kRecVerts * sizeof(ntr::GxRecVertex);
bool g_rec_drawn;             // draw_record ran this frame (its buffers are taken)
// what g_rblock holds (draw_record's upload once a job; record_upload_forget)
bool g_rb_valid;
const void *g_rb_idx, *g_rb_v;
size_t g_rb_ni, g_rb_nv;
// Depth fog for the record's draws: texel n is the visibility at FOG_TABLE
// entry n, interpolated between entries. The shader's texcoord2 s is
// g_fogk[0] * w + g_fogk[1] * z + g_fogk[2].
C3D_Tex g_fogd_tex;           // 32 x 8
bool g_fogd_up;
float g_fogk[3];
Vtx *g_vbo;
size_t g_used;
std::unordered_map<TexKey, C3D_Tex, TexKeyHash> g_tex;
uint32_t g_tex_gen;
Stats g_stats;
unsigned long g_tex_total, g_tex_fails;   // uploads and failed uploads, whole run
int g_frame_no;               // frames begun, for GPU3DS_TRACE
C3D_Tex g_ov[kMaxOverlays];   // the 2D overlays, re-uploaded every frame
bool g_ov_up[kMaxOverlays];
int g_ov_w[kMaxOverlays], g_ov_h[kMaxOverlays];   // the used part of each this frame
int g_mask_w, g_mask_h;       // fade_mask_upload's, this frame
bool g_mask_split;            // ...and whether its two verdicts differ anywhere
// the mask texture's width: the picture's, to a power of two (512 on the
// widescreen build's 320-wide picture)
constexpr int kMaskW = ntr::SCREEN_W > 256 ? 512 : 256;
int g_ov_used;                // overlays drawn this frame
constexpr int kOvW = 512, kOvH = 256;
// readback()'s offscreen target, the DS picture at 1:1, and its linear copy.
constexpr int kOffW = (ntr::SCREEN_W + 7) & ~7, kOffH = (ntr::SCREEN_H + 7) & ~7;
C3D_RenderTarget *g_off;
u32 *g_off_lin;
uint32_t g_off_argb[kOffW * kOffH];
int g_off_done[4];   // the area of g_off_argb made from g_off_lin: x0, y0, x1, y1

// Building frames ahead of the GPU (ahead_ready / ahead_flush).
//
// C3D_FrameBegin waits for citro3d's queue, so an in-between frame couldn't
// start until the GPU finished the frame before it, and ticks ran past their
// two vblanks. A 1-vblank frame now builds without that wait: its commands go
// in their own buffer (g_cmd), vertices in the other half of g_vbo, the record
// in the other g_rblock block, and the queue entries and overlay transfers are
// held in g_hold / g_xfer. ahead_flush waits for the last frame, runs the
// transfers and hands the rest to the GPU.
//
// GAME3DS_AHEAD=0 goes back to the old order.
bool g_ahead_ok;              // ahead_init made the buffers
bool g_ahead;                 // the frame being built is ahead (its queue entries held)
bool g_ahead_begin;           // inside its C3D_FrameBegin: no wait, no stop, no clear
u32 g_ahead_tid;              // the thread building it
constexpr int kHoldMax = 96, kXferMax = 16;
gxCmdEntry_s g_hold_e[kHoldMax], g_xfer_e[kXferMax];
gxCmdQueue_s g_hold, g_xfer;
constexpr size_t kCmdWords = C3D_DEFAULT_CMDBUF_SIZE;   // citro3d's own: C3D_Init's bytes / 4
u32 *g_cmd[2];                // the ahead frames' command lists, alternately
int g_cmd_i;
u32 *g_cmd_prev;              // the last frame's (citro3d's own for a frame not ahead)
size_t g_vlo, g_vhi = kMaxVerts;   // this frame's part of g_vbo
size_t g_vprev_lo, g_vprev_hi;      // the last frame's (its GPU work may still read it)
constexpr int kRBlocks = 2;   // g_rblock's blocks (draw_record uploads into them in turn)
int g_rb_n = 1, g_rb_cur;
u64 g_ahead_t_run;            // when ahead_flush last handed a frame over
bool g_was_ahead;             // this frame was built ahead (frame_end's GPU time)
// citro3d's queue callback (its onQueueFinish), with the time of each end of
// the queue noted first
void (*g_c3d_queue_cb)(gxCmdQueue_s *);
volatile u64 g_queue_end;
void queue_end_hook(gxCmdQueue_s *q)
{
    g_queue_end = svcGetSystemTick();
    __dmb();
    if (g_c3d_queue_cb) g_c3d_queue_cb(q);
}

// Only the game thread's card3ds_present is active here. Readbacks during
// the tick, screenshots and renderer tests do not enter this interval.
PresentTiming g_present;
bool g_present_active;
PresentZone g_present_zone = P_OTHER;
u64 g_present_last, g_present_start;
PresentMarks g_marks, g_marks_done;   // the open interval's, the last closed one's

float marks_ms(u64 t)
{
    return (float)((double)(t - g_present_start) * 1000.0 / (double)SYSCLOCK_ARM11);
}
float ticks_ms(u64 t)
{
    return (float)((double)t * 1000.0 / (double)SYSCLOCK_ARM11);
}

void timing_switch(PresentZone zone)
{
    const u64 now = svcGetSystemTick();
    g_present.ticks[g_present_zone] += now - g_present_last;
    g_present_last = now;
    g_present_zone = zone;
}

// Nested scopes charge the enclosing zone only while its child is absent.
// No per-vertex clock reads: the geometry loop is timed around whole runs.
struct PresentScope {
    PresentZone previous;
    bool active;
    explicit PresentScope(PresentZone zone)
        : previous(g_present_zone), active(g_present_active)
    {
        if (active) timing_switch(zone);
    }
    ~PresentScope() { if (active) timing_switch(previous); }
};

// ---- textures ------------------------------------------------------------

// PICA textures are 8x8 tiles in Morton order, tiles row-major from the bottom
// of the image. RGBA8 texels are 0xRRGGBBAA; gx.cpp's are 0xAARRGGBB, top row
// first.
inline u32 morton8(u32 x, u32 y)
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
           ((x & 4) << 2) | ((y & 4) << 3);
}

bool pow2_in_range(int v) { return v >= 8 && v <= 1024 && (v & (v - 1)) == 0; }

C3D_Tex *texture_for(const ntr::GxTriangle &t)
{
    const TexKey key = tex_key(t);
    auto it = g_tex.find(key);
    if (it != g_tex.end()) {
        ++g_stats.tex_cached;
        return &it->second;
    }
    if (!pow2_in_range(t.tw) || !pow2_in_range(t.th)) { ++g_tex_fails; return nullptr; }
    C3D_Tex tex;
    if (!C3D_TexInit(&tex, (u16)t.tw, (u16)t.th, GPU_RGBA8)) { ++g_tex_fails; return nullptr; }
    ++g_tex_total;
    u32 *dst = (u32 *)tex.data;
    const u32 tiles_x = (u32)t.tw >> 3;
    for (int y = 0; y < t.th; ++y) {
        const u32 fy = (u32)(t.th - 1 - y);
        const uint32_t *row = t.tex + (size_t)y * t.tw;
        for (int x = 0; x < t.tw; ++x) {
            const u32 c = row[x];
            const u32 rgba = ((c >> 16 & 0xFF) << 24) | ((c >> 8 & 0xFF) << 16) |
                             ((c & 0xFF) << 8) | (c >> 24);
            const u32 tile = (fy >> 3) * tiles_x + ((u32)x >> 3);
            dst[tile * 64 + morton8((u32)x & 7, fy & 7)] = rgba;
        }
    }
    C3D_TexFlush(&tex);
    C3D_TexSetFilter(&tex, GPU_NEAREST, GPU_NEAREST);
    ++g_stats.tex_uploads;
    return &g_tex.emplace(key, tex).first->second;
}

GPU_TEXTURE_WRAP_PARAM wrap_mode(bool repeat, bool flip)
{
    if (!repeat) return GPU_CLAMP_TO_EDGE;
    return flip ? GPU_MIRRORED_REPEAT : GPU_REPEAT;
}

// ---- state ---------------------------------------------------------------

// Mode 2 (toon/highlight): texture unit 1 holds the toon table, sampled at the
// vertex's red channel. Toon replaces the vertex colour with the table entry;
// highlight modulates the texture by the vertex colour and adds the entry.
void env_toon(bool textured, int shade)
{
    C3D_TexBind(1, &g_toon_tex);
    C3D_TexEnv *e0 = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e0);
    C3D_TexEnv *e1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(e1);
    if (shade == 1) {
        if (textured) {
            C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(e0, C3D_RGB, GPU_MODULATE);
        } else {
            C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(e0, C3D_RGB, GPU_REPLACE);
        }
    } else {
        C3D_TexEnvSrc(e0, C3D_RGB, textured ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR,
                      GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(e0, C3D_RGB, textured ? GPU_MODULATE : GPU_REPLACE);
        C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(e1, C3D_RGB, GPU_ADD);
    }
    C3D_TexEnvSrc(e0, C3D_Alpha, textured ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR,
                  GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e0, C3D_Alpha, textured ? GPU_MODULATE : GPU_REPLACE);
    if (textured)
        C3D_AlphaTest(true, GPU_GREATER, 0);
    else
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
}

void env_textured(bool textured)
{
    C3D_TexEnvInit(C3D_GetTexEnv(1));   // a pass-through after a mode-2 run
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (textured) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
        // gx.cpp: a texel of alpha 0 is not drawn at all (no colour, no depth).
        C3D_AlphaTest(true, GPU_GREATER, 0);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    }
}

void pass_state(bool translucent)
{
    // Depth is stored reversed here (near is larger, far is 0), so the DS's LESS
    // becomes GEQUAL. Equal is included because the DS lets a polygon at the same
    // depth draw over (the title backdrop relies on that).
    //
    // Destination alpha is used as coverage: the target clears to 0 and 3D writes
    // 1, so readback() and 2D layered behind the 3D can tell where 3D drew.
    //
    // The stencil holds gx.cpp's per-pixel attributes:
    //   bits 0-5  polygon ID
    //   bit 6     a translucent fragment landed here this frame
    //   bit 7     shadow mask (mode 3, ID 0)
    C3D_BlendingColor(0xFF000000u);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    if (translucent) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_CONSTANT_ALPHA, GPU_ONE);
        C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);
    } else {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_CONSTANT_ALPHA,
                       GPU_ZERO);
        C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
    }
}

// Stencil for a normal run: opaque writes its polygon ID; translucent is
// rejected where its own ID already landed this frame (the DS's translucent ID
// rule) and writes 0x40 | ID otherwise. Without this Peach's model, which
// fades in with every part at ID 0, drew its crown over its hair.
//
// TexEnv stage 2 is fog or a pass-through: colour = prev * visibility +
// fog colour * (1 - visibility), with visibility from texture 2's alpha.
bool g_fog_by_depth;   // draw_record: texture 2 is the depth ramp (g_fogd_tex)
void fog_stage(bool on)
{
    C3D_TexEnv *e = C3D_GetTexEnv(2);
    C3D_TexEnvInit(e);
    if (!on) return;
    C3D_TexBind(2, g_fog_by_depth ? &g_fogd_tex : &g_fog_tex);
    C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_TEXTURE2);
    C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_INTERPOLATE);
    const u32 c = g_fog.color_argb;
    C3D_TexEnvColor(e, 0xFF000000u | (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF));
}

// The vertex's DS fog density on the ramp (fog_stage): the 15-bit depth is
// w * 8 for a W frame, z * 0x7FFF for a Z frame (ntr/gx.h, GxFog).
float fog_coord(const ntr::GxVertex &v)
{
    const float d = g_fog_w ? v.w * 8.0f : v.z * 32767.0f;
    const int d15 = d <= 0.0f ? 0 : d >= 32767.0f ? 32767 : (int)d;
    return ((float)ntr::gx_fog_density(g_fog, d15) + 0.5f) / 256.0f;
}

void run_stencil(bool translucent, unsigned polyid)
{
    polyid &= 0x3F;
    if (translucent)
        C3D_StencilTest(true, GPU_NOTEQUAL, (int)(0x40 | polyid), 0x7F, 0x7F);
    else
        C3D_StencilTest(true, GPU_ALWAYS, (int)polyid, 0xFF, 0x7F);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
}

// gx.cpp's rule: screen y runs down, so a front face has negative area;
// POLYGON_ATTR bit 6 draws back faces, bit 7 front faces.
bool culled(const ntr::GxTriangle &t)
{
    const ntr::GxVertex &a = t.v[0], &b = t.v[1], &c = t.v[2];
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (fabsf(area) < 1e-6f) return true;
    const bool back = area > 0.0f;
    return back ? !(t.cull & 1) : !(t.cull & 2);
}

// The record shader's eye uniform for a shift k (signed by eye, 0 = none).
// GAME3DS_STEREO_PX (e, default 6): pixels each eye moves at infinity with the
// slider at the top. GAME3DS_STEREO_DEPTH (d, default 9): the depth at the
// screen plane, just behind Mario so he pops out a bit. GAME3DS_STEREO_POP
// (c, default 0.6): how far in front of the screen, as a fraction of e.
void stereo_eye_vec(float k)
{
    static float e = -1.0f, d, c;
    if (e < 0.0f) {
        const char *ep = getenv("GAME3DS_STEREO_PX"), *dp = getenv("GAME3DS_STEREO_DEPTH"),
                   *cp = getenv("GAME3DS_STEREO_POP");
        e = ep ? (float)atof(ep) : 6.0f;
        d = dp ? (float)atof(dp) : 9.0f;
        c = cp ? (float)atof(cp) : 0.6f;
        if (e < 0.0f) e = 0.0f;
        printf("[gpu3ds] 3D: %.2f pixels an eye at infinity, the screen's plane at w %.3f, "
               "in front up to %.2f of that\n", e, d, c);
    }
    g_eye_vec[0] = e * k;
    g_eye_vec[1] = d;
    g_eye_vec[2] = c;
    g_eye_vec[3] = 0.0f;
}

// The FSAA target (g_top_ss), made at init under GAME3DS_FSAA or the first
// time set_fsaa turns it on.
void ss_create(int ky)
{
    g_ss_ky = ky;
    g_ss_w = kTopH * g_ss_ky;
    g_ss_tex_w = g_ss_ky == 2 ? 512 : 256;
    // The texture's own target has no depth; ours is 256 (512) x 800, the first
    // 800 rows of the texture in memory.
    if (C3D_TexInitVRAM(&g_ss_tex, g_ss_tex_w, 1024, GPU_RGBA8)) {
        C3D_TexSetFilter(&g_ss_tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&g_ss_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        g_top_ss = C3D_RenderTargetCreateFromTex(&g_ss_tex, GPU_TEXFACE_2D, 0, -1);
        g_ss_depth = vramAlloc((size_t)g_ss_tex_w * g_ss_h * 4);
        if (g_top_ss && g_ss_depth) {
            C3D_FrameBufAttrib(&g_top_ss->frameBuf, g_ss_tex_w, g_ss_h, false);
            C3D_FrameBufDepth(&g_top_ss->frameBuf, g_ss_depth, GPU_RB_DEPTH24_STENCIL8);
        } else {
            if (g_top_ss) C3D_RenderTargetDelete(g_top_ss);
            if (g_ss_depth) vramFree(g_ss_depth);
            C3D_TexDelete(&g_ss_tex);
            g_top_ss = nullptr;
            g_ss_depth = nullptr;
        }
    }
    printf("[gpu3ds] FSAA %s (%dx%d target; vram free %lu)\n", g_top_ss ? "ON" : "FAILED, off",
           g_ss_h, g_ss_w, (unsigned long)vramSpaceFree());
}

}  // namespace

bool init()
{
    if (g_up) return true;
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) return false;
    g_top = C3D_RenderTargetCreate(kTopH, kTopW, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!g_top) return false;
    C3D_RenderTargetSetOutput(g_top, GFX_TOP, GFX_LEFT, kDisplayTransferFlags);
    if (getenv("GAME3DS_FSAA") && atoi(getenv("GAME3DS_FSAA")) != 0) {
        ss_create(atoi(getenv("GAME3DS_FSAA")) == 2 ? 2 : 1);
        g_ss_want = true;
    }

    g_dvlb = DVLB_ParseFile((u32 *)gx_shader_shbin, gx_shader_shbin_size);
    shaderProgramInit(&g_prog);
    shaderProgramSetVsh(&g_prog, &g_dvlb->DVLE[0]);
    C3D_BindProgram(&g_prog);
    g_loc_proj = shaderInstanceGetUniformLocation(g_prog.vertexShader, "projection");

    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);   // x, y, z, w
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);   // s, t
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 4);   // r, g, b, a
    AttrInfo_AddLoader(attr, 3, GPU_FLOAT, 2);   // fog ramp s, t

    g_vbo = (Vtx *)linearAlloc(kMaxVerts * sizeof(Vtx));
    if (!g_vbo) return false;
    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_vbo, sizeof(Vtx), 4, 0x3210);

    // Screen pixels (x right, y down) to the rotated top screen. z arrives
    // negated (see draw), so near = 0 and far = 1 bracket gx.cpp's [0,1].
    Mtx_OrthoTilt(&g_proj, 0.0f, (float)kTopW, (float)kTopH, 0.0f, 0.0f, 1.0f, false);
    C3D_CullFace(GPU_CULL_NONE);   // culling is done on the CPU, by gx.cpp's rule

    // Step 5's program and layout (draw_record); the main ones are kept to
    // switch back to.
    g_attr_main = *attr;
    g_buf_main = *buf;
    g_rec_dvlb = DVLB_ParseFile((u32 *)gx_rec_shader_shbin, gx_rec_shader_shbin_size);
    // two blocks (frames built ahead upload into them in turn), else one
    g_rblock = (uint8_t *)linearAlloc(kRBlocks * kRBlock);
    g_rb_n = g_rblock ? kRBlocks : 1;
    if (!g_rblock) g_rblock = (uint8_t *)linearAlloc(kRBlock);
    if (g_rec_dvlb && g_rblock) {
        shaderProgramInit(&g_rec_prog);
        shaderProgramSetVsh(&g_rec_prog, &g_rec_dvlb->DVLE[0]);
        shaderInstance_s *vs = g_rec_prog.vertexShader;
        g_rloc_proj = shaderInstanceGetUniformLocation(vs, "projection");
        g_rloc_texscale = shaderInstanceGetUniformLocation(vs, "texscale");
        g_rloc_colk = shaderInstanceGetUniformLocation(vs, "colk");
        g_rloc_colh = shaderInstanceGetUniformLocation(vs, "colh");
        g_rloc_fogk = shaderInstanceGetUniformLocation(vs, "fogk");
        g_rloc_ring = shaderInstanceGetUniformLocation(vs, "ring");
        g_rloc_eye = shaderInstanceGetUniformLocation(vs, "eye");
        AttrInfo_Init(&g_attr_rec);
        AttrInfo_AddLoader(&g_attr_rec, 0, GPU_SHORT, 4);           // x, y, z, id
        AttrInfo_AddLoader(&g_attr_rec, 1, GPU_UNSIGNED_BYTE, 4);   // b, g, r, a
        AttrInfo_AddLoader(&g_attr_rec, 2, GPU_FLOAT, 2);           // u, v
        g_rec_up = g_rloc_ring >= 0 && g_rloc_proj >= 0;
    }
    printf("[gpu3ds] step 5 record program %s (ring at c%d, projection c%d / main c%d)\n",
           g_rec_up ? "ready" : "MISSING", g_rloc_ring, g_rloc_proj, g_loc_proj);
    g_up = true;
    return true;
}

static void dump_write();

void shutdown()
{
    if (!g_up) return;
    dump_write();
    printf("gpu3ds: %lu texture uploads, %lu failed, %u cached at exit, linear free %lu\n",
           g_tex_total, g_tex_fails, (unsigned)g_tex.size(), (unsigned long)linearSpaceFree());
    textures_invalidate();
    for (int i = 0; i < kMaxOverlays; ++i)
        if (g_ov_up[i]) { C3D_TexDelete(&g_ov[i]); g_ov_up[i] = false; }
    overlay_transfers_free();
    linearFree(g_vbo);
    shaderProgramFree(&g_prog);
    DVLB_Free(g_dvlb);
    if (g_rec_up) shaderProgramFree(&g_rec_prog);
    if (g_rec_dvlb) DVLB_Free(g_rec_dvlb);
    if (g_rblock) linearFree(g_rblock);
    g_rec_dvlb = nullptr;
    g_rblock = nullptr;
    g_rec_up = false;
    if (g_fogd_up) { C3D_TexDelete(&g_fogd_tex); g_fogd_up = false; }
    if (g_off) { C3D_RenderTargetDelete(g_off); g_off = nullptr; }
    if (g_off_lin) { linearFree(g_off_lin); g_off_lin = nullptr; }
    C3D_RenderTargetDelete(g_top);
    if (g_top_r) { C3D_RenderTargetDelete(g_top_r); g_top_r = nullptr; }
    if (g_top_ss) {
        C3D_RenderTargetDelete(g_top_ss);
        vramFree(g_ss_depth);
        C3D_TexDelete(&g_ss_tex);
        g_top_ss = nullptr;
        g_ss_depth = nullptr;
    }
    if (g_bot) { C3D_RenderTargetDelete(g_bot); g_bot = nullptr; }
    if (g_toon_up) { C3D_TexDelete(&g_toon_tex); g_toon_up = false; }
    if (g_fog_up) { C3D_TexDelete(&g_fog_tex); g_fog_up = false; }
    for (int i = 0; i < 2; ++i)
        if (g_mask_up[i]) { C3D_TexDelete(&g_mask_tex[i]); g_mask_up[i] = false; }
    C3D_Fini();
    g_up = false;
}

// Frame pacing. C3D_FrameRate(30) presents on a fixed 30 Hz grid, so a frame
// 1 ms late waits a whole slot. Instead citro3d runs at 60 Hz and a frame that
// spans n vblanks first waits until n - 1 have passed since the last one
// began, using GSP's sticky VBlank0 event. A slow frame now costs one vblank,
// not a 30 Hz slot.
namespace {
int g_vblanks = 1;
int g_vblanks_override;   // set_vblanks_override: the next frames' own count

void pace_frame()
{
    const int n = g_vblanks_override > 0 ? g_vblanks_override : g_vblanks;
    for (int i = 1; i < n; ++i)
        gspWaitForEvent(GSPGPU_EVENT_VBlank0, i > 1);
}

// Wait until the last frame is actually on screen before drawing this one.
// citro3d swaps buffers when a frame's GPU work ends, but GSP only shows it at
// the next vblank. With 1-vblank interpolation frames the next frame could
// overwrite the buffer still being shown and drop the in-between (the flicker
// seen on hardware, not in Azahar).
void wait_present()
{
    if (!gspIsPresentPending(0) && !gspIsPresentPending(1)) return;
    ++g_gt.waits;
    for (int i = 0; i < 200 && (gspIsPresentPending(0) || gspIsPresentPending(1)); ++i)
        svcSleepThread(200000);   // 0.2 ms; a vblank at most (40 ms cap)
}

void pace_frame_begun()
{
    // The vblank SYNCDRAW just waited for is this frame's start; forget it.
    // That vblank has set the event, so this wait returns at once, clearing it.
    if (g_vblanks_override > 0 || g_vblanks > 1)
        gspWaitForEvent(GSPGPU_EVENT_VBlank0, false);
}

// Count vblanks the same way citro3d does: vblank_hook is wrapped in front of
// its onVBlank0/1 handlers, so g_vbl moves exactly when frame_sync's counters
// do.
volatile u32 g_vbl[2];
ThreadFunc g_vbl_cb[2];
void *g_vbl_data[2];
void vblank_hook0(void *) { ++g_vbl[0]; if (g_vbl_cb[0]) g_vbl_cb[0](g_vbl_data[0]); }
void vblank_hook1(void *) { ++g_vbl[1]; if (g_vbl_cb[1]) g_vbl_cb[1](g_vbl_data[1]); }

// Build 1-vblank frames before their vblank. SYNCDRAW waits for the vblank
// first and only then lets the frame build, so the GPU started 4-6 ms late and
// heavy frames missed their vblank. Here frame_begin only waits for the last
// frame's GPU work, the frame is built, and frame_end waits for the vblank
// and submits. Same vblank, same picture, the build just happens earlier.
// GAME3DS_VBLANK_FIRST=1 restores the old order.
bool g_frame_late;         // this frame waits in frame_end (frame_sync)
u32 g_late_vbl[2];         // g_vbl as frame_begin noted it
u64 g_late_qw;             // its GPU-queue wait, for the timeline

bool vblank_first_env()
{
    static const bool on = getenv("GAME3DS_VBLANK_FIRST") != nullptr;
    return on;
}
}  // namespace

void set_vblanks_per_frame(int n)
{
    if (n < 1) n = 1;
    g_vblanks = n;
}

void set_vblanks_override(int n)
{
    g_vblanks_override = n > 0 ? n : 0;
}

int vblanks_per_frame()
{
    return g_vblanks;
}

void textures_invalidate()
{
    for (auto &kv : g_tex) C3D_TexDelete(&kv.second);
    g_tex.clear();
}

namespace {

void textures_check_generation()
{
    const uint32_t gen = ntr::gx_texture_generation();
    if (gen != g_tex_gen) {
        textures_invalidate();
        g_tex_gen = gen;
    }
}

}  // namespace

void screen_size(Screen s, int *w, int *h)
{
    *w = s == SCREEN_BOTTOM ? kBotW : kTopW;
    *h = kTopH;
}

namespace {
// Called once a frame's waits are done: updates the timeline, the shown rate,
// the last frame's GPU time, and clears the pacing vblank.
void frame_begun(u64 vblank_ticks, u64 queue_ticks, u64 present_ticks)
{
    const int mf = g_marks.frames;
    if (g_present_active && mf < 2) {   // the timeline (PresentMarks)
        g_marks.begun[mf] = marks_ms(svcGetSystemTick());
        g_marks.vblank_wait[mf] = ticks_ms(vblank_ticks);
        g_marks.queue_wait[mf] = ticks_ms(queue_ticks);
        g_marks.present_wait[mf] = ticks_ms(present_ticks);
        g_marks.gpu_before[mf] = C3D_GetDrawingTime();
        g_marks.frames = mf + 1;
    }
    {   // the displayed rate: vblanks from the last frame's begin to this one's
        static u64 last;
        const u64 now = svcGetSystemTick();
        if (last) {
            const u64 vblank = SYSCLOCK_ARM11 * 1000ull / 59831;   // 59.831 Hz
            u64 n = (now - last + vblank / 2) / vblank;
            if (n < 1) n = 1;
            ++g_gt.vbl[n > 4 ? 3 : n - 1];
        }
        last = now;
    }
    if (g_tag_on) {   // set_frame_tag: the last frame's GPU time, filed under its tag
        if (g_frame_no > 1) {
            const float ms = C3D_GetDrawingTime();
            g_tag.sum[g_tag_prev] += ms;
            ++g_tag.n[g_tag_prev];
            if (++g_tag.frames == 1200) {
                if (!g_tag_has) {
                    g_tag_ready = g_tag;
                    __dmb();
                    g_tag_has = true;
                }
                g_tag = TagTime();
            }
        }
        g_tag_prev = g_tag_cur;
    }
    if (g_frame_no > 1) {
        const float ms = C3D_GetDrawingTime();
        g_gt.sum += ms;
        if (ms > g_gt.max) g_gt.max = ms;
        if (++g_gt.frames == 120) {
            if (!g_gt_has) {
                g_gt_ready = g_gt;
                __dmb();
                g_gt_has = true;
            }
            g_gt = GpuTime();
        }
    }
    { PresentScope timer(P_PACE_WAIT); pace_frame_begun(); }
}

void frame_sync()
{
    // (a frame built ahead was marked built as ahead_flush began waiting)
    if (g_present_active && g_marks.frames < 2 && g_marks.built[g_marks.frames] == 0.0f)
        g_marks.built[g_marks.frames] = marks_ms(svcGetSystemTick());
    const u64 vb0 = svcGetSystemTick();
    {
        PresentScope timer(P_VBLANK_WAIT);
        while (g_vbl[0] == g_late_vbl[0] || g_vbl[1] == g_late_vbl[1]) gspWaitForAnyEvent();
    }
    const u64 pw0 = svcGetSystemTick();
    { PresentScope timer(P_PACE_WAIT); wait_present(); }
    frame_begun(pw0 - vb0, g_late_qw, svcGetSystemTick() - pw0);
}

// The frames built ahead's buffers, made once on first use (or not: then
// every frame waits, as before).
bool ahead_init()
{
    static int state;   // 0 untried, 1 ready, -1 off
    if (state) return state > 0;
    state = -1;
    if (getenv("GAME3DS_AHEAD") && atoi(getenv("GAME3DS_AHEAD")) == 0) {
        printf("[gpu3ds] frames built ahead: off (GAME3DS_AHEAD=0)\n");
        return false;
    }
    for (u32 *&b : g_cmd) b = (u32 *)linearAlloc(kCmdWords * 4);
    if (!g_cmd[0] || !g_cmd[1] || g_rb_n < 2) {
        for (u32 *&b : g_cmd) { if (b) linearFree(b); b = nullptr; }
        printf("[gpu3ds] frames built ahead: off (no memory for its buffers; linear free %lu)\n",
               (unsigned long)linearSpaceFree());
        return false;
    }
    g_hold.entries = g_hold_e;
    g_hold.maxEntries = kHoldMax;
    g_xfer.entries = g_xfer_e;
    g_xfer.maxEntries = kXferMax;
    g_c3d_queue_cb = __C3D_Context.callback;
    __C3D_Context.callback = queue_end_hook;
    state = 1;
    g_ahead_ok = true;
    printf("[gpu3ds] frames built ahead: on (1-vblank frames; linear free %lu)\n",
           (unsigned long)linearSpaceFree());
    return true;
}

u32 current_thread_id()
{
    u32 id = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return id;
}

void queue_reset(gxCmdQueue_s &q)
{
    q.numEntries = q.curEntry = q.lastEntry = 0;
}
}  // namespace

bool ahead_ready()
{
    return g_vblanks_override == 1 && !vblank_first_env() && g_vbl_cb[0] && g_vbl_cb[1] && ahead_init();
}

void ahead_flush()
{
    if (!g_ahead) return;
    g_ahead = false;
    gxCmdQueue_s *const q = &__C3D_Context;
    C3D_FrameSplit(0);   // the frame's list so far, held with the rest
    GX_BindQueue(q);
    if (g_present_active && g_marks.frames < 2) g_marks.built[g_marks.frames] = marks_ms(svcGetSystemTick());
    // Wait for the last frame's queue to finish (citro3d swaps its screens there)
    const u64 w0 = svcGetSystemTick();
    gxCmdQueueWait(q, -1);
    const u64 waited = svcGetSystemTick() - w0;
    g_late_qw = waited;
    ++g_gt.ahead;
    g_gt.ahead_wait += ticks_ms(waited);
    gxCmdQueueStop(q);
    gxCmdQueueClear(q);
    {   // what the build wrote for the GPU: the list (the vertices, the record,
        // the textures and the transfers' sources were flushed as written)
        PresentScope timer(P_VBO_FLUSH);
        if (g_used > g_vlo) GSPGPU_FlushDataCache(g_vbo + g_vlo, (g_used - g_vlo) * sizeof(Vtx));
        if (g_cmd_prev && gpuCmdBuf > g_cmd_prev)
            GSPGPU_FlushDataCache(g_cmd_prev, (size_t)(gpuCmdBuf - g_cmd_prev) * 4);
    }
    // Run the overlay transfers first, to the end: their sources are the game
    // thread's to write again once this returns
    if (g_xfer.numEntries) {
        for (u16 i = 0; i < g_xfer.numEntries; ++i) gxCmdQueueAdd(q, &g_xfer_e[i]);
        gxCmdQueueRun(q);
        gxCmdQueueWait(q, -1);
        gxCmdQueueStop(q);
        gxCmdQueueClear(q);
    }
    queue_reset(g_xfer);
    // then the frame, which the GPU starts on now (in pieces citro3d's queue
    // of 32 takes, the display transfers still to come at frame_end)
    const u16 n = g_hold.numEntries;
    for (u16 i = 0; i < n;) {
        const u16 k = (u16)(n - i < 24 ? n - i : 24);
        if (i) {
            gxCmdQueueWait(q, -1);
            gxCmdQueueStop(q);
            gxCmdQueueClear(q);
        }
        for (u16 j = 0; j < k; ++j) gxCmdQueueAdd(q, &g_hold_e[i + j]);
        gxCmdQueueRun(q);
        i = (u16)(i + k);
    }
    queue_reset(g_hold);
    g_ahead_t_run = svcGetSystemTick();
}

namespace {
// Something about to write what the GPU may still read (a CPU texture, a
// table): the frame built ahead is handed over first, which waits for the last.
void ahead_settle()
{
    if (!g_ahead) return;
    ++g_gt.ahead_early;
    ahead_flush();
}
}  // namespace

void frame_begin(uint32_t clear_argb, Screen s, bool scene)
{
    // the last frame's part of g_vbo (built ahead, its GPU work may still read it)
    g_vprev_lo = g_vlo;
    g_vprev_hi = g_used;
    // 1-vblank frames build before their vblank, but only while citro3d's vblank
    // counters are hooked. They also build ahead of the GPU only while textures
    // are unchanged: a new texture generation deletes them, and only a frame that
    // waits can upload.
    static const bool notexcache = getenv("GPU3DS_NOTEXCACHE") != nullptr;
    const bool late = g_vblanks_override == 1 && !vblank_first_env() && g_vbl_cb[0] && g_vbl_cb[1];
    bool ahead = late && !notexcache && ntr::gx_texture_generation() == g_tex_gen && ahead_init();
    size_t vlo = 0, vhi = kMaxVerts;   // a frame that waits has all of g_vbo
    if (ahead) {
        if (g_vprev_hi <= kMaxVerts / 2) vlo = kMaxVerts / 2;
        else if (g_vprev_lo >= kMaxVerts / 2) vhi = kMaxVerts / 2;
        else ahead = false;   // the last frame took both halves
    }
    textures_check_generation();
    // GPU3DS_NOTEXCACHE=1: upload every texture every frame (an A/B for the
    // texture cache's key)
    if (notexcache) textures_invalidate();
    memset(&g_stats, 0, sizeof g_stats);
    g_eye = 0;
    if (g_stereo) g_slider = stereo_slider();
    g_vlo = g_used = vlo;
    g_vhi = vhi;
    g_rec_drawn = false;
    if (g_keep_overlays) {   // frame_keep_overlays: the last frame's uploads stay
        g_keep_overlays = false;
    } else {
        g_ov_used = 0;
        g_mask_w = g_mask_h = 0;
    }
    ++g_frame_no;
    g_was_ahead = false;
    g_frame_late = late;
    if (g_frame_late) {
        g_late_vbl[0] = g_vbl[0];
        g_late_vbl[1] = g_vbl[1];
        const u64 qw0 = g_present.ticks[P_GPU_QUEUE_WAIT];
        if (ahead) {
            // no wait (the queue wrappers below), the queue entries held, the
            // list in the buffer the last frame did not use
            u32 *const buf = g_cmd[0] != g_cmd_prev ? g_cmd[0] : g_cmd[1];
            g_ahead_tid = current_thread_id();
            queue_reset(g_hold);
            queue_reset(g_xfer);
            GX_BindQueue(&g_hold);
            g_ahead_begin = true;
            {
                PresentScope timer(P_BEGIN_CPU);
                C3D_FrameBegin(0);
            }
            g_ahead_begin = false;
            g_ahead = g_was_ahead = true;
            GPUCMD_SetBuffer(buf, kCmdWords, 0);
            g_cmd_prev = buf;
        } else {
            PresentScope timer(P_BEGIN_CPU);
            C3D_FrameBegin(0);   // the last frame's GPU work only
            g_cmd_prev = nullptr;   // citro3d's own buffer
        }
        g_late_qw = g_present.ticks[P_GPU_QUEUE_WAIT] - qw0;
        frame_target(s, clear_argb, scene);
        return;
    }
    g_cmd_prev = nullptr;
    { PresentScope timer(P_PACE_WAIT); pace_frame(); }
    const u64 vb0 = g_present.ticks[P_VBLANK_WAIT], qw0 = g_present.ticks[P_GPU_QUEUE_WAIT];
    {
        PresentScope timer(P_BEGIN_CPU);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    }
    const u64 pw0 = svcGetSystemTick();
    { PresentScope timer(P_PACE_WAIT); wait_present(); }
    frame_begun(g_present.ticks[P_VBLANK_WAIT] - vb0, g_present.ticks[P_GPU_QUEUE_WAIT] - qw0,
                svcGetSystemTick() - pw0);
    frame_target(s, clear_argb, scene);
}

void frame_target(Screen s, uint32_t clear_argb, bool scene)
{
    if (g_ss_active) resolve();
    PresentScope timer(P_TARGETS);
    g_screen_key[s] = 0;   // drawn again: unknown until screen_mark
    const bool eyes = g_stereo && s == SCREEN_TOP;
    g_ss_active = scene && s == SCREEN_TOP && fsaa_on() && !eyes;
    g_tgt_kx = g_ss_active ? 2 : 1;
    g_tgt_ky = g_ss_active ? g_ss_ky : 1;
    C3D_RenderTarget *t = g_ss_active ? g_top_ss : eyes && g_eye == 1 ? g_top_r : g_top;
    // Shift for this eye: left eye's picture to the left, right eye's to the
    // right, e pixels apart at infinity. The right eye draws the same record again.
    stereo_eye_vec(eyes ? (g_eye == 1 ? 1.0f : -1.0f) * g_slider : 0.0f);
    if (eyes && g_eye == 1) g_rec_drawn = false;
    const C3D_Mtx *proj = &g_proj;
    if (s == SCREEN_BOTTOM) {
        if (!g_bot) {
            // The display transfer writes RGB8; the console left RGB565 here.
            gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
            g_bot = C3D_RenderTargetCreate(kTopH, kBotW, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
            if (!g_bot) return;
            C3D_RenderTargetSetOutput(g_bot, GFX_BOTTOM, GFX_LEFT, kDisplayTransferFlags);
            Mtx_OrthoTilt(&g_proj_bot, 0.0f, (float)kBotW, (float)kTopH, 0.0f, 0.0f, 1.0f, false);
        }
        t = g_bot;
        proj = &g_proj_bot;
    }
    g_tgt_w = s == SCREEN_BOTTOM ? kBotW : kTopW;
    // Alpha 0: nothing drawn yet (pass_state: destination alpha is coverage).
    const u32 rgba = ((clear_argb >> 16 & 0xFF) << 24) | ((clear_argb >> 8 & 0xFF) << 16) |
                     ((clear_argb & 0xFF) << 8);
    // Depth clears to the far value, 0 (depth is stored reversed, see pass_state).
    C3D_RenderTargetClear(t, C3D_CLEAR_ALL, rgba, 0);
    C3D_FrameDrawOn(t);
    if (g_ss_active) C3D_SetViewport(0, 0, g_ss_w, g_ss_h);   // not the texture's full width
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_loc_proj, proj);
    g_cur_proj = proj;
}

bool gpu_time_line(char *out, size_t n)
{
    if (!g_gt_has) return false;
    __dmb();
    const GpuTime t = g_gt_ready;
    __dmb();
    g_gt_has = false;
    unsigned shown = t.vbl[0] + t.vbl[1] + t.vbl[2] + t.vbl[3];
    unsigned span = t.vbl[0] + 2 * t.vbl[1] + 3 * t.vbl[2] + 4 * t.vbl[3];
    static const bool verify = getenv("GPU3DS_OVERLAY_VERIFY") != nullptr;
    const int w = snprintf(out, n,
             "[gpu] drawing per frame: %.2f ms avg, %.2f max (%u frames, FSAA %s); shown %.1f fps, "
             "vblanks between frames 1:%u 2:%u 3:%u 4+:%u; waited for the last present %u; "
             "2D pictures kept %u, sent %u; other screen held %u, drawn %u; "
             "masked fades in one pass %u, in three %u; built ahead %u (handed over early %u), "
             "waiting %.2f ms for the frame before, their GPU work %.2f ms (%u still on the GPU at "
             "their vblank)",
             t.frames ? t.sum / (float)t.frames : 0.0f, t.max, t.frames, fsaa_on() ? "on" : "off",
             span ? 59.831 * (double)shown / (double)span : 0.0, t.vbl[0], t.vbl[1], t.vbl[2],
             t.vbl[3], t.waits, t.ov_kept, t.ov_sent, t.bot_held, t.bot_drawn, t.fade_one,
             t.fade_three, t.ahead, t.ahead_early, t.ahead ? t.ahead_wait / t.ahead : 0.0,
             t.ahead_gpu_n ? t.ahead_gpu / t.ahead_gpu_n : 0.0, t.ahead_gpu_late);
    if (verify && w > 0 && (size_t)w < n)
        snprintf(out + w, n - (size_t)w, " (verify: %u kept by the maker's number differ)", t.ov_bad);
    return true;
}

void set_frame_tag(int tag, const char *name)
{
    if (tag < 0 || tag > 7) return;
    g_tag_on = true;
    g_tag_cur = tag;
    g_tag_name[tag] = name;
}

bool gpu_tag_line(char *out, size_t n)
{
    if (!g_tag_has) return false;
    __dmb();
    const TagTime t = g_tag_ready;
    __dmb();
    g_tag_has = false;
    int w = snprintf(out, n, "[gpu-pass] GPU ms a frame, by the pass left out:");
    for (int i = 0; i < 8 && w > 0 && (size_t)w < n; ++i)
        if (t.n[i])
            w += snprintf(out + w, n - (size_t)w, " %s %.2f (%u)", g_tag_name[i] ? g_tag_name[i] : "?",
                          t.sum[i] / t.n[i], t.n[i]);
    return w > 0;
}

void frame_keep_overlays()
{
    g_keep_overlays = true;
}

int present_zone()
{
    return g_present_zone;
}

bool fsaa_on()
{
    return g_top_ss != nullptr && g_ss_want;
}

bool set_fsaa(bool on)
{
    if (on && !g_top_ss) ss_create(1);
    g_ss_want = on && g_top_ss;
    return g_ss_want;
}

bool set_stereo(bool on)
{
    if (on && !g_top_r) {
        g_top_r = C3D_RenderTargetCreate(kTopH, kTopW, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        if (g_top_r) C3D_RenderTargetSetOutput(g_top_r, GFX_TOP, GFX_RIGHT, kDisplayTransferFlags);
        printf("[gpu3ds] 3D: the right eye's target %s (vram free %lu)\n",
               g_top_r ? "made" : "FAILED", (unsigned long)vramSpaceFree());
    }
    g_stereo = on && g_top_r && g_rloc_eye >= 0;
    gfxSet3D(g_stereo);
    g_slider = g_stereo ? stereo_slider() : 0.0f;
    return g_stereo;
}

bool stereo_on()
{
    return g_stereo;
}

void set_eye(int eye)
{
    g_eye = eye;
}

// GAME3DS_STEREO_SLIDER=x stands in for the slider; =x,y@s is x for s seconds
// from the first read, then y (a test of the slider moving).
float stereo_slider()
{
    static const char *const e = getenv("GAME3DS_STEREO_SLIDER");
    float k;
    if (!e) {
        k = osGet3DSliderState();
    } else {
        static u64 t0;
        if (!t0) t0 = svcGetSystemTick();
        float x = 0.0f, y = 0.0f, secs = 0.0f;
        const int n = sscanf(e, "%f,%f@%f", &x, &y, &secs);
        k = n == 3 && (float)(svcGetSystemTick() - t0) >= secs * (float)SYSCLOCK_ARM11 ? y : x;
    }
    return k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
}

namespace {
void overlay_draw_modes(size_t first, OverlayMode mode, bool permuted);
}  // namespace

void resolve()
{
    if (!g_ss_active) return;
    PresentScope timer(P_TARGETS);
    g_ss_active = false;
    g_tgt_kx = g_tgt_ky = 1;
    C3D_FrameDrawOn(g_top);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_loc_proj, &g_proj);
    g_cur_proj = &g_proj;
    if (g_used + 6 > g_vhi) return;
    // Each screen corner's texel on g_top_ss's 240 (480) x 800 viewport over the
    // 256 (512) x 1024 texture. The target uses the texture's last 800 rows, so t
    // is offset by the 224 unused rows (without it the picture sat 112 px too high).
    const float xs[6] = {0, (float)kTopW, 0, (float)kTopW, (float)kTopW, 0};
    const float ys[6] = {0, 0, (float)kTopH, 0, (float)kTopH, (float)kTopH};
    const size_t first = g_used;
    for (int i = 0; i < 6; ++i) {
        const C3D_FVec *r = g_proj.r;
        const float cw = r[3].x * xs[i] + r[3].y * ys[i] + r[3].w;
        const float cx = (r[0].x * xs[i] + r[0].y * ys[i] + r[0].w) / cw;
        const float cy = (r[1].x * xs[i] + r[1].y * ys[i] + r[1].w) / cw;
        const float t = ((cy + 1.0f) * 0.5f * (float)g_ss_h + (float)(1024 - g_ss_h)) / 1024.0f;
        Vtx &o = g_vbo[g_used++];
        o.x = xs[i]; o.y = ys[i]; o.z = 0.0f; o.w = 1.0f;
        o.s = (cx + 1.0f) * 0.5f * (float)g_ss_w / (float)g_ss_tex_w;
        o.t = t;
        o.r = o.g = o.b = o.a = 1.0f;
        o.fs = 0.0f; o.ft = 0.5f;
    }
    C3D_TexBind(0, &g_ss_tex);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    fog_stage(false);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    // colour and coverage alpha as they are (OVERLAY_OPAQUE's state)
    overlay_draw_modes(first, OVERLAY_OPAQUE, false);
}

// GPU3DS_TRACE=<n>: every draw run of the n-th presented frame on stderr, with
// what decided how it was drawn (a rendering question answered from the log).
bool trace_this_frame()
{
    static int want = -2;
    if (want == -2) {
        const char *e = getenv("GPU3DS_TRACE");
        want = e && *e ? atoi(e) : -1;
    }
    return want >= 0 && g_frame_no == want;
}

namespace {
size_t push_quad(float x0, float y0, float x1, float y1, float s1, float t1,
                 float r, float g, float b, float a);
void overlay_draw_modes(size_t first, OverlayMode mode, bool permuted);
}  // namespace

// Shadow volumes (POLYGON_ATTR mode 3), using stencil bit 7 like gx.cpp.
// The game draws each shadow as a closed volume twice:
//   ID 0 (mask, back faces): set bit 7 where the depth test fails. No colour or
//     depth writes.
//   ID n (draw, front faces): where bit 7 is set, clear it, and if depth passes
//     and the pixel isn't the caster's own ID, blend the shadow colour.
// One stencil compare can't test both "bit 7 set" and "ID differs", so the
// draw takes three passes: (a) clear bit 7 on the caster's own pixels,
// (b) blend where bit 7 is still set and depth passes, (c) clear bit 7
// everywhere the polygon covers. Translucent pixels never match an ID, so
// shadows fall on them. `issue` makes one GPU draw of the run.
template <typename F>
static void draw_shadow_with(const ntr::GxTriangle &t, bool textured, F issue)
{
    const int id = t.polyid & 0x3F;
    fog_stage(false);
    if (id == 0) {
        env_textured(false);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        // The DS test is a strict LESS; failing it (depth not nearer) is
        // GREATER failing here (nearer is larger).
        C3D_DepthTest(true, GPU_GREATER, (GPU_WRITEMASK)0);
        C3D_StencilTest(true, GPU_ALWAYS, 0x80, 0xFF, 0x80);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE, GPU_STENCIL_KEEP);
        issue();
        ++g_stats.draw_calls;
    } else {
        env_textured(false);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_DepthTest(true, GPU_ALWAYS, (GPU_WRITEMASK)0);
        C3D_StencilTest(true, GPU_EQUAL, 0x80 | id, 0xFF, 0x80);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_ZERO);
        issue();                                                       // (a)

        env_textured(textured);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_ZERO, GPU_ONE);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
        C3D_StencilTest(true, GPU_EQUAL, 0x80, 0x80, 0x00);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
        issue();                                                       // (b)

        env_textured(false);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_DepthTest(true, GPU_ALWAYS, (GPU_WRITEMASK)0);
        C3D_StencilTest(true, GPU_ALWAYS, 0, 0xFF, 0x80);
        C3D_StencilOp(GPU_STENCIL_ZERO, GPU_STENCIL_ZERO, GPU_STENCIL_ZERO);
        issue();                                                       // (c)
        g_stats.draw_calls += 3;
    }
    pass_state(true);   // the translucent pass's blend and depth again
}

static void draw_shadow_run(const ntr::GxTriangle &t, bool textured, int first, int n)
{
    draw_shadow_with(t, textured, [&] { C3D_DrawArrays(GPU_TRIANGLES, first, n); });
}

// Every mask bit (stencil bit 7) back to 0 inside the viewport.
static void clear_shadow_mask(const Viewport &vp)
{
    const size_t first = push_quad(vp.x, vp.y, vp.x + vp.w, vp.y + vp.h, 0, 0, 0, 0, 0, 0);
    if (first == (size_t)-1) return;
    env_textured(false);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(true, GPU_ALWAYS, (GPU_WRITEMASK)0);
    C3D_StencilTest(true, GPU_ALWAYS, 0, 0xFF, 0x80);
    C3D_StencilOp(GPU_STENCIL_ZERO, GPU_STENCIL_ZERO, GPU_STENCIL_ZERO);
    C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
    ++g_stats.draw_calls;
    pass_state(true);
}

// GPU3DS_DUMP=1: the last drawn triangle list, with each texture's texels, to
// sdmc:/sm64ds3ds/tris.bin at shutdown (tools/tris_render.py draws it).
static std::vector<uint8_t> g_dump;

static void dump_tris(const ntr::GxTriangle *tris, size_t count, const Viewport &vp)
{
    std::vector<uint8_t> &b = g_dump;
    b.clear();
    auto put = [&](const void *p, size_t n) {
        b.insert(b.end(), (const uint8_t *)p, (const uint8_t *)p + n);
    };
    const uint32_t hdr[4] = {0x53495254u, (uint32_t)count, (uint32_t)sizeof(ntr::GxTriangle),
                             (uint32_t)ntr::active_w};
    put(hdr, sizeof hdr);
    put(&vp, sizeof vp);
    put(tris, count * sizeof *tris);
    std::vector<const uint32_t *> seen;
    for (size_t i = 0; i < count; ++i) {
        const ntr::GxTriangle &t = tris[i];
        if (!t.tex || std::find(seen.begin(), seen.end(), t.tex) != seen.end()) continue;
        seen.push_back(t.tex);
        const uint32_t th[4] = {(uint32_t)(uintptr_t)t.tex, (uint32_t)t.tw, (uint32_t)t.th, 0};
        put(th, sizeof th);
        put(t.tex, (size_t)t.tw * t.th * 4);
    }
}

static void dump_write()
{
    if (g_dump.empty()) return;
    if (FILE *f = fopen("sdmc:/sm64ds3ds/tris.bin", "wb")) {
        fwrite(g_dump.data(), 1, g_dump.size(), f);
        fclose(f);
    }
}

// A colour byte as the vertex buffer's float, (float)i / 255.0f: the same
// values, without three divides a vertex.
static const struct U8f {
    float v[256];
    U8f() { for (int i = 0; i < 256; ++i) v[i] = (float)i / 255.0f; }
} g_u8f;

void draw(const ntr::GxTriangle *tris, size_t count, const Viewport &vp)
{
    PresentScope timer(P_GEOMETRY);
    g_stats.tris_in += (unsigned)count;
    static const bool dump = getenv("GPU3DS_DUMP") != nullptr;
    if (dump) dump_tris(tris, count, vp);
    const bool trace = trace_this_frame();
    if (trace)
        fprintf(stderr, "[gpu3ds-trace] frame %d draw: %u triangles, toon %d\n", g_frame_no,
                (unsigned)count, g_toon_shade);
    const float sx = vp.w / (float)ntr::SCREEN_W, sy = vp.h / (float)ntr::SCREEN_H;
    // The DS clips at its screen edge, but gx.cpp can hand over triangles that
    // reach past it, which would draw into the margins. Scissor to the picture.
    // The target is the screen on its side (x is screen y from the bottom).
    const float sx0 = vp.x > 0.0f ? vp.x : 0.0f;
    const float sx1 = vp.x + vp.w < (float)g_tgt_w ? vp.x + vp.w : (float)g_tgt_w;
    // buffer pixels a screen pixel (FSAA: 2 across, 1 or 2 down)
    const float bkx = (float)g_tgt_kx, bky = (float)g_tgt_ky;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(((float)g_tgt_h - (vp.y + vp.h)) * bky), (u32)(sx0 * bkx),
                   (u32)(((float)g_tgt_h - vp.y) * bky), (u32)(sx1 * bkx));

    bool have_shadow = false;
    for (size_t i = 0; i < count && !have_shadow; ++i) have_shadow = tris[i].mode == 3;

    // The last run's state, so consecutive runs with the same texture, combiner,
    // stencil or fog don't set it again (each call dirties citro3d state). Matters
    // for particles, which come as many short runs. -1 / null means unknown, after
    // a shadow run or mask clear.
    const C3D_Tex *last_tex = nullptr;
    int last_wrap = -1, last_env = -1, last_stencil = -1, last_fog = -1;
    auto forget = [&] {
        last_tex = nullptr;
        last_wrap = last_env = last_stencil = last_fog = -1;
    };

    // Two passes like gx.cpp: all opaque polygons first, then translucent ones and
    // shadow volumes in submission order.
    for (int pass = 0; pass < 2; ++pass) {
        pass_state(pass == 1);
        last_stencil = -1;
        size_t run_start = g_used;
        const ntr::GxTriangle *run_tri = nullptr;
        auto flush = [&](void) {
            if (g_used > run_start && run_tri) {
                PresentScope timer(P_DRAW_SUBMIT);
                const bool textured = run_tri->tex != nullptr;
                C3D_Tex *tex = nullptr;
                if (textured) {
                    PresentScope timer(P_TEXTURES);
                    tex = texture_for(*run_tri);
                }
                if (tex && (tex != last_tex || run_tri->wrap != last_wrap)) {
                    C3D_TexSetWrap(tex, wrap_mode(run_tri->wrap & 1, run_tri->wrap & 4),
                                   wrap_mode(run_tri->wrap & 2, run_tri->wrap & 8));
                    C3D_TexBind(0, tex);
                    last_tex = tex;
                    last_wrap = run_tri->wrap;
                }
                // GPU3DS_NOTOON=1: mode 2 as plain texture x vertex colour (an
                // A/B for the toon stage)
                static const bool notoon = getenv("GPU3DS_NOTOON") != nullptr;
                const bool toon = g_toon_shade && run_tri->mode == 2 && !notoon;
                const int env = (toon ? g_toon_shade << 1 : 0) | (tex != nullptr);
                if (env != last_env) {
                    if (toon)
                        env_toon(tex != nullptr, g_toon_shade);
                    else
                        env_textured(tex != nullptr);
                    last_env = env;
                }
                if (run_tri->mode != 3 && (int)(run_tri->polyid & 0x3F) != last_stencil) {
                    run_stencil(pass == 1, run_tri->polyid);
                    last_stencil = run_tri->polyid & 0x3F;
                }
                const int fog = g_fog_on && run_tri->fog && run_tri->mode != 3;
                if (fog != last_fog) {
                    fog_stage(fog);
                    last_fog = fog;
                }
                if (trace)
                    fprintf(stderr, "[gpu3ds-trace]  pass %d run %3u tris: mode %u transl %u "
                            "alpha %2u tex %dx%d fmt %u %s polyid %2u eq %u cull %u wrap %x "
                            "teximage %08lx\n", pass, (unsigned)((g_used - run_start) / 3),
                            run_tri->mode, run_tri->translucent, run_tri->alpha,
                            textured ? run_tri->tw : 0, textured ? run_tri->th : 0,
                            (unsigned)((run_tri->dbg_tex >> 26) & 7),
                            textured ? (tex ? "found" : "NO TEXTURE") : "untextured",
                            run_tri->polyid, run_tri->depth_eq, run_tri->cull, run_tri->wrap,
                            (unsigned long)run_tri->dbg_tex);
                if (trace && textured)
                    fprintf(stderr, "[gpu3ds-trace]    uv (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f) "
                            "xy (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) tex_scale %u texel0 %08lx\n",
                            run_tri->v[0].u, run_tri->v[0].v, run_tri->v[1].u, run_tri->v[1].v,
                            run_tri->v[2].u, run_tri->v[2].v, run_tri->v[0].x, run_tri->v[0].y,
                            run_tri->v[1].x, run_tri->v[1].y, run_tri->v[2].x, run_tri->v[2].y,
                            run_tri->tex_scale, (unsigned long)run_tri->tex[0]);
                if (run_tri->mode == 3) {
                    draw_shadow_run(*run_tri, tex != nullptr, (int)run_start,
                                    (int)(g_used - run_start));
                    forget();
                } else {
                    C3D_DrawArrays(GPU_TRIANGLES, (int)run_start, (int)(g_used - run_start));
                    ++g_stats.draw_calls;
                }
            }
            run_start = g_used;
        };
        bool prev_mask = false;
        int memo_tw = -1, memo_th = -1;
        float memo_iw = 0.0f, memo_ih = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            const ntr::GxTriangle &t = tris[i];
            if ((t.translucent != 0) != (pass == 1)) continue;
            // A new shadow mask group clears every mask bit first so one volume can't leak
            // into the next (gx.cpp's g_clr_cum, counted the same way).
            if (pass == 1 && have_shadow) {
                const bool is_mask = t.mode == 3 && t.polyid == 0;
                if (is_mask && !prev_mask) {
                    flush();
                    run_tri = nullptr;
                    clear_shadow_mask(vp);
                    forget();
                    run_start = g_used;
                }
                prev_mask = is_mask;
            }
            if (culled(t)) { if (pass == 0) ++g_stats.tris_culled; continue; }
            if (g_used + 3 > g_vhi) break;
            // Split runs on polygon ID wherever the stencil carries it: always in the
            // translucent pass, and in the opaque pass when a shadow will read the ID.
            if (!run_tri || t.tex != run_tri->tex || t.tex_id != run_tri->tex_id ||
                t.tw != run_tri->tw || t.wrap != run_tri->wrap ||
                (t.mode == 2) != (run_tri->mode == 2) || t.mode != run_tri->mode ||
                ((pass == 1 || have_shadow) && t.polyid != run_tri->polyid) ||
                (g_fog_on && t.fog != run_tri->fog)) {
                flush();
                run_tri = &t;
            }
            const float pa = (t.alpha == 0 || t.alpha >= 31) ? 1.0f : t.alpha / 31.0f;
            const bool fog_t = g_fog_on && t.fog && t.mode != 3;
            // the texture's reciprocal size, divided out only when it changes
            if (t.tw != memo_tw) { memo_tw = t.tw; memo_iw = 1.0f / (float)t.tw; }
            if (t.th != memo_th) { memo_th = t.th; memo_ih = 1.0f / (float)t.th; }
            const float inv_w = t.tex ? memo_iw : 0.0f;
            const float inv_h = t.tex ? memo_ih : 0.0f;
            for (int k = 0; k < 3; ++k) {
                const ntr::GxVertex &v = t.v[k];
                Vtx &o = g_vbo[g_used++];
                o.x = vp.x + v.x * sx;
                o.y = vp.y + v.y * sy;
                // The DS already clipped to its frustum, so z is in [0,1]. Clamp instead of
                // letting the GPU clip a vertex the DS would draw (and count it).
                float z = v.z;
                if (z < 0.0f || z > 1.0f) { ++g_stats.z_clamped; z = z < 0.0f ? 0.0f : 1.0f; }
                o.z = -z;
                o.w = v.w > 1e-6f ? v.w : 1.0f;
                // gx.cpp's u, v are texels with the top row first. Textures are uploaded
                // bottom row first, so t = 0 is the image's top row.
                o.s = v.u * inv_w;
                o.t = v.v * inv_h;
                o.r = g_u8f.v[(v.color >> 16) & 0xFF];
                o.g = g_u8f.v[(v.color >> 8) & 0xFF];
                o.b = g_u8f.v[v.color & 0xFF];
                if (g_toon_shade == 2 && t.mode == 2) o.g = o.b = o.r;   // highlight
                o.a = pa;
                o.fs = fog_t ? fog_coord(v) : 0.0f;
                o.ft = 0.5f;
            }
            ++g_stats.tris_drawn;
        }
        flush();
    }
    fog_stage(false);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

namespace {

// Both programs use `projection` (c0-c3), so it's set again on every switch,
// and draw_record's ring is reloaded after one (a program's constants can land
// on the other's registers).
void use_main_program()
{
    C3D_BindProgram(&g_prog);
    C3D_SetAttrInfo(&g_attr_main);
    C3D_SetBufInfo(&g_buf_main);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_loc_proj, g_cur_proj);
}

void use_rec_program()
{
    C3D_BindProgram(&g_rec_prog);
    C3D_SetAttrInfo(&g_attr_rec);
    C3D_SetBufInfo(&g_buf_rec);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_rloc_proj, g_cur_proj);
    if (g_rloc_eye >= 0)
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_rloc_eye, g_eye_vec[0], g_eye_vec[1], g_eye_vec[2],
                      g_eye_vec[3]);
}

// POLYGON_ATTR bits 6-7 as the GPU cull mode. A DS front face (negative area,
// y down) is counter-clockwise on the tilted target. GPU3DS_REC_CULLFLIP=1
// flips it.
GPU_CULLMODE cull_for(uint8_t cull)
{
    static const bool flip = getenv("GPU3DS_REC_CULLFLIP") != nullptr;
    if (cull == 3) return GPU_CULL_NONE;
    const bool front_only = cull == 2;   // back faces culled
    return front_only != flip ? GPU_CULL_BACK_CCW : GPU_CULL_FRONT_CCW;
}

}  // namespace

// GPU3DS_REC_PROF=N: draw_record's time per part every 120 frames, published
// by core 2 and printed by the game thread (rec_prof_line).
struct RecProf {
    enum { CONVERT, SWITCH, STATE, UNIF, TRIS, RING, DRAW, FLUSH, N };
    unsigned long long t[N];
    unsigned frames, runs, draws, uploads, verts, tris;
    unsigned reused;   // frames whose record was in g_rblock already
    // why an open draw was closed before the next run joined it
    enum { W_GAP, W_RING, W_TEX, W_ENV, W_ID, W_FOG, W_CULL, W_ALPHA, W_SCALE, W_SHADOW, W_N };
    unsigned why[W_N];
};

void record_upload_forget()
{
    g_rb_valid = false;
}
RecProf g_rp, g_rp_ready;
volatile bool g_rp_has;
const char *const kRecProfNames[RecProf::N] = {"convert", "switch", "state", "uniforms",
                                               "indices", "ring", "draw", "flush"};
const char *const kRecWhyNames[RecProf::W_N] = {"gap", "ring", "texture", "combiner", "polyid", "fog",
                                                "cull", "alpha", "texscale", "shadow"};

bool record_fits(size_t verts, size_t tris)
{
    return g_rec_up && verts <= kRecVerts && tris * 3 <= kRecIdx;
}

// GPU3DS_REC_DUMP=N: dump N frames' records (one every
// GPU3DS_REC_DUMP_EVERY, default 60) to sdmc:/sm64ds3ds/recdump/ for
// tools/f24_check.py, which replays the shader in 24-bit floats.
void rec_dump(const ntr::GxRecord &rec, const Viewport &vp)
{
    static int left = getenv("GPU3DS_REC_DUMP") ? atoi(getenv("GPU3DS_REC_DUMP")) : 0;
    static const int every =
        getenv("GPU3DS_REC_DUMP_EVERY") ? atoi(getenv("GPU3DS_REC_DUMP_EVERY")) : 60;
    static int frame, written;
    if (left <= 0 || every <= 0 || frame++ % every) return;
    --left;
    mkdir("sdmc:/sm64ds3ds/recdump", 0777);
    char path[96];
    const char *level = getenv("SM64DS_LEVEL");
    snprintf(path, sizeof path, "sdmc:/sm64ds3ds/recdump/L%s_%04d.bin", level ? level : "x", written++);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const uint32_t head[8] = {0x504D4452u, 1, (uint32_t)rec.v.size(), (uint32_t)rec.idx.size(),
                              (uint32_t)rec.runs.size(), (uint32_t)rec.mtx.size(),
                              (uint32_t)g_tgt_w, (uint32_t)g_tgt_h};
    fwrite(head, sizeof head, 1, f);
    const float vpf[4] = {vp.x, vp.y, vp.w, vp.h};
    fwrite(vpf, sizeof vpf, 1, f);
    float pm[16];
    for (int r = 0; r < 4; ++r) {
        pm[r * 4 + 0] = g_cur_proj->r[r].x;
        pm[r * 4 + 1] = g_cur_proj->r[r].y;
        pm[r * 4 + 2] = g_cur_proj->r[r].z;
        pm[r * 4 + 3] = g_cur_proj->r[r].w;
    }
    fwrite(pm, sizeof pm, 1, f);
    fwrite(rec.v.data(), sizeof(ntr::GxRecVertex), rec.v.size(), f);
    fwrite(rec.idx.data(), sizeof(uint16_t), rec.idx.size(), f);
    for (const ntr::GxRecRun &r : rec.runs) {
        struct {
            uint32_t first, count;
            uint16_t lo, hi;
            uint8_t translucent, cull, mode, polyid, depth_eq, pad[3];
        } o = {r.first, r.count, r.id_lo, r.id_hi, r.st.translucent, r.st.cull, r.st.mode,
               r.st.polyid, r.st.depth_eq, {0, 0, 0}};
        fwrite(&o, sizeof o, 1, f);
    }
    for (const ntr::GxRecMatrix &m : rec.mtx) {
        const uint32_t persp = m.persp;
        fwrite(m.m, sizeof m.m, 1, f);
        fwrite(&persp, sizeof persp, 1, f);
    }
    fclose(f);
}

// GAME3DS_STEREO_WLOG=1: log the w of perspective vertices (10/50/90th
// percentile) every 60th record, to help pick GAME3DS_STEREO_DEPTH.
static void stereo_wlog(const ntr::GxRecord &rec, const ntr::GxRecMatrix *mtx,
                        const ntr::GxRecVertex *v, size_t nv)
{
    static const bool on = getenv("GAME3DS_STEREO_WLOG") != nullptr;
    static unsigned n;
    if (!on || n++ % 60) return;
    static std::vector<float> ws;
    ws.clear();
    for (size_t i = 0; i < nv; ++i) {
        const unsigned id = v[i].id & 0x7FFFu;
        if (!(v[i].id & 0x8000u) || id >= rec.mtx.size()) continue;
        const float *m = mtx ? mtx[id].m : rec.mtx[id].m;
        ws.push_back(m[3] * v[i].x + m[7] * v[i].y + m[11] * v[i].z + m[15]);
    }
    if (ws.empty()) return;
    std::sort(ws.begin(), ws.end());
    const size_t k = ws.size();
    printf("[stereo] w of %u perspective vertices: min %.4f, 10%% %.4f, 50%% %.4f, 90%% %.4f, max %.4f\n",
           (unsigned)k, ws[0], ws[k / 10], ws[k / 2], ws[k * 9 / 10], ws[k - 1]);
}

bool draw_record(const ntr::GxRecord &rec, const Viewport &vp, const ntr::GxRecMatrix *mtx,
                 const ntr::GxRecVertex *verts)
{
    const size_t nv = rec.v.size(), ni = rec.idx.size();
    if (!g_rec_up || g_rec_drawn || nv > kRecVerts || ni > kRecIdx || rec.mtx.size() >= 0x8000)
        return false;
    g_rec_drawn = true;
    rec_dump(rec, vp);
    PresentScope timer(P_GEOMETRY);
    // GPU3DS_REC_PROF=N: one frame in N timed (1: every one; the clock reads
    // cost the worker ~0.3 ms a timed frame)
    static const int prof_every = getenv("GPU3DS_REC_PROF") ? atoi(getenv("GPU3DS_REC_PROF")) : 0;
    static unsigned prof_n;
    const bool prof = prof_every > 0 && prof_n++ % (unsigned)prof_every == 0;
    u64 tp = prof ? svcGetSystemTick() : 0;
    auto lap = [&](int part) {
        if (!prof) return;
        const u64 now = svcGetSystemTick();
        g_rp.t[part] += now - tp;
        tp = now;
    };
    g_stats.tris_in += (unsigned)(ni / 3);
    // Upload the record as-is, indices then vertices in one block, one flush. The
    // tick frame after an in-between reuses the same upload unless the in-between
    // had its own vertices. New uploads go in the other block, since the previous
    // frame may still be on the GPU.
    const ntr::GxRecVertex *const src_v = verts ? verts : rec.v.data();
    if (g_eye == 0) stereo_wlog(rec, mtx, src_v, nv);
    const bool uploaded = g_rb_valid && g_rb_idx == rec.idx.data() && g_rb_v == src_v &&
                          g_rb_ni == ni && g_rb_nv == nv;
    if (!uploaded) g_rb_cur = (g_rb_cur + 1) % g_rb_n;
    uint8_t *const block = g_rblock + (size_t)g_rb_cur * kRBlock;
    uint16_t *const ribo = (uint16_t *)block;
    uint8_t *const rvbo = block + ((ni * sizeof(uint16_t) + 63) & ~(size_t)63);
    if (!uploaded) {
        memcpy(ribo, rec.idx.data(), ni * sizeof(uint16_t));
        memcpy(rvbo, src_v, nv * sizeof(ntr::GxRecVertex));
    }
    g_rb_valid = true;
    g_rb_idx = rec.idx.data();
    g_rb_v = src_v;
    g_rb_ni = ni;
    g_rb_nv = nv;
    lap(RecProf::CONVERT);
    const float sx = vp.w / (float)ntr::SCREEN_W, sy = vp.h / (float)ntr::SCREEN_H;
    // draw()'s scissor: the picture's rectangle on the tilted target
    const float sx0 = vp.x > 0.0f ? vp.x : 0.0f;
    const float sx1 = vp.x + vp.w < (float)g_tgt_w ? vp.x + vp.w : (float)g_tgt_w;
    // buffer pixels a screen pixel (FSAA: 2 across, 1 or 2 down)
    const float bkx = (float)g_tgt_kx, bky = (float)g_tgt_ky;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(((float)g_tgt_h - (vp.y + vp.h)) * bky), (u32)(sx0 * bkx),
                   (u32)(((float)g_tgt_h - vp.y) * bky), (u32)(sx1 * bkx));
    bool have_shadow = false;
    for (const ntr::GxRecRun &r : rec.runs) have_shadow = have_shadow || r.st.mode == 3;

    BufInfo_Init(&g_buf_rec);
    BufInfo_Add(&g_buf_rec, rvbo, sizeof(ntr::GxRecVertex), 3, 0x210);
    use_rec_program();
    // the uniforms a run sets, written only when they change (-1: unknown)
    float u_iw = -1.0f, u_ih = -1.0f, u_pa = -1.0f;
    int u_hl = -1, u_fog = -1;
    auto uniforms_forget = [&] {
        u_hl = u_fog = -1;
        u_iw = u_ih = u_pa = -1.0f;
    };
    lap(RecProf::SWITCH);
    // The matrix ring: id n lives in slot n mod kRing. Each matrix has draw()'s
    // viewport scale and offset folded in, stored as columns for the shader's four
    // dot products.
    int owner[kRing];
    auto ring_forget = [&] { for (int &o : owner) o = -1; };
    ring_forget();
    auto upload = [&](unsigned id) {
        const int slot = (int)(id % kRing);
        if (owner[slot] == (int)id) return;
        owner[slot] = (int)id;
        const float *m = mtx ? mtx[id].m : rec.mtx[id].m;
        C3D_FVec *row = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g_rloc_ring + slot * 4, 4);
        row[0].x = m[0] * sx + m[3] * vp.x;
        row[0].y = m[4] * sx + m[7] * vp.x;
        row[0].z = m[8] * sx + m[11] * vp.x;
        row[0].w = m[12] * sx + m[15] * vp.x;
        row[1].x = m[1] * sy + m[3] * vp.y;
        row[1].y = m[5] * sy + m[7] * vp.y;
        row[1].z = m[9] * sy + m[11] * vp.y;
        row[1].w = m[13] * sy + m[15] * vp.y;
        row[2].x = m[2];
        row[2].y = m[6];
        row[2].z = m[10];
        row[2].w = m[14];
        row[3].x = m[3];
        row[3].y = m[7];
        row[3].z = m[11];
        row[3].w = m[15];
        ++g_rp.uploads;
    };

    // The open draw: record triangles [start, start + n) using ids lo..hi. The next
    // run joins it if it follows on and needs no state change; otherwise the draw
    // is closed first.
    uint32_t start = 0, n = 0;
    unsigned lo = 0, hi = 0;
    uint64_t open_ids = 0;   // the polygon IDs of the open draw's translucent runs
    static const bool merge_ids = getenv("GPU3DS_MERGE_IDS_OFF") == nullptr;
    const ntr::GxTriangle *shadow_st = nullptr;
    const C3D_Tex *shadow_tex = nullptr;
    auto issue = [&] {
        if (!n) return;
        lap(RecProf::TRIS);
        for (unsigned id = lo; id <= hi; ++id) upload(id);
        lap(RecProf::RING);
        ++g_rp.draws;
        const void *ix = &ribo[(size_t)start * 3];
        const int count = (int)(n * 3);
        if (shadow_st) {
            draw_shadow_with(*shadow_st, shadow_tex != nullptr, [&] {
                C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, ix);
            });
        } else {
            C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, ix);
            ++g_stats.draw_calls;
        }
        g_stats.tris_drawn += n;
        lap(RecProf::DRAW);
        n = 0;
    };
    // add triangles [first, first + count) spanning ids lo_..hi_ (< kRing)
    auto add = [&](uint32_t first, uint32_t count, unsigned lo_, unsigned hi_) {
        if (n && (first != start + n ||
                  (hi_ > hi ? hi_ : hi) - (lo_ < lo ? lo_ : lo) >= (unsigned)kRing)) {
            ++g_rp.why[first != start + n ? RecProf::W_GAP : RecProf::W_RING];
            issue();
        }
        if (!n) {
            start = first;
            lo = lo_;
            hi = hi_;
        } else {
            if (lo_ < lo) lo = lo_;
            if (hi_ > hi) hi = hi_;
        }
        n += count;
    };

    const C3D_Tex *last_tex = nullptr;
    int last_wrap = -1, last_env = -1, last_stencil = -1, last_fog = -1, last_cull = -1;
    auto forget = [&] {
        last_tex = nullptr;
        last_wrap = last_env = last_stencil = last_fog = last_cull = -1;
    };
    // texture_for's answer for the last run's texture
    const uint32_t *memo_px = nullptr;
    uint32_t memo_id = 0;
    int memo_w = -1, memo_h = -1;
    C3D_Tex *memo_tex = nullptr;
    static const bool notoon = getenv("GPU3DS_NOTOON") != nullptr;
    g_fog_by_depth = true;
    for (int pass = 0; pass < 2; ++pass) {
        issue();
        pass_state(pass == 1);
        last_stencil = -1;
        open_ids = 0;
        bool prev_mask = false;
        // draw() splits runs on the polygon ID only where the stencil carries
        // it: the translucent pass, and the opaque one when a shadow reads it
        const bool by_id = pass == 1 || have_shadow;
        for (const ntr::GxRecRun &run : rec.runs) {
            const ntr::GxTriangle &st = run.st;
            if ((st.translucent != 0) != (pass == 1)) continue;
            // draw()'s mask groups, run by run (the state is the run's)
            if (pass == 1 && have_shadow) {
                const bool is_mask = st.mode == 3 && st.polyid == 0;
                if (is_mask && !prev_mask && run.count) {
                    issue();
                    use_main_program();
                    clear_shadow_mask(vp);
                    use_rec_program();
                    uniforms_forget();
                    ring_forget();
                    forget();
                }
                if (run.count) prev_mask = is_mask;
            }
            if (!run.count) continue;
            if (st.cull == 0) {
                if (pass == 0) g_stats.tris_culled += run.count;
                continue;
            }
            ++g_rp.runs;
            const bool textured = st.tex != nullptr;
            C3D_Tex *tex = nullptr;
            if (textured) {
                if (st.tex != memo_px || st.tex_id != memo_id || st.tw != memo_w || st.th != memo_h) {
                    PresentScope tt(P_TEXTURES);
                    memo_tex = texture_for(st);
                    memo_px = st.tex;
                    memo_id = st.tex_id;
                    memo_w = st.tw;
                    memo_h = st.th;
                }
                tex = memo_tex;
            }
            const bool toon = g_toon_shade && st.mode == 2 && !notoon;
            const int env = (toon ? g_toon_shade << 1 : 0) | (tex != nullptr);
            const int fog = g_fog_on && st.fog && st.mode != 3;
            const bool shadow = st.mode == 3;
            const int hl = g_toon_shade == 2 && st.mode == 2;
            const float pa = (st.alpha == 0 || st.alpha >= 31) ? 1.0f : st.alpha / 31.0f;
            const float iw = textured && st.tw ? 1.0f / (float)st.tw : 0.0f;
            const float ih = textured && st.th ? 1.0f / (float)st.th : 0.0f;
            const bool id_change = st.mode != 3 && (int)(st.polyid & 0x3F) != last_stencil;
            const bool other_change = shadow || shadow_st ||
                                      (tex && (tex != last_tex || st.wrap != last_wrap)) || env != last_env ||
                                      fog != last_fog || (int)st.cull != last_cull || hl != u_hl || pa != u_pa ||
                                      iw != u_iw || ih != u_ih || fog != u_fog;
            // Merge particle runs. Particle systems give each quad an increasing polygon
            // ID so they blend over each other, and every ID change was a new draw (over
            // 200 draws a frame for a star burst). In the translucent pass, a run that only
            // differs by a new ID joins the open draw; the DS rule only blocks same-ID
            // overlap, so this looks the same apart from the odd sparkle.
            // GAME3DS_MERGE_IDS_OFF=1 disables it.
            const uint64_t id_bit = 1ull << (st.polyid & 0x3F);
            bool id_split = by_id && id_change;
            if (merge_ids && pass == 1 && st.mode != 3 && n && !other_change)
                id_split = (open_ids & id_bit) && open_ids != id_bit;
            const bool change = other_change || id_split;
            if (change) {
                if (n) {
                    const int w = shadow || shadow_st ? RecProf::W_SHADOW
                                  : tex && (tex != last_tex || st.wrap != last_wrap) ? RecProf::W_TEX
                                  : env != last_env ? RecProf::W_ENV
                                  : id_split ? RecProf::W_ID
                                  : fog != last_fog || fog != u_fog ? RecProf::W_FOG
                                  : (int)st.cull != last_cull ? RecProf::W_CULL
                                  : hl != u_hl || pa != u_pa ? RecProf::W_ALPHA
                                  : RecProf::W_SCALE;
                    ++g_rp.why[w];
                }
                issue();
                open_ids = 0;
                if (shadow_st) {   // a shadow run set the state its own way
                    forget();
                    shadow_st = nullptr;
                }
                if (tex && (tex != last_tex || st.wrap != last_wrap)) {
                    C3D_TexSetWrap(tex, wrap_mode(st.wrap & 1, st.wrap & 4),
                                   wrap_mode(st.wrap & 2, st.wrap & 8));
                    C3D_TexBind(0, tex);
                    last_tex = tex;
                    last_wrap = st.wrap;
                }
                if (env != last_env) {
                    if (toon)
                        env_toon(tex != nullptr, g_toon_shade);
                    else
                        env_textured(tex != nullptr);
                    last_env = env;
                }
                if (st.mode != 3 && (int)(st.polyid & 0x3F) != last_stencil) {
                    run_stencil(pass == 1, st.polyid);
                    last_stencil = st.polyid & 0x3F;
                }
                if (fog != last_fog) {
                    fog_stage(fog);
                    last_fog = fog;
                }
                if ((int)st.cull != last_cull) {
                    C3D_CullFace(cull_for(st.cull));
                    last_cull = st.cull;
                }
                lap(RecProf::STATE);
                // the run's uniforms: texel scale, colour (draw()'s highlight
                // takes red on all three), alpha, the fog ramp's map
                if (iw != u_iw || ih != u_ih) {
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_rloc_texscale, iw, ih, 0.0f, 0.0f);
                    u_iw = iw;
                    u_ih = ih;
                }
                if (hl != u_hl || pa != u_pa) {
                    const float k = 1.0f / 255.0f, kc = hl ? 0.0f : k, kh = hl ? k : 0.0f;
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_rloc_colk, kc, kc, kc, pa);
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_rloc_colh, kh, kh, kh, 0.0f);
                    u_hl = hl;
                    u_pa = pa;
                }
                if (fog != u_fog) {
                    if (fog)
                        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_rloc_fogk, g_fogk[0], g_fogk[1], g_fogk[2], 0.0f);
                    else
                        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_rloc_fogk, 0.0f, 0.0f, 0.0f, 0.0f);
                    u_fog = fog;
                }
                if (shadow) {
                    shadow_st = &st;
                    shadow_tex = tex;
                }
                lap(RecProf::UNIF);
            }
            if (pass == 1) {
                // A second ID in the draw: its stencil test always passes, since members have
                // to blend over each other like their different IDs allow on the DS. -2 makes
                // the next draw set its own state.
                if (!change && n && (open_ids & ~id_bit) == open_ids && last_stencil != -2) {
                    C3D_StencilTest(true, GPU_ALWAYS, (int)(0x40 | (last_stencil & 0x3F)), 0x7F, 0x7F);
                    last_stencil = -2;
                }
                open_ids |= id_bit;
            }
            // its triangles: whole when its ids fit the ring, else in pieces
            if ((unsigned)run.id_hi - run.id_lo < (unsigned)kRing) {
                add(run.first, run.count, run.id_lo, run.id_hi);
                continue;
            }
            const uint16_t *ix = rec.idx.data() + (size_t)run.first * 3;
            uint32_t pf = run.first, pn = 0;
            unsigned plo = 0, phi = 0;
            for (uint32_t t = 0; t < run.count; ++t, ix += 3) {
                const unsigned ia = rec.v[ix[0]].id & 0x7FFFu, ib = rec.v[ix[1]].id & 0x7FFFu,
                               ic = rec.v[ix[2]].id & 0x7FFFu;
                const unsigned tlo = ia < ib ? (ia < ic ? ia : ic) : (ib < ic ? ib : ic);
                const unsigned thi = ia > ib ? (ia > ic ? ia : ic) : (ib > ic ? ib : ic);
                if (pn && (thi > phi ? thi : phi) - (tlo < plo ? tlo : plo) >= (unsigned)kRing) {
                    add(pf, pn, plo, phi);
                    issue();
                    pf = run.first + t;
                    pn = 0;
                }
                if (!pn) {
                    plo = tlo;
                    phi = thi;
                } else {
                    if (tlo < plo) plo = tlo;
                    if (thi > phi) phi = thi;
                }
                ++pn;
            }
            if (pn) add(pf, pn, plo, phi);
            lap(RecProf::TRIS);
        }
    }
    issue();
    if (shadow_st) forget();
    g_fog_by_depth = false;
    fog_stage(false);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    use_main_program();
    lap(RecProf::SWITCH);
    if (!uploaded) {
        PresentScope tf(P_VBO_FLUSH);
        const size_t span = (size_t)(rvbo + nv * sizeof(ntr::GxRecVertex) - block);
        GSPGPU_FlushDataCache(block, span);
    } else {
        ++g_rp.reused;
    }
    lap(RecProf::FLUSH);
    if (prof) {
        g_rp.verts += (unsigned)nv;
        g_rp.tris += (unsigned)(ni / 3);
        const unsigned per_report = prof_every > 1 && prof_every < 120 ? 120u / (unsigned)prof_every : 1u;
        if (++g_rp.frames == (prof_every == 1 ? 120u : per_report)) {
            if (!g_rp_has) {
                g_rp_ready = g_rp;
                __dmb();
                g_rp_has = true;
            }
            memset(&g_rp, 0, sizeof g_rp);
        }
    }
    return true;
}

bool rec_prof_line(char *out, size_t n)
{
    if (!g_rp_has) return false;
    __dmb();
    const RecProf &p = g_rp_ready;
    const double k = 1.0 / 268111.856 / (p.frames ? p.frames : 1);
    int w = snprintf(out, n, "[gpu-xform] draw_record per frame:");
    for (int i = 0; i < RecProf::N && w > 0 && (size_t)w < n; ++i)
        w += snprintf(out + w, n - (size_t)w, " %s %.3f", kRecProfNames[i], p.t[i] * k);
    if (w > 0 && (size_t)w < n)
        snprintf(out + w, n - (size_t)w, " ms; runs %u draws %u ring uploads %u verts %u tris %u; uploads reused %u of %u frames",
                 p.runs / (p.frames ? p.frames : 1), p.draws / (p.frames ? p.frames : 1),
                 p.uploads / (p.frames ? p.frames : 1), p.verts / (p.frames ? p.frames : 1),
                 p.tris / (p.frames ? p.frames : 1), p.reused, p.frames);
    w = (int)strlen(out);
    for (int i = 0; i < RecProf::W_N && w > 0 && (size_t)w < n; ++i)
        w += snprintf(out + w, n - (size_t)w, "%s%s %.1f", i ? ", " : "; draws closed by: ", kRecWhyNames[i],
                      (double)p.why[i] / (p.frames ? p.frames : 1));
    g_rp_has = false;
    return true;
}

namespace {

// A screen-space quad as two triangles, appended to the vertex buffer:
// texture (s0, t0) at (x0, y0) to (s1, t1) at (x1, y1).
size_t push_quad_st(float x0, float y0, float x1, float y1, float s0, float t0, float s1,
                    float t1, float r, float g, float b, float a)
{
    if (g_used + 6 > g_vhi) return (size_t)-1;
    const size_t first = g_used;
    const float xs[6] = {x0, x1, x0, x1, x1, x0}, ys[6] = {y0, y0, y1, y0, y1, y1};
    const float ss[6] = {s0, s1, s0, s1, s1, s0}, ts[6] = {t0, t0, t1, t0, t1, t1};
    for (int i = 0; i < 6; ++i) {
        Vtx &o = g_vbo[g_used++];
        o.x = xs[i]; o.y = ys[i]; o.z = 0.0f; o.w = 1.0f;
        o.s = ss[i]; o.t = ts[i];
        o.r = r; o.g = g; o.b = b; o.a = a;
        o.fs = 0.0f; o.ft = 0.5f;
    }
    return first;
}

size_t push_quad(float x0, float y0, float x1, float y1, float s1, float t1,
                 float r, float g, float b, float a)
{
    return push_quad_st(x0, y0, x1, y1, 0, 0, s1, t1, r, g, b, a);
}

// Swizzle a w x h 0xAARRGGBB picture into an RGBA8 texture, top row at t = 0.
// Eight pixels of a tile row at a time (Morton offsets 0 1 4 5 16 17 20 21),
// with the channel shuffle as a rotate. This runs on both screens every frame,
// so it's worth keeping fast.
void swizzle_into(u32 *dst, int tex_w, int tex_h, const uint32_t *px, int stride, int w, int h)
{
    const u32 tiles_x = (u32)tex_w >> 3;
    auto rot = [](u32 c) { return (c << 8) | (c >> 24); };
    for (int y = 0; y < h; ++y) {
        const u32 fy = (u32)(tex_h - 1 - y);
        const uint32_t *row = px + (size_t)y * stride;
        u32 *trow = dst + (fy >> 3) * tiles_x * 64 + morton8(0, fy & 7);
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            u32 *t = trow + ((u32)x >> 3) * 64;
            const uint32_t *s = row + x;
            t[0] = rot(s[0]);  t[1] = rot(s[1]);  t[4] = rot(s[2]);  t[5] = rot(s[3]);
            t[16] = rot(s[4]); t[17] = rot(s[5]); t[20] = rot(s[6]); t[21] = rot(s[7]);
        }
        for (; x < w; ++x) trow[((u32)x >> 3) * 64 + morton8((u32)x & 7, 0)] = rot(row[x]);
    }
}

}  // namespace

void draw_overlay(const uint32_t *px, int stride, int w, int h, OverlayMode mode,
                  const Viewport &vp)
{
    overlay_draw(overlay_upload(px, stride, w, h), mode, vp);
}

int overlay_upload(const uint32_t *px, int stride, int w, int h)
{
    PresentScope timer(P_OVERLAY_SUBMIT);
    if (!px || w <= 0 || h <= 0) return -1;
    if (g_ov_used >= kMaxOverlays) return -1;
    ahead_settle();   // the swizzle writes a texture the last frame may be reading
    if (w > kOvW) w = kOvW;
    if (h > kOvH) h = kOvH;
    const int slot = g_ov_used++;
    C3D_Tex &tex = g_ov[slot];
    if (!g_ov_up[slot]) {
        if (!C3D_TexInit(&tex, kOvW, kOvH, GPU_RGBA8)) return -1;
        C3D_TexSetFilter(&tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        g_ov_up[slot] = true;
    }
    g_ov_w[slot] = w;
    g_ov_h[slot] = h;
    // Swizzle the used rectangle in (same layout as texture_for). The GPU may still
    // be reading last frame's copy; frame_begin's SYNCDRAW waited for it.
    {
        PresentScope timer(P_OVERLAY_SWIZZLE);
        swizzle_into((u32 *)tex.data, kOvW, kOvH, px, stride, w, h);
    }
    { PresentScope timer(P_OVERLAY_FLUSH); C3D_TexFlush(&tex); }
    return slot;
}

// ---- overlays through the transfer engine ----
//
// Let the GPU tile the 2D pictures instead of the CPU (the CPU swizzle was
// about 4 ms a frame on hardware). A linear picture 256 wide and a multiple of
// 8 tall goes through a display transfer into its own 256x256 texture. The
// bytes aren't converted: an 0xAARRGGBB word read as RGBA8 has its channels
// rotated, and overlay_draw puts them back with three combiner stages.
//
// Call it before frame_begin: outside a frame the transfer waits for the last
// frame and for itself, so the source can be reused right after. Slots start
// at kTransferBase.
//
// Pictures whose rows don't match the texture width (the 320-wide wide build)
// are first copied into a linear scratch at the texture's width.
namespace {
constexpr int kTransferBase = 100;
constexpr int kTrH = 256;
C3D_Tex g_ovt[kMaxOverlays];
bool g_ovt_up[kMaxOverlays];
int g_ovt_used, g_ovt_w[kMaxOverlays], g_ovt_h[kMaxOverlays], g_ovt_tw[kMaxOverlays];
u32 *g_restride;               // linear, 512 x kTrH, for the re-laid pictures
// what each slot's texture holds: the picture's hash and shape when it was
// transferred (overlay_transfer keeps a slot whose picture has not changed)
uint64_t g_ovt_hash[kMaxOverlays];
uint64_t g_ovt_sig[kMaxOverlays];   // the maker's number for the picture held
unsigned g_ovt_gen[kMaxOverlays];   // bumped each time the slot's texture is written (overlay_sharpen)
int g_ovt_stride[kMaxOverlays];
bool g_ovt_held[kMaxOverlays];

// Hash a picture (two lanes, four words per step). Too slow on hardware to use
// all the time; the keep now goes by the makers' numbers and this is for
// GPU3DS_OVERLAY_HASH / GPU3DS_OVERLAY_VERIFY checks.
uint64_t picture_hash(const uint32_t *px, int stride, int w, int h)
{
    uint32_t a = 0x811C9DC5u ^ (uint32_t)w, b = 0x9E3779B9u ^ (uint32_t)h;
    for (int y = 0; y < h; ++y) {
        const uint32_t *r = px + (size_t)y * stride;
        int x = 0;
        for (; x + 4 <= w; x += 4) {
            a = (a ^ r[x]) * 0x01000193u;
            b = (b ^ r[x + 1]) * 0x85EBCA6Bu;
            a = (a ^ r[x + 2]) * 0x01000193u;
            b = (b ^ r[x + 3]) * 0x85EBCA6Bu;
        }
        for (; x < w; ++x) a = (a ^ r[x]) * 0x01000193u;
        b ^= a >> 15;
    }
    return ((uint64_t)a << 32) | b;
}

void env_permuted()
{
    // stage 0: red <- texel green, alpha <- texel red
    C3D_TexEnv *e0 = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e0);
    C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(e0, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(e0, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvColor(e0, 0xFF0000FFu);   // ABGR: red lane only
    C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpAlpha(e0, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);
    // stage 1: + green <- texel blue
    C3D_TexEnv *e1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(e1);
    C3D_TexEnvSrc(e1, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(e1, GPU_TEVOP_RGB_SRC_B, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(e1, C3D_RGB, GPU_MULTIPLY_ADD);
    C3D_TexEnvColor(e1, 0xFF00FF00u);   // green lane
    C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
    // stage 2: + blue <- texel alpha
    C3D_TexEnv *e2 = C3D_GetTexEnv(2);
    C3D_TexEnvInit(e2);
    C3D_TexEnvSrc(e2, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(e2, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(e2, C3D_RGB, GPU_MULTIPLY_ADD);
    C3D_TexEnvColor(e2, 0xFFFF0000u);   // blue lane
    C3D_TexEnvSrc(e2, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e2, C3D_Alpha, GPU_REPLACE);
}
}  // namespace

void transfers_begin()
{
    g_ovt_used = 0;
}

void overlay_transfers_free()
{
    for (int i = 0; i < kMaxOverlays; ++i) {
        if (g_ovt_up[i]) { C3D_TexDelete(&g_ovt[i]); g_ovt_up[i] = false; }
        g_ovt_held[i] = false;
    }
    if (g_restride) { linearFree(g_restride); g_restride = nullptr; }
}

int overlay_transfer(const uint32_t *px, int stride, int w, int h, uint64_t sig)
{
    static const bool off = getenv("GPU3DS_NOTRANSFER") != nullptr;
    if (off || !px || w <= 0 || w > 512 || (w & 7) || stride < w || (stride & 3) || h <= 0 ||
        h > kTrH || (h & 7) || ((uintptr_t)px & 15) || !osConvertVirtToPhys(px) ||
        g_ovt_used >= kMaxOverlays)
        return -1;
    const int tw = w <= 256 ? 256 : 512;
    const bool restride = stride != tw;
    if (restride && !g_restride) {
        g_restride = (u32 *)linearAlloc((size_t)512 * kTrH * 4);
        if (!g_restride) return -1;
    }
    PresentScope timer(P_OVERLAY_SUBMIT);
    const int slot = g_ovt_used;
    C3D_Tex &tex = g_ovt[slot];
    // Skip the transfer if this slot already holds the same picture (the HUD is
    // usually unchanged). Known from the maker's number (sig), or by hashing under
    // GPU3DS_OVERLAY_HASH=1. GPU3DS_OVERLAY_ALWAYS=1 always transfers.
    static const bool always = getenv("GPU3DS_OVERLAY_ALWAYS") != nullptr;
    static const bool by_hash = getenv("GPU3DS_OVERLAY_HASH") != nullptr;
    static const bool verify = getenv("GPU3DS_OVERLAY_VERIFY") != nullptr;
    uint64_t hash = 0;
    if (!always) {
        if (by_hash || verify) {
            PresentScope hs(P_OVERLAY_SWIZZLE);
            hash = picture_hash(px, stride, w, h);
        }
        const bool shape = g_ovt_up[slot] && g_ovt_held[slot] && g_ovt_tw[slot] == tw &&
                           g_ovt_w[slot] == w && g_ovt_h[slot] == h && g_ovt_stride[slot] == stride;
        bool keep = shape && (by_hash ? g_ovt_hash[slot] == hash : sig && g_ovt_sig[slot] == sig);
        if (keep && verify && !by_hash && g_ovt_hash[slot] != hash) {
            ++g_gt.ov_bad;   // the maker said the same picture; the pixels differ
            keep = false;
        }
        if (keep) {
            ++g_ovt_used;
            ++g_gt.ov_kept;
            return kTransferBase + slot;
        }
    }
    g_ovt_held[slot] = false;
    ++g_gt.ov_sent;
    ++g_ovt_gen[slot];
    if (g_ovt_up[slot] && g_ovt_tw[slot] != tw) { C3D_TexDelete(&tex); g_ovt_up[slot] = false; }
    if (!g_ovt_up[slot]) {
        if (!C3D_TexInit(&tex, tw, kTrH, GPU_RGBA8)) return -1;
        C3D_TexSetFilter(&tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        g_ovt_up[slot] = true;
        g_ovt_tw[slot] = tw;
    }
    ++g_ovt_used;
    g_ovt_w[slot] = w;
    g_ovt_h[slot] = h;
    {
        PresentScope f(P_OVERLAY_FLUSH);
        GSPGPU_FlushDataCache(px, ((u32)stride * (h - 1) + (restride ? w : tw)) * 4);
    }
    // In a frame built ahead the two transfers are held (g_xfer) and
    // ahead_flush runs them, to their end, before anything of the frame.
    if (g_ahead) GX_BindQueue(&g_xfer);
    const u32 *src = px;
    // Line size and gaps in 16-byte units; the size counts output bytes.
    const u32 copy_in = GX_BUFFER_DIM(w / 4, (stride - w) / 4), copy_out = GX_BUFFER_DIM(w / 4, (tw - w) / 4);
    if (restride) {
        if (g_ahead)
            GX_TextureCopy((u32 *)px, copy_in, g_restride, copy_out, (u32)w * h * 4, GX_TRANSFER_RAW_COPY(1));
        else
            C3D_SyncTextureCopy((u32 *)px, copy_in, g_restride, copy_out, (u32)w * h * 4,
                                GX_TRANSFER_RAW_COPY(1));
        src = g_restride;
    }
    // The picture sits at the top of the texture and tile rows count from the
    // bottom, so it starts this many tile rows in.
    u32 *dst = (u32 *)tex.data + (size_t)((kTrH - h) / 8) * (tw / 8) * 64;
    const u32 flags = GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
                      GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    if (g_ahead) {
        GX_DisplayTransfer((u32 *)src, GX_BUFFER_DIM(tw, h), dst, GX_BUFFER_DIM(tw, h), flags);
        GX_BindQueue(&g_hold);
    } else {
        C3D_SyncDisplayTransfer((u32 *)src, GX_BUFFER_DIM(tw, h), dst, GX_BUFFER_DIM(tw, h), flags);
    }
    g_ovt_hash[slot] = hash;
    g_ovt_sig[slot] = sig;
    g_ovt_stride[slot] = stride;
    g_ovt_held[slot] = !always;
    return kTransferBase + slot;
}

void overlay_draw(int slot, OverlayMode mode, const Viewport &vp)
{
    PresentScope timer(P_OVERLAY_SUBMIT);
    const bool transferred = slot >= kTransferBase;
    if (transferred ? slot - kTransferBase >= g_ovt_used : (slot < 0 || slot >= g_ov_used))
        return;
    const int i = transferred ? slot - kTransferBase : slot;
    C3D_Tex &tex = transferred ? g_ovt[i] : g_ov[i];
    const float s1 = transferred ? (float)g_ovt_w[i] / g_ovt_tw[i] : (float)g_ov_w[i] / kOvW;
    const float t1 = transferred ? (float)g_ovt_h[i] / kTrH : (float)g_ov_h[i] / kOvH;
    const size_t first = push_quad(vp.x, vp.y, vp.x + vp.w, vp.y + vp.h, s1, t1, 1, 1, 1, 1);
    if (first == (size_t)-1) return;
    C3D_TexBind(0, &tex);
    if (transferred) {
        overlay_draw_modes(first, mode, true);
        return;
    }
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    overlay_draw_modes(first, mode, false);
}

void overlay_draw_part(int slot, OverlayMode mode, const Viewport &vp, float u0, float v0,
                       float u1, float v1)
{
    PresentScope timer(P_OVERLAY_SUBMIT);
    const bool transferred = slot >= kTransferBase;
    if (transferred ? slot - kTransferBase >= g_ovt_used : (slot < 0 || slot >= g_ov_used))
        return;
    const int i = transferred ? slot - kTransferBase : slot;
    C3D_Tex &tex = transferred ? g_ovt[i] : g_ov[i];
    // texture units per picture pixel (overlay_draw's s1 / w, t1 / h)
    const float ks = transferred ? 1.0f / g_ovt_tw[i] : 1.0f / kOvW;
    const float kt = transferred ? 1.0f / kTrH : 1.0f / kOvH;
    const size_t first = push_quad_st(vp.x, vp.y, vp.x + vp.w, vp.y + vp.h, u0 * ks, v0 * kt,
                                      u1 * ks, v1 * kt, 1, 1, 1, 1);
    if (first == (size_t)-1) return;
    C3D_TexBind(0, &tex);
    if (transferred) {
        overlay_draw_modes(first, mode, true);
        return;
    }
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    overlay_draw_modes(first, mode, false);
}

// HUD picture (hud_draw): two textures used in turn, rewritten only when the
// caller's version changes. The last frame may still be reading the other one,
// so no mid-frame hand-over is needed.
namespace {
constexpr int kHudW = 128, kHudH = 32;
C3D_Tex g_hud_tex[2];
int g_hud_up;            // 0 untried, 1 made, -1 none
int g_hud_i;             // the texture drawn
unsigned g_hud_ver;
int g_hud_frame;         // the frame that last wrote one (g_frame_no)
int g_hud_w, g_hud_h;
}  // namespace

void hud_draw(const uint32_t *px, int stride, int w, int h, unsigned version, const Viewport &vp)
{
    PresentScope timer(P_OVERLAY_SUBMIT);
    if (!px || w <= 0 || h <= 0) return;
    if (w > kHudW) w = kHudW;
    if (h > kHudH) h = kHudH;
    if (!g_hud_up) {
        g_hud_up = -1;
        for (C3D_Tex &t : g_hud_tex) {
            if (!C3D_TexInit(&t, kHudW, kHudH, GPU_RGBA8)) return;
            C3D_TexSetFilter(&t, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        }
        g_hud_up = 1;
        g_hud_ver = version + 1;
        g_hud_frame = g_frame_no - 2;
    }
    if (g_hud_up < 0) return;
    if (version != g_hud_ver || w != g_hud_w || h != g_hud_h) {
        if (g_frame_no != g_hud_frame && g_frame_no - g_hud_frame < 2)
            ahead_settle();   // the last frame wrote and may be reading the other
        if (g_frame_no != g_hud_frame) g_hud_i ^= 1;
        C3D_Tex &t = g_hud_tex[g_hud_i];
        swizzle_into((u32 *)t.data, kHudW, kHudH, px, stride, w, h);
        C3D_TexFlush(&t);
        g_hud_ver = version;
        g_hud_frame = g_frame_no;
        g_hud_w = w;
        g_hud_h = h;
    }
    const size_t first = push_quad(vp.x, vp.y, vp.x + vp.w, vp.y + vp.h, (float)w / kHudW,
                                   (float)h / kHudH, 1, 1, 1, 1);
    if (first == (size_t)-1) return;
    C3D_TexBind(0, &g_hud_tex[g_hud_i]);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    overlay_draw_modes(first, OVERLAY_PREMUL, false);
}

namespace {
// Sharp scaling. 256x192 shown at 320x240 is 1.25x, so nearest sampling makes
// one-pixel outlines alternate between one and two pixels wide (the pause menu
// buttons looked jagged). Instead draw the picture at 2x nearest into a
// 512x512 texture and then bilinear to the screen: lines keep an even width
// and only their edges soften. The 2x pass only reruns when the picture
// changes. GAME3DS_SHARP=0 draws nearest.
C3D_Tex g_sb_tex;
C3D_RenderTarget *g_sb;
C3D_Mtx g_sb_proj;
int g_sb_state;          // 0 untried, 1 ready, -1 none
int g_sb_src = -1;       // the transfer slot the texture was made from
unsigned g_sb_gen;
int g_sb_w, g_sb_h;      // the picture's size (the texture holds 2x)

bool sharp_ready()
{
    if (g_sb_state) return g_sb_state > 0;
    g_sb_state = -1;
    if (getenv("GAME3DS_SHARP") && atoi(getenv("GAME3DS_SHARP")) == 0) return false;
    if (!C3D_TexInitVRAM(&g_sb_tex, 512, 512, GPU_RGBA8)) return false;
    C3D_TexSetFilter(&g_sb_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_sb_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    g_sb = C3D_RenderTargetCreateFromTex(&g_sb_tex, GPU_TEXFACE_2D, 0, -1);
    if (!g_sb) {
        C3D_TexDelete(&g_sb_tex);
        return false;
    }
    C3D_FrameBufAttrib(&g_sb->frameBuf, 512, 512, false);
    g_sb_state = 1;
    printf("[gpu3ds] sharp scaling on (a 512 x 512 VRAM target)\n");
    return true;
}
}  // namespace

bool overlay_sharpen(int slot, int w, int h)
{
    if (w <= 0 || h <= 0 || w * 2 > 512 || h * 2 > 512 || !sharp_ready()) return false;
    const bool transferred = slot >= kTransferBase;
    if (transferred ? slot - kTransferBase >= g_ovt_used : (slot < 0 || slot >= g_ov_used))
        return false;
    // the same picture as the texture holds: nothing to draw
    if (transferred && slot == g_sb_src && g_ovt_gen[slot - kTransferBase] == g_sb_gen &&
        g_sb_w == w && g_sb_h == h)
        return true;
    PresentScope timer(P_OVERLAY_SUBMIT);
    const int W = w * 2, H = h * 2;   // the target, tilted: H across, W down
    C3D_FrameDrawOn(g_sb);
    C3D_SetViewport(0, 0, (u32)H, (u32)W);
    Mtx_OrthoTilt(&g_sb_proj, 0.0f, (float)W, (float)H, 0.0f, 0.0f, 1.0f, false);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_loc_proj, &g_sb_proj);
    g_cur_proj = &g_sb_proj;
    Viewport vp;
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.w = (float)W;
    vp.h = (float)H;
    overlay_draw(slot, OVERLAY_OPAQUE, vp);
    g_sb_src = transferred ? slot : -1;
    g_sb_gen = transferred ? g_ovt_gen[slot - kTransferBase] : 0;
    g_sb_w = w;
    g_sb_h = h;
    return true;   // the caller's frame_target puts its own target and projection back
}

void overlay_draw_sharp(const Viewport &vp, OverlayMode mode)
{
    if (g_sb_state <= 0 || g_used + 6 > g_vhi) return;
    PresentScope timer(P_OVERLAY_SUBMIT);
    const int W = g_sb_w * 2, H = g_sb_h * 2;
    // Each corner's texel, as in resolve(): where the 2x pass put that corner on
    // the tilted target, over the 512x512 texture.
    const float px[6] = {0, (float)W, 0, (float)W, (float)W, 0};
    const float py[6] = {0, 0, (float)H, 0, (float)H, (float)H};
    const size_t first = g_used;
    for (int i = 0; i < 6; ++i) {
        const C3D_FVec *r = g_sb_proj.r;
        const float cw = r[3].x * px[i] + r[3].y * py[i] + r[3].w;
        const float cx = (r[0].x * px[i] + r[0].y * py[i] + r[0].w) / cw;
        const float cy = (r[1].x * px[i] + r[1].y * py[i] + r[1].w) / cw;
        Vtx &o = g_vbo[g_used++];
        o.x = vp.x + vp.w * px[i] / (float)W;
        o.y = vp.y + vp.h * py[i] / (float)H;
        o.z = 0.0f;
        o.w = 1.0f;
        o.s = (cx + 1.0f) * 0.5f * (float)H / 512.0f;
        o.t = ((cy + 1.0f) * 0.5f * (float)W + (float)(512 - W)) / 512.0f;
        o.r = o.g = o.b = o.a = 1.0f;
        o.fs = 0.0f;
        o.ft = 0.5f;
    }
    C3D_TexBind(0, &g_sb_tex);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    overlay_draw_modes(first, mode, false);
}

// The affine BG's textures: two in turn, as the HUD's (a frame built ahead,
// still on the GPU, reads the other), each sized to the map.
namespace {
C3D_Tex g_aff_tex[2];
int g_aff_tw[2], g_aff_th[2];   // 0: not made
int g_aff_i;
unsigned g_aff_ver;
bool g_aff_have;
int g_aff_frame = -100;
}  // namespace

void affine_draw(const SubAffine &a, const Viewport &vp, int src_w, int src_h)
{
    PresentScope timer(P_OVERLAY_SUBMIT);
    const int w = a.map_w, h = a.map_h;
    if (!a.pic || w < 8 || h < 8 || w > 1024 || h > 1024 || (w & (w - 1)) || (h & (h - 1)) ||
        g_used + 6 > g_vhi)
        return;
    if (!g_aff_have || a.ver != g_aff_ver || g_aff_tw[g_aff_i] != w || g_aff_th[g_aff_i] != h) {
        if (g_frame_no != g_aff_frame && g_frame_no - g_aff_frame < 2)
            ahead_settle();   // the last frame wrote and may be reading the other
        if (g_frame_no != g_aff_frame) g_aff_i ^= 1;
        C3D_Tex &t = g_aff_tex[g_aff_i];
        if (g_aff_tw[g_aff_i] != w || g_aff_th[g_aff_i] != h) {
            if (g_aff_tw[g_aff_i]) C3D_TexDelete(&t);
            g_aff_tw[g_aff_i] = g_aff_th[g_aff_i] = 0;
            if (!C3D_TexInit(&t, (u16)w, (u16)h, GPU_RGBA8)) {
                g_aff_have = false;
                return;
            }
            C3D_TexSetFilter(&t, GPU_NEAREST, GPU_NEAREST);
            g_aff_tw[g_aff_i] = w;
            g_aff_th[g_aff_i] = h;
        }
        swizzle_into((u32 *)t.data, w, h, a.pic, w, w, h);
        C3D_TexFlush(&t);
        g_aff_ver = a.ver;
        g_aff_have = true;
        g_aff_frame = g_frame_no;
    }
    C3D_Tex &t = g_aff_tex[g_aff_i];
    const GPU_TEXTURE_WRAP_PARAM wrap = a.wrap ? GPU_REPEAT : GPU_CLAMP_TO_BORDER;
    C3D_TexSetWrap(&t, wrap, wrap);
    t.border = 0;   // past the map: transparent, the backdrop shows
    // Each corner's texel: the DS takes pixel (x, y) from map pixel
    // (ref + pa * x + pb * y) >> 8, and screen corners are half a pixel off the
    // sample centres.
    float s[4], tt[4];
    const float cx[4] = {0, (float)src_w, 0, (float)src_w}, cy[4] = {0, 0, (float)src_h, (float)src_h};
    for (int k = 0; k < 4; ++k) {
        const float X = cx[k] - 0.5f, Y = cy[k] - 0.5f;
        s[k] = ((float)a.refx + (float)a.pa * X + (float)a.pb * Y) / (256.0f * (float)w);
        tt[k] = ((float)a.refy + (float)a.pc * X + (float)a.pd * Y) / (256.0f * (float)h);
    }
    if (a.wrap) {   // whole turns of a repeated map change nothing: keep the numbers small
        const float fs = floorf(s[0]), ft = floorf(tt[0]);
        for (int k = 0; k < 4; ++k) { s[k] -= fs; tt[k] -= ft; }
    }
    const int order[6] = {0, 1, 2, 1, 3, 2};   // push_quad_st's: TL TR BL, TR BR BL
    const size_t first = g_used;
    for (int i = 0; i < 6; ++i) {
        const int k = order[i];
        Vtx &o = g_vbo[g_used++];
        o.x = vp.x + vp.w * cx[k] / (float)src_w;
        o.y = vp.y + vp.h * cy[k] / (float)src_h;
        o.z = 0.0f;
        o.w = 1.0f;
        o.s = s[k];
        o.t = tt[k];
        o.r = o.g = o.b = o.a = 1.0f;
        o.fs = 0.0f;
        o.ft = 0.5f;
    }
    C3D_TexBind(0, &t);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    overlay_draw_modes(first, OVERLAY_PREMUL, false);
}

namespace {
void overlay_draw_modes(size_t first, OverlayMode mode, bool permuted)
{
    if (permuted) env_permuted();
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    if (mode == OVERLAY_OPAQUE) {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
        C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
        ++g_stats.draw_calls;
    } else if (mode == OVERLAY_PREMUL) {
        // c + dst * (1 - a); a zero word adds nothing, so no alpha test.
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_ZERO, GPU_ONE);
        C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
        ++g_stats.draw_calls;
    } else {
        // GX_OV_FRONT: in front of whatever is there.
        C3D_AlphaTest(true, GPU_EQUAL, 0xFF);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
        C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
        // GX_OV_BEHIND: only where the 3D layer drew nothing, which is
        // destination alpha 0 (pass_state): src * (1 - dstA) + dst * dstA.
        C3D_AlphaTest(true, GPU_EQUAL, 0xFE);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE_MINUS_DST_ALPHA, GPU_DST_ALPHA,
                       GPU_ZERO, GPU_ONE);
        C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
        g_stats.draw_calls += 2;
        if (mode == OVERLAY_KEPT_BLEND) {
            // GX_OV_BLEND: c + dst * (1 - a). Alpha 0 passes the test too and adds nothing;
            // destination alpha is kept.
            C3D_AlphaTest(true, GPU_LESS, 0xFE);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ZERO, GPU_ONE);
            C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
            ++g_stats.draw_calls;
        }
    }
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    if (permuted) {   // the next draw's own stage 0 must not run through these
        C3D_TexEnvInit(C3D_GetTexEnv(1));
        C3D_TexEnvInit(C3D_GetTexEnv(2));
    }
}
}  // namespace

uint64_t overlay_key(int slot)
{
    const int i = slot - kTransferBase;
    if (i < 0 || i >= g_ovt_used || !g_ovt_held[i]) return 0;
    return (uint64_t)(i + 1) << 32 | g_ovt_gen[i];
}

bool screen_holds(Screen s, uint64_t key)
{
    const bool held = key && g_screen_key[s] == key;
    ++(held ? g_gt.bot_held : g_gt.bot_drawn);
    return held;
}

void screen_mark(Screen s, uint64_t key)
{
    g_screen_key[s] = key;
}

void screens_forget()
{
    g_screen_key[0] = g_screen_key[1] = 0;
}

void draw_fade_masked(int evy, bool white, const uint8_t *mask, int stride, int w, int h,
                      bool shown3d, bool clear_opaque, const Viewport &vp)
{
    if (evy <= 0) return;
    if (!mask || !fade_mask_upload(mask, stride, w, h, shown3d, clear_opaque)) {
        draw_fade(evy, white, vp);
        return;
    }
    fade_masked_draw(evy, white, vp);
}

bool fade_mask_upload(const uint8_t *mask, int stride, int w, int h, bool shown3d,
                      bool clear_opaque)
{
    PresentScope timer(P_EFFECTS);
    g_mask_w = g_mask_h = 0;
    if (!mask) return false;
    if (w > kMaskW) w = kMaskW;
    if (h > 256) h = 256;
    const int m = g_mask_cur ^ 1;   // the one the last frames did not draw with
    C3D_Tex &tex = g_mask_tex[m];
    if (!g_mask_up[m]) {
        if (!C3D_TexInit(&tex, kMaskW, 256, GPU_RGBA8)) return false;
        C3D_TexSetFilter(&tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        g_mask_up[m] = true;
    }
    g_mask_cur = m;
    // Both fade verdicts per pixel, with coverage left open: red where the 3D
    // drew, green where it didn't. The texel only depends on five bits of the code
    // and two frame flags, so it's a 32-entry table, eight pixels at a time.
    u32 lut[256];
    for (unsigned code = 0; code < 256; ++code) {
        auto verdict = [&](bool cov) {
            const bool is3d = clear_opaque || (shown3d && cov);
            const bool def = (code & (is3d ? 1u : 2u)) != 0;
            const bool bmv = (code & 4u) != 0;
            return (code & 8u) ? bmv : (code & 16u) ? (cov ? def : bmv) : def;
        };
        // Blue is 1 where the two verdicts differ (the TEV only reads red, green and
        // alpha). ORed over the picture, it tells us whether coverage matters at all
        // this frame.
        const bool cov = verdict(true), back = verdict(false);
        lut[code] = (cov ? 0xFF000000u : 0u) | (back ? 0x00FF0000u : 0u) | (cov != back ? 0x100u : 0u) | 0xFFu;
    }
    u32 any = 0;
    u32 *dst = (u32 *)tex.data;
    for (int y = 0; y < h; ++y) {
        const u32 fy = (u32)(255 - y);
        const uint8_t *row = mask + (size_t)y * stride;
        u32 *trow = dst + (fy >> 3) * (kMaskW / 8) * 64 + morton8(0, fy & 7);
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            u32 *t = trow + ((u32)x >> 3) * 64;
            const uint8_t *s = row + x;
            t[0] = lut[s[0]];  t[1] = lut[s[1]];  t[4] = lut[s[2]];  t[5] = lut[s[3]];
            t[16] = lut[s[4]]; t[17] = lut[s[5]]; t[20] = lut[s[6]]; t[21] = lut[s[7]];
            any |= t[0] | t[1] | t[4] | t[5] | t[16] | t[17] | t[20] | t[21];
        }
        for (; x < w; ++x) any |= trow[((u32)x >> 3) * 64 + morton8((u32)x & 7, 0)] = lut[row[x]];
    }
    C3D_TexFlush(&tex);
    g_mask_w = w;
    g_mask_h = h;
    g_mask_split = (any & 0x100u) != 0;
    return true;
}

void fade_masked_draw(int evy, bool white, const Viewport &vp)
{
    PresentScope timer(P_EFFECTS);
    if (evy <= 0) return;
    if (evy > 16) evy = 16;
    if (!g_mask_w || !g_mask_h) {
        draw_fade(evy, white, vp);
        return;
    }
    const int w = g_mask_w, h = g_mask_h;
    const float k = white ? 1.0f : 0.0f;
    const size_t first = push_quad(vp.x, vp.y, vp.x + vp.w, vp.y + vp.h, (float)w / (float)kMaskW,
                                   (float)h / 256.0f, k, k, k, (float)evy / 16.0f);
    if (first == (size_t)-1) return;
    // One pass when coverage doesn't matter (e.g. the text box darkening): use the
    // red verdict alone with no stencil. The three-pass version took BoB ticks past
    // their vblanks. GAME3DS_FADE_THREE=1 keeps three passes.
    static const bool three = getenv("GAME3DS_FADE_THREE") && atoi(getenv("GAME3DS_FADE_THREE")) != 0;
    if (!g_mask_split && !three) {
        ++g_gt.fade_one;
        C3D_TexBind(0, &g_mask_tex[g_mask_cur]);
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_ZERO, GPU_ONE);
        C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
        ++g_stats.draw_calls;
        return;
    }
    ++g_gt.fade_three;
    // Coverage into stencil bit 7 from the 3D's depth: the quad sits just in front
    // of the clear depth, so it passes wherever a polygon wrote depth. Translucent
    // polygons write no depth, so pixels covered only by them count as backdrop.
    for (size_t i = first; i < first + 6; ++i) g_vbo[i].z = -(1.0f - 1.0f / 65536.0f);
    env_textured(false);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(true, GPU_LESS, (GPU_WRITEMASK)0);
    C3D_StencilTest(true, GPU_ALWAYS, 0x80, 0xFF, 0x80);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_ZERO, GPU_STENCIL_REPLACE);
    C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);

    // The fade twice: covered pixels by the red verdict, the rest by green.
    C3D_TexBind(0, &g_mask_tex[g_mask_cur]);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ZERO, GPU_ONE);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_StencilTest(true, GPU_EQUAL, 0x80, 0x80, 0x00);
    C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
    C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
    C3D_StencilTest(true, GPU_EQUAL, 0x00, 0x80, 0x00);
    C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    g_stats.draw_calls += 3;
}

// The DS rear plane, only inside the picture so the margins stay black (the
// fogged rear colour there looked like a grey veil). Alpha 0 like the clear.
void draw_rear_plane(uint32_t argb, const Viewport &vp)
{
    const size_t first = push_quad(vp.x, vp.y, vp.x + vp.w, vp.y + vp.h, 0, 0,
                                   (float)(argb >> 16 & 0xFF) / 255.0f,
                                   (float)(argb >> 8 & 0xFF) / 255.0f,
                                   (float)(argb & 0xFF) / 255.0f, 0.0f);
    if (first == (size_t)-1) return;
    fog_stage(false);
    env_textured(false);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
    ++g_stats.draw_calls;
}

void draw_fade(float evy, bool white, const Viewport &vp)
{
    PresentScope timer(P_EFFECTS);
    if (evy <= 0.0f) return;
    if (evy > 16.0f) evy = 16.0f;
    const float k = white ? 1.0f : 0.0f;
    const size_t first = push_quad(vp.x, vp.y, vp.x + vp.w, vp.y + vp.h, 0, 0,
                                   k, k, k, evy / 16.0f);
    if (first == (size_t)-1) return;
    env_textured(false);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    // dst + (k - dst) * a: toward black (k = 0) or white (k = 1).
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ZERO, GPU_ONE);
    C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
    ++g_stats.draw_calls;
}

namespace {
C3D_Mtx g_off_proj;

// readback()'s target and its linear copy, made on first use
bool off_target()
{
    if (!g_up) return false;
    if (!g_off) {
        g_off = C3D_RenderTargetCreate(kOffH, kOffW, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        if (!g_off_lin) g_off_lin = (u32 *)linearAlloc(kOffW * kOffH * sizeof(u32));
        // Same tilted projection as the screen, over a screen the size of the DS
        // picture, so the display transfer untiles it the same way.
        Mtx_OrthoTilt(&g_off_proj, 0.0f, (float)kOffW, (float)kOffH, 0.0f, 0.0f, 1.0f, false);
    }
    return g_off && g_off_lin;
}

// The readback's frame on g_off: false if the target couldn't be made.
bool readback_begin()
{
    if (!off_target()) return false;
    capture_finish();   // a capture's copy still in the target is taken first
    textures_check_generation();
    // The GPU's queue, not a vblank (C3D_FRAME_SYNCDRAW): that was half a frame
    // of each display capture spent waiting, in a picture no screen shows.
    C3D_FrameBegin(0);
    // Alpha clears to 0: what is still 0 afterwards was not drawn.
    C3D_RenderTargetClear(g_off, C3D_CLEAR_ALL, 0x00000000, 0);
    C3D_FrameDrawOn(g_off);
    g_tgt_h = kOffH;
    g_tgt_w = kOffW;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_loc_proj, &g_off_proj);
    g_vlo = g_used = 0;   // the whole buffer: C3D_FrameBegin waited for the GPU
    g_vhi = kMaxVerts;
    return true;
}

Viewport readback_vp()
{
    Viewport vp;
    vp.x = vp.y = 0.0f;
    vp.w = (float)ntr::SCREEN_W;
    vp.h = (float)ntr::SCREEN_H;
    return vp;
}

// The drawn picture to the CPU, and the area asked for made from it.
const uint32_t *readback_end(int *stride, int x0, int y0, int x1, int y1)
{
    if (g_used) GSPGPU_FlushDataCache(g_vbo, g_used * sizeof(Vtx));
    C3D_FrameEnd(0);
    g_used = 0;   // done with below (the transfer waits for it): nothing in flight
    g_cmd_prev = nullptr;
    g_tgt_h = kTopH;
    g_tgt_w = kTopW;
    // Outside a frame C3D_SyncDisplayTransfer waits for the queue and the copy;
    // inside one it only queues it. Tiled to linear, RGBA8 kept.
    C3D_SyncDisplayTransfer((u32 *)g_off->frameBuf.colorBuf, GX_BUFFER_DIM(kOffH, kOffW),
                            g_off_lin, GX_BUFFER_DIM(kOffH, kOffW),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                                GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(g_off_lin, kOffW * kOffH * sizeof(u32));
    g_off_done[0] = g_off_done[1] = g_off_done[2] = g_off_done[3] = 0;
    *stride = kOffW;
    return readback_area(x0, y0, x1, y1);
}
}  // namespace

const uint32_t *readback(const ntr::GxTriangle *tris, size_t count, int *stride,
                         int x0, int y0, int x1, int y1)
{
    if (!readback_begin()) return nullptr;
    draw(tris, count, readback_vp());
    return readback_end(stride, x0, y0, x1, y1);
}

const uint32_t *readback_record(const ntr::GxRecord &rec, int *stride, int x0, int y0, int x1,
                                int y1)
{
    if (!readback_begin()) return nullptr;
    // draw_record reads the drawn target's state: this one's, with no eye's shift
    // and no supersampling, and it may draw a second record this frame (the
    // present's comes after). All put back for the present.
    const C3D_Mtx *const proj = g_cur_proj;
    const int kx = g_tgt_kx, ky = g_tgt_ky;
    const bool drawn = g_rec_drawn;
    float eye[4];
    memcpy(eye, g_eye_vec, sizeof eye);
    g_cur_proj = &g_off_proj;
    g_tgt_kx = g_tgt_ky = 1;
    g_rec_drawn = false;
    stereo_eye_vec(0.0f);
    // a copy made each capture in the same vectors: never the last upload's
    record_upload_forget();
    const bool ok = draw_record(rec, readback_vp());
    record_upload_forget();   // nor is the present's record this one
    g_cur_proj = proj;
    g_tgt_kx = kx;
    g_tgt_ky = ky;
    g_rec_drawn = drawn;
    memcpy(g_eye_vec, eye, sizeof eye);
    // drawn or not, the frame is ended (nothing drawn: a clear picture, not used)
    const uint32_t *r = readback_end(stride, x0, y0, x1, y1);
    return ok ? r : nullptr;
}

namespace {
// capture_queue's copy, waited for and handed over by capture_finish
struct CapQueued {
    void (*done)(const uint32_t *lin, int rows, void *arg);
    void *arg;
} g_capq;
}  // namespace

void capture_finish()
{
    if (!g_capq.done) return;
    gxCmdQueue_s *const q = &__C3D_Context;
    gxCmdQueueWait(q, -1);
    GSPGPU_InvalidateDataCache(g_off_lin, kOffW * kOffH * sizeof(u32));
    const CapQueued c = g_capq;
    g_capq = CapQueued{};
    c.done(g_off_lin, kOffH, c.arg);
}

bool capture_queue(const ntr::GxRecord &rec, uint32_t rear_argb, int ov_slot, OverlayMode mode,
                   void (*done)(const uint32_t *lin, int rows, void *arg), void *arg)
{
    if (!g_rec_up || g_capq.done || !off_target()) return false;
    resolve();   // the screen's own picture is finished first
    // the screen's state, put back for whatever the frame draws after this
    const C3D_Mtx *const proj = g_cur_proj;
    const int kx = g_tgt_kx, ky = g_tgt_ky, tw = g_tgt_w, th = g_tgt_h;
    const bool drawn = g_rec_drawn;
    float eye[4];
    memcpy(eye, g_eye_vec, sizeof eye);
    // Alpha clears to 0: what is still 0 afterwards was not drawn (the
    // overlay's behind-the-3D pixels read it).
    C3D_RenderTargetClear(g_off, C3D_CLEAR_ALL, 0x00000000, 0);
    C3D_FrameDrawOn(g_off);
    g_tgt_w = kOffW;
    g_tgt_h = kOffH;
    g_tgt_kx = g_tgt_ky = 1;
    g_cur_proj = &g_off_proj;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_loc_proj, &g_off_proj);
    stereo_eye_vec(0.0f);
    g_rec_drawn = false;   // the screen's record, again (its upload is kept)
    const Viewport vp = readback_vp();
    draw_rear_plane(rear_argb, vp);
    const bool ok = draw_record(rec, vp);
    if (ok) overlay_draw(ov_slot, mode, vp);
    g_cur_proj = proj;
    g_tgt_kx = kx;
    g_tgt_ky = ky;
    g_tgt_w = tw;
    g_tgt_h = th;
    g_rec_drawn = drawn;
    memcpy(g_eye_vec, eye, sizeof eye);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_loc_proj, g_cur_proj);
    if (!ok) return false;
    // Inside the frame this only queues the copy, after the drawing above.
    C3D_SyncDisplayTransfer((u32 *)g_off->frameBuf.colorBuf, GX_BUFFER_DIM(kOffH, kOffW),
                            g_off_lin, GX_BUFFER_DIM(kOffH, kOffW),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                                GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    g_off_done[0] = g_off_done[1] = g_off_done[2] = g_off_done[3] = 0;   // not readback's now
    g_capq.done = done;
    g_capq.arg = arg;
    return true;
}

const uint32_t *readback_area(int x0, int y0, int x1, int y1)
{
    if (!g_off_lin) return nullptr;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > kOffW) x1 = kOffW;
    if (y1 > kOffH) y1 = kOffH;
    int *d = g_off_done;
    if (x0 >= d[0] && y0 >= d[1] && x1 <= d[2] && y1 <= d[3]) return g_off_argb;
    // Un-rotate like screenshot_bmp: picture column x is buffer row x, picture row
    // y is buffer column H - 1 - y.
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const u32 c = g_off_lin[(size_t)x * kOffH + (kOffH - 1 - y)];
            g_off_argb[(size_t)y * kOffW + x] = (c >> 8) | (c << 24);
        }
    // what is made now (any of the old area outside it is made again if asked)
    d[0] = x0;
    d[1] = y0;
    d[2] = x1;
    d[3] = y1;
    return g_off_argb;
}

void set_toon(int shade, const float *rgb)
{
    g_toon_shade = rgb ? shade : 0;
    if (!g_toon_shade) return;
    // the table as it is already (a level's stays put): nothing to write
    if (g_toon_up && !memcmp(g_toon_rgb, rgb, sizeof g_toon_rgb)) return;
    ahead_settle();   // the last frame may be reading the old table
    memcpy(g_toon_rgb, rgb, sizeof g_toon_rgb);
    if (!g_toon_up) {
        if (!C3D_TexInit(&g_toon_tex, 32, 8, GPU_RGBA8)) { g_toon_shade = 0; return; }
        C3D_TexSetFilter(&g_toon_tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&g_toon_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        g_toon_up = true;
    }
    u32 *dst = (u32 *)g_toon_tex.data;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 32; ++x) {
            auto c8 = [&](int k) {
                const float v = rgb[x * 3 + k];
                return (u32)(v < 0.0f ? 0 : (v > 255.0f ? 255 : (int)(v + 0.5f)));
            };
            dst[((u32)x >> 3) * 64 + morton8((u32)x & 7, (u32)y)] =
                (c8(0) << 24) | (c8(1) << 16) | (c8(2) << 8) | 0xFF;
        }
    C3D_TexFlush(&g_toon_tex);
}

void set_fog(const ntr::GxFog *fog, bool wdepth)
{
    g_fog_on = fog && fog->enabled && !fog->alpha_only;
    if (!g_fog_on) return;
    g_fog = *fog;
    g_fog_w = wdepth;
    if (!g_fog_up) {
        if (!C3D_TexInit(&g_fog_tex, 256, 8, GPU_RGBA8)) { g_fog_on = false; return; }
        C3D_TexSetFilter(&g_fog_tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&g_fog_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        u32 *dst = (u32 *)g_fog_tex.data;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 256; ++x) {
                const int den = x < 128 ? x : 128;
                const u32 a = (u32)((255 * (128 - den) + 64) / 128);
                dst[((u32)x >> 3) * 64 + morton8((u32)x & 7, (u32)y)] = 0xFFFFFF00u | a;
            }
        C3D_TexFlush(&g_fog_tex);
        g_fog_up = true;
    }
    // The record's fog ramp: gx_fog_density reaches FOG_TABLE entry n at depth
    // FOG_OFFSET + (n + 1) * step (step = 0x400 >> shift). Texel n is the
    // visibility there, linear between entries, clamped at both ends like
    // gx_fog_density.
    if (!g_fogd_up) {
        if (!C3D_TexInit(&g_fogd_tex, 32, 8, GPU_RGBA8)) return;
        C3D_TexSetFilter(&g_fogd_tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&g_fogd_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        g_fogd_up = true;
    }
    const int step = 0x400 >> (fog->shift & 15);
    // the ramp as it is already (a level's fog stays put): nothing to write
    static ntr::GxFog ramp_of;
    static bool ramp_ok;
    if (!ramp_ok || memcmp(&ramp_of, fog, sizeof ramp_of)) {
        ahead_settle();   // the last frame may be reading the old ramp
        u32 *dst = (u32 *)g_fogd_tex.data;
        for (int x = 0; x < 32; ++x) {
            int d = fog->offset + (x + 1) * (step ? step : 1);
            if (d > 0x7FFF) d = 0x7FFF;
            const int den = ntr::gx_fog_density(*fog, d);
            const int dc = den < 128 ? den : 128;
            const u32 a = (u32)((255 * (128 - dc) + 64) / 128);
            for (int y = 0; y < 8; ++y)
                dst[((u32)x >> 3) * 64 + morton8((u32)x & 7, (u32)y)] = 0xFFFFFF00u | a;
        }
        C3D_TexFlush(&g_fogd_tex);
        memcpy(&ramp_of, fog, sizeof ramp_of);
        ramp_ok = true;
    }
    // s = ((d15 - offset) / step - 1 + 0.5) / 32, d15 = w * 8 or Z * 0x7FFF
    const float k = 1.0f / (32.0f * (float)(step ? step : 1));
    g_fogk[0] = wdepth ? 8.0f * k : 0.0f;
    g_fogk[1] = wdepth ? 0.0f : 32767.0f * k;
    g_fogk[2] = -(float)fog->offset * k - 0.5f / 32.0f;
}

void frame_end()
{
    resolve();
    g_toon_shade = 0;
    g_fog_on = false;
    {
        PresentScope timer(P_VBO_FLUSH);
        if (g_used > g_vlo) GSPGPU_FlushDataCache(g_vbo + g_vlo, (g_used - g_vlo) * sizeof(Vtx));
    }
    ahead_flush();   // a frame built ahead: the GPU starts on it now
    // a capture in a frame built ahead: its GPU work is running now, so it is
    // finished while the frame waits for its vblank, not after (waiting on the
    // queue past C3D_FrameEnd waits for the screens' swap)
    if (g_was_ahead) capture_finish();
    if (g_frame_late) {   // built before its vblank: the waits SYNCDRAW skipped
        g_frame_late = false;
        frame_sync();
    }
    if (g_was_ahead) {   // its GPU work: done by its vblank (and how long it took), or not
        g_was_ahead = false;
        __dmb();
        const u64 end = g_queue_end;
        if (end > g_ahead_t_run) {
            g_gt.ahead_gpu += ticks_ms(end - g_ahead_t_run);
            ++g_gt.ahead_gpu_n;
        } else {
            ++g_gt.ahead_gpu_late;
        }
    }
    { PresentScope timer(P_FRAME_SUBMIT); C3D_FrameEnd(0); }
    capture_finish();   // (a frame that waited: its queue runs from C3D_FrameEnd)
    if (g_present_active && g_marks.frames > 0 && g_marks.frames <= 2)
        g_marks.submitted[g_marks.frames - 1] = marks_ms(svcGetSystemTick());
}

void present_timing_begin()
{
    g_present_zone = P_PREPARE;
    g_present_start = g_present_last = svcGetSystemTick();
    g_present_active = true;
    g_marks = PresentMarks{};
}

void present_timing_prepared()
{
    if (g_present_active) timing_switch(P_OTHER);
}

void present_job_begin(uint64_t prep_ticks)
{
    const u64 now = svcGetSystemTick();
    g_present.ticks[P_PREPARE] += prep_ticks;
    g_present_zone = P_OTHER;
    g_present_start = now - prep_ticks;
    g_present_last = now;
    g_present_active = true;
    g_marks = PresentMarks{};
}

void present_timing_end()
{
    if (!g_present_active) return;
    timing_switch(P_OTHER);
    g_marks.end = marks_ms(g_present_last);
    g_marks_done = g_marks;
    g_present.total += g_present_last - g_present_start;
    ++g_present.frames;
    g_present.tris_in += g_stats.tris_in;
    g_present.tris_drawn += g_stats.tris_drawn;
    g_present.draw_calls += g_stats.draw_calls;
    if (g_stats.tris_drawn > g_present.tris_max) g_present.tris_max = g_stats.tris_drawn;
    if (g_stats.draw_calls > g_present.draws_max) g_present.draws_max = g_stats.draw_calls;
    g_present_active = false;

    static u64 prev_end;
    if (prev_end) {
        const u64 span = g_present_last - prev_end;
        const u64 vblank = SYSCLOCK_ARM11 * 1000ull / 59831;   // 59.831 Hz
        u64 n = (span + vblank / 2) / vblank;
        if (n < 1) n = 1;
        ++g_present.pace_vbl[n > 5 ? 4 : n - 1];
        ++g_present.pace_frames;
        g_present.pace_span += span;
        if (span > g_present.pace_worst) g_present.pace_worst = span;
    }
    prev_end = g_present_last;
}

PresentMarks take_present_marks()
{
    return g_marks_done;
}

PresentTiming take_present_timing()
{
    const PresentTiming out = g_present;
    g_present = PresentTiming{};
    return out;
}

// Keep citro3d's SYNCDRAW path as is. FrameBegin waits on its frame-sync
// counters, then gxCmdQueueWait; AnyEvent calls under that queue wait are
// counted as queue time, not vblank pacing.
extern "C" bool __real_gxCmdQueueWait(gxCmdQueue_s *, s64);
extern "C" GSPGPU_Event __real_gspWaitForAnyEvent(void);
extern "C" bool __wrap_gxCmdQueueWait(gxCmdQueue_s *queue, s64 timeout)
{
    // a frame built ahead begins at once: the last frame's queue runs on
    // (ahead_flush waits for it)
    if (g_ahead_begin) return true;
    PresentScope timer(P_GPU_QUEUE_WAIT);
    return __real_gxCmdQueueWait(queue, timeout);
}
// ...and C3D_FrameBegin neither stops nor clears that queue then.
extern "C" void __real_gxCmdQueueStop(gxCmdQueue_s *);
extern "C" void __real_gxCmdQueueClear(gxCmdQueue_s *);
extern "C" void __wrap_gxCmdQueueStop(gxCmdQueue_s *queue)
{
    if (!g_ahead_begin) __real_gxCmdQueueStop(queue);
}
extern "C" void __wrap_gxCmdQueueClear(gxCmdQueue_s *queue)
{
    if (!g_ahead_begin) __real_gxCmdQueueClear(queue);
}
// Waiting on the GPU inside a frame built ahead (e.g. a sync transfer) would
// wait on entries we're still holding, so hand the frame over first.
extern "C" void __real_gspWaitForEvent(GSPGPU_Event, bool);
extern "C" void __wrap_gspWaitForEvent(GSPGPU_Event id, bool next)
{
    if (g_ahead && id != GSPGPU_EVENT_VBlank0 && id != GSPGPU_EVENT_VBlank1 &&
        current_thread_id() == g_ahead_tid)
        ahead_settle();
    __real_gspWaitForEvent(id, next);
}
extern "C" GSPGPU_Event __wrap_gspWaitForAnyEvent(void)
{
    if (g_present_active && g_present_zone == P_BEGIN_CPU) {
        PresentScope timer(P_VBLANK_WAIT);
        return __real_gspWaitForAnyEvent();
    }
    return __real_gspWaitForAnyEvent();
}

// citro3d's vblank counters, hooked (vblank_hook0/1, for frame_sync): C3D_Init
// registers onVBlank0/1 here, C3D_Fini clears them. Any other event passes.
extern "C" void __real_gspSetEventCallback(GSPGPU_Event, ThreadFunc, void *, bool);
extern "C" void __wrap_gspSetEventCallback(GSPGPU_Event id, ThreadFunc cb, void *data, bool oneShot)
{
    if ((id == GSPGPU_EVENT_VBlank0 || id == GSPGPU_EVENT_VBlank1) && !oneShot) {
        const int i = id == GSPGPU_EVENT_VBlank1;
        g_vbl_cb[i] = cb;
        g_vbl_data[i] = data;
        if (cb) {
            __real_gspSetEventCallback(id, i ? vblank_hook1 : vblank_hook0, nullptr, false);
            return;
        }
    }
    __real_gspSetEventCallback(id, cb, data, oneShot);
}

bool screenshot_bmp(const char *path, Screen s)
{
    const int fsw = s == SCREEN_BOTTOM ? kBotW : kTopW;
    u16 fw = 0, fh = 0;   // the framebuffer is stored rotated: 240 x width
    const u8 *fb = gfxGetFramebuffer(s == SCREEN_BOTTOM ? GFX_BOTTOM : GFX_TOP, GFX_LEFT, &fw, &fh);
    if (!fb || fw != kTopH || fh != fsw) return false;
    // under 3D the top screen's two eyes side by side, the left eye's first
    const u8 *fb_r = s == SCREEN_TOP && g_stereo ? gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &fw, &fh) : nullptr;
    const int sw = fb_r ? fsw * 2 : fsw;
    // Build the whole file in memory and write once; row by row was 240 SD writes
    // per screenshot.
    const int row = sw * 3, size = 54 + row * kTopH;
    static std::vector<u8> img;
    img.assign((size_t)size, 0);
    u8 *h = img.data();
    h[0] = 'B';
    h[1] = 'M';
    auto put32 = [&](int off, u32 v) { memcpy(h + off, &v, 4); };
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, sw); put32(22, kTopH);
    h[26] = 1; h[28] = 24; put32(34, row * kTopH);
    for (int y = kTopH - 1; y >= 0; --y) {           // BMP rows bottom-up
        u8 *line = h + 54 + (size_t)(kTopH - 1 - y) * row;
        for (int x = 0; x < sw; ++x) {
            // column x of the screen is framebuffer row x; screen y counts
            // down from the top, the framebuffer column counts up from it.
            const u8 *const src = x < fsw ? fb : fb_r;
            const int fx = x < fsw ? x : x - fsw;
            const u8 *p = src + ((size_t)fx * kTopH + (kTopH - 1 - y)) * 3;
            line[x * 3 + 0] = p[0];   // B
            line[x * 3 + 1] = p[1];   // G
            line[x * 3 + 2] = p[2];   // R
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = fwrite(h, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    return ok;
}

const Stats &stats() { return g_stats; }

}  // namespace gpu3ds
