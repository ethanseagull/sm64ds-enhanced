// Options menu, opened with SELECT. The DS's own SELECT menu (pad vs touch
// control style) doesn't make sense on 3DS, so the game never sees SELECT
// (input3ds.cpp) and it opens this instead. The game pauses while it's open
// (the top screen holds its last frame, music keeps playing), and the bottom
// screen shows four switches:
//   60 FPS         in-between frames (interp3ds); off means levels run at 30
//   ANTI-ALIASING  top screen 3D drawn at 2x width and averaged down; off by
//                  default
//   FPS COUNTER    frames drawn per second, bottom-left of the top screen
//   ANALOG         on: the Circle Pad works as a stick and push distance sets
//                  speed; off: the DS controls, Circle Pad as a D-pad, Y to run
// Anti-aliasing is saved as anti_aliasing (an old antialiasing line is ignored).
// GAME3DS_FSAA in env.txt also turns it on, =2 for 2x both ways.
//
// Up/down picks, A (or left/right, or a tap) toggles, B, SELECT or START (or
// tapping BACK) closes. Changes apply when the game resumes. Choices are saved
// to sdmc:/sm64ds3ds/options.txt and loaded at boot over env.txt. Until the
// player saves any, the build's own options.txt applies: romfs:/options.txt,
// the same lines, written into the .cia by the builder's settings (with the
// console it was built for).
//
// Stereo 3D follows the 3D slider rather than a switch. With it up the top
// screen is drawn once per eye and anti-aliasing is paused; 60 fps stays on.
// GAME3DS_STEREO=0 keeps 3D off.
//
// The art comes from the PC settings page (hal/settings_art_data.cpp) and its
// font (hal/ui_text.cpp), built once (art_make).
//
// For tests: GAME3DS_OPTIONS_AT=N opens it at the N-th button read, and
// GAME3DS_OPTIONS_SCRIPT=TOKEN,... plays one token per menu frame (UP, DOWN, A,
// B, WAIT, SHOT), since the regression harness can't press SELECT in Azahar.
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "card3ds.h"
#include "hal/ui_text.h"
#include "third_party/stb/stb_image.h"

extern "C" int port_gpu_raster_active(void);   // card3ds.cpp: the card is up
extern "C" void game3ds_watchdog_away(int away);   // game/watchdog3ds.cpp
extern "C" void game3ds_lid_plain(void);           // game/lid3ds.cpp
extern "C" const unsigned char g_setart_png_parchment[], g_setart_png_title[], g_setart_png_back[],
    g_setart_png_key[], g_setart_png_plank[];
extern "C" const unsigned int g_setart_png_parchment_len, g_setart_png_title_len,
    g_setart_png_back_len, g_setart_png_key_len, g_setart_png_plank_len;

namespace {

const char kPath[] = "sdmc:/sm64ds3ds/options.txt";

struct Options {
    bool fps60, fsaa, counter, analog;
} g_opt;

bool env_on(const char *k)
{
    const char *e = getenv(k);
    return e && atoi(e) != 0;
}

// ---- the FPS counter's picture (card3ds draws it) ---------------------------

// "60 FPS" on a dark rounded plate: green at full speed, yellow from 28,
// red under it.
void counter_art(unsigned fps, uint32_t *px, int stride, int *w, int *h)
{
    char text[16];
    snprintf(text, sizeof text, "%u FPS", fps);
    const float em = 14.0f;
    int W = (int)ceilf(ui_text_width(text, em)) + 10;
    const int H = 16;
    if (W > 128) W = 128;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            // the plate's corners cut by a radius of 3
            const int dx = x < 3 ? 3 - x : x > W - 4 ? x - (W - 4) : 0;
            const int dy = y < 3 ? 3 - y : y > H - 4 ? y - (H - 4) : 0;
            px[(size_t)y * stride + x] = dx * dx + dy * dy > 9 ? 0u : 0xB0000000u;
        }
    const uint32_t rgb = fps >= 55 ? 0x8CFF8Cu : fps >= 28 ? 0xFFE066u : 0xFF6A5Au;
    UiSurface s = {px, stride, W, H};
    ui_text_draw(&s, 5.0f, 13.0f, text, em, rgb, 255, nullptr);
    *w = W;
    *h = H;
}

// counter_art runs on the present worker (core 2), and ui_text's glyph cache
// isn't thread-safe, so make the glyphs here on the game thread while the
// worker is idle; the worker only looks them up.
void counter_glyphs()
{
    static uint32_t scratch[128 * 16];
    const UiSurface s = {scratch, 128, 128, 16};
    ui_text_draw(&s, 0.0f, 13.0f, "0123456789 FPS", 14.0f, 0xFFFFFFu, 255, nullptr);
}

bool g_stereo;          // 3D as apply() last asked for it
bool g_stereo_failed;   // its target could not be made: not asked again

void apply()
{
    if (g_opt.counter) {
        card3ds_wait_idle();
        counter_glyphs();
    }
    const bool st = card3ds_set_options(g_opt.fps60, g_stereo, g_opt.fsaa, g_opt.counter ? counter_art : nullptr);
    if (g_stereo && !st) {
        g_stereo_failed = true;
        g_stereo = false;
        apply();
    }
}

// The slider: up (a little past the bottom, so a slider resting at its edge
// does not flicker it), 3D on; at the bottom, off. True when it changed.
bool stereo_check()
{
    static const bool never = getenv("GAME3DS_STEREO") && atoi(getenv("GAME3DS_STEREO")) == 0;
    if (never || g_stereo_failed) return false;
    const float k = card3ds_stereo_slider();
    const bool want = g_stereo ? k > 0.0f : k >= 0.03f;
    if (want == g_stereo) return false;
    g_stereo = want;
    printf("[options] 3D slider %.2f: 3D %s\n", k, want ? "on (anti-aliasing waits)" : "off");
    apply();
    return true;
}

void save()
{
    FILE *f = fopen(kPath, "w");
    if (!f) {
        printf("[options] %s could not be written\n", kPath);
        return;
    }
    fprintf(f, "# The options menu's choices (SELECT), read at boot over env.txt's.\n"
               "60fps=%d\nanti_aliasing=%d\nfps_counter=%d\nanalog_controls=%d\n",
            g_opt.fps60 ? 1 : 0, g_opt.fsaa ? 1 : 0, g_opt.counter ? 1 : 0, g_opt.analog ? 1 : 0);
    fclose(f);
    printf("[options] saved to %s\n", kPath);
}

void print_analog()
{
    printf("[options] analog %s\n", g_opt.analog ? "on (the Circle Pad a stick)"
                                                 : "off (the Circle Pad a D-pad, Y runs)");
}

// ---- the art -----------------------------------------------------------------

struct Img {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // premultiplied 0xAARRGGBB
};

Img decode(const unsigned char *png, unsigned len)
{
    Img I;
    int w = 0, h = 0, comp = 0;
    unsigned char *p = stbi_load_from_memory(png, (int)len, &w, &h, &comp, 4);
    if (!p) return I;
    I.w = w;
    I.h = h;
    I.px.resize((size_t)w * h);
    for (int i = 0; i < w * h; ++i) {
        const unsigned r = p[i * 4], g = p[i * 4 + 1], b = p[i * 4 + 2], a = p[i * 4 + 3];
        I.px[i] = (a << 24) | (((r * a + 127) / 255) << 16) | (((g * a + 127) / 255) << 8) |
                  ((b * a + 127) / 255);
    }
    stbi_image_free(p);
    return I;
}

// Along one axis of an area average from sn pixels down to dn: each output
// pixel's first source pixel and the weights of those it covers (summing to 1).
struct Taps {
    int per;                 // weights kept per output pixel
    std::vector<int> first, n;
    std::vector<float> w;
};

Taps taps_for(int sn, int dn)
{
    Taps t;
    const double k = (double)sn / dn;
    t.per = (int)ceil(k) + 1;
    t.first.resize(dn);
    t.n.resize(dn);
    t.w.assign((size_t)dn * t.per, 0.0f);
    for (int d = 0; d < dn; ++d) {
        const double a = d * k, b = a + k;
        t.first[d] = (int)a;
        int n = 0;
        for (int s = (int)a; s < sn && s < b && n < t.per; ++s, ++n)
            t.w[(size_t)d * t.per + n] = (float)((fmin(b, s + 1.0) - fmax(a, (double)s)) / k);
        t.n[d] = n;
    }
    return t;
}

// An area average down to W x H (every use is a shrink): rows, then columns.
Img scaled(const Img &s, int W, int H)
{
    Img o;
    if (!s.w || !s.h) return o;
    const Taps tx = taps_for(s.w, W), ty = taps_for(s.h, H);
    std::vector<float> row((size_t)s.w * 4), mid((size_t)W * s.h * 4);
    for (int y = 0; y < s.h; ++y) {
        const uint32_t *src = &s.px[(size_t)y * s.w];
        for (int x = 0; x < s.w; ++x) {
            const uint32_t c = src[x];
            row[x * 4] = (float)(c >> 24);
            row[x * 4 + 1] = (float)(c >> 16 & 0xFF);
            row[x * 4 + 2] = (float)(c >> 8 & 0xFF);
            row[x * 4 + 3] = (float)(c & 0xFF);
        }
        float *m = &mid[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x, m += 4) {
            const float *w = &tx.w[(size_t)x * tx.per];
            const float *r = &row[(size_t)tx.first[x] * 4];
            float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            for (int i = 0; i < tx.n[x]; ++i, r += 4) {
                a0 += w[i] * r[0];
                a1 += w[i] * r[1];
                a2 += w[i] * r[2];
                a3 += w[i] * r[3];
            }
            m[0] = a0;
            m[1] = a1;
            m[2] = a2;
            m[3] = a3;
        }
    }
    o.w = W;
    o.h = H;
    o.px.resize((size_t)W * H);
    auto byte = [](float v) { const int i = (int)(v + 0.5f); return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i); };
    for (int y = 0; y < H; ++y) {
        const float *w = &ty.w[(size_t)y * ty.per];
        for (int x = 0; x < W; ++x) {
            const float *m = &mid[((size_t)ty.first[y] * W + x) * 4];
            float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            for (int i = 0; i < ty.n[y]; ++i, m += (size_t)W * 4) {
                a0 += w[i] * m[0];
                a1 += w[i] * m[1];
                a2 += w[i] * m[2];
                a3 += w[i] * m[3];
            }
            o.px[(size_t)y * W + x] = byte(a0) << 24 | byte(a1) << 16 | byte(a2) << 8 | byte(a3);
        }
    }
    return o;
}

// The layout on the bottom screen's 320 x 240.
constexpr int kW = 320, kH = 240;
constexpr int kRows = 4;
constexpr int kTitleX = 70, kTitleY = 4, kTitleW = 180, kTitleH = 48;
constexpr int kRowX = 12, kRowY0 = 54, kRowStep = 36, kRowW = 296, kRowH = 32;
constexpr int kKeyW = 56, kKeyH = 26;
constexpr int kBackX = 240, kBackY = 196, kBackW = 72, kBackH = 41;

uint32_t scale_rgb(uint32_t c, float k)
{
    const int r = (int)((c >> 16 & 0xFF) * k), g = (int)((c >> 8 & 0xFF) * k), b = (int)((c & 0xFF) * k);
    return (c & 0xFF000000u) | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 |
           (uint32_t)(b > 255 ? 255 : b);
}

// premultiplied src over an opaque kW x kH picture, its colour times `k`
void blit_on(uint32_t *dst, const Img &s, int x0, int y0, float k = 1.0f)
{
    for (int y = 0; y < s.h; ++y) {
        const int yy = y0 + y;
        if (yy < 0 || yy >= kH) continue;
        for (int x = 0; x < s.w; ++x) {
            const int xx = x0 + x;
            if (xx < 0 || xx >= kW) continue;
            uint32_t c = s.px[(size_t)y * s.w + x];
            const unsigned a = c >> 24;
            if (!a) continue;
            if (k != 1.0f) c = scale_rgb(c, k);
            uint32_t &d = dst[(size_t)yy * kW + xx];
            const unsigned ia = 255 - a;
            const unsigned r = (c >> 16 & 0xFF) + ((d >> 16 & 0xFF) * ia + 127) / 255;
            const unsigned g = (c >> 8 & 0xFF) + ((d >> 8 & 0xFF) * ia + 127) / 255;
            const unsigned b = (c & 0xFF) + ((d & 0xFF) * ia + 127) / 255;
            d = 0xFF000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
    }
}

// The art, made once: the background (parchment with darkened edges, the
// SETTINGS plaque and BACK) plus a plank and key plate for the rows. On New 3DS
// a lowest-priority thread on core 2 builds it from the first button read
// (art_start), saving ~0.3 s the first menu would otherwise freeze for.
// art_wait waits for it (or builds it here without the thread).
struct Art {
    int state;   // 0 not made, 1 made, -1 failed (flat colours instead)
    Img bg, plank, key;
    double decode_ms, scale_ms;
} g_art;
Thread g_art_thread;

void art_make()
{
    if (g_art.state) return;
    const u64 t0 = svcGetSystemTick();
    Img parch = decode(g_setart_png_parchment, g_setart_png_parchment_len);
    Img title = decode(g_setart_png_title, g_setart_png_title_len);
    Img plank = decode(g_setart_png_plank, g_setart_png_plank_len);
    Img key = decode(g_setart_png_key, g_setart_png_key_len);
    Img back = decode(g_setart_png_back, g_setart_png_back_len);
    if (parch.w < kW || parch.h < kH || !title.w || !plank.w || !key.w || !back.w) {
        g_art.state = -1;
        return;
    }
    const u64 t1 = svcGetSystemTick();
    Img &bg = g_art.bg;
    bg.w = kW;
    bg.h = kH;
    bg.px.resize((size_t)kW * kH);
    const int ox = (parch.w - kW) / 2, oy = (parch.h - kH) / 2;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const int e = std::min(std::min(x, kW - 1 - x), std::min(y, kH - 1 - y));
            const uint32_t c = parch.px[(size_t)(y + oy) * parch.w + x + ox];
            bg.px[(size_t)y * kW + x] = e < 16 ? scale_rgb(c, 0.80f + 0.20f * e / 16.0f) : c;
        }
    blit_on(bg.px.data(), scaled(title, kTitleW, kTitleH), kTitleX, kTitleY);
    blit_on(bg.px.data(), scaled(back, kBackW, kBackH), kBackX, kBackY);
    g_art.plank = scaled(plank, kRowW, kRowH);
    g_art.key = scaled(key, kKeyW, kKeyH);
    g_art.decode_ms = (double)(t1 - t0) * 1000.0 / SYSCLOCK_ARM11;
    g_art.scale_ms = (double)(svcGetSystemTick() - t1) * 1000.0 / SYSCLOCK_ARM11;
    g_art.state = 1;
}

void art_start()
{
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    if (n3ds) g_art_thread = threadCreate([](void *) { art_make(); }, nullptr, 64 * 1024, 0x3F, 2, false);
}

void art_wait()
{
    const u64 t0 = svcGetSystemTick();
    const bool threaded = g_art_thread != nullptr;
    if (threaded) {
        threadJoin(g_art_thread, U64_MAX);
        threadFree(g_art_thread);
        g_art_thread = nullptr;
    }
    const bool fresh = g_art.state == 0 || threaded;
    art_make();
    if (!fresh) return;
    if (g_art.state < 0)
        printf("[options] the menu's art did not decode: flat colours\n");
    else
        printf("[options] the menu's art: decoded in %.1f ms, scaled in %.1f ms (%s); waited %.1f ms for it\n",
               g_art.decode_ms, g_art.scale_ms, threaded ? "on core 2" : "here",
               (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11);
}

uint32_t g_canvas[kW * kH];

void blit(const Img &s, int x0, int y0, float k = 1.0f)
{
    blit_on(g_canvas, s, x0, y0, k);
}

void fill(int x0, int y0, int w, int h, uint32_t rgb)
{
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            if (x >= 0 && x < kW && y >= 0 && y < kH) g_canvas[(size_t)y * kW + x] = 0xFF000000u | rgb;
}

// a frame `t` pixels thick just outside the rectangle, its corners cut
void outline(int x0, int y0, int w, int h, int t, uint32_t rgb)
{
    for (int i = 1; i <= t; ++i) {
        fill(x0 - i + 1, y0 - i, w + 2 * i - 2, 1, rgb);
        fill(x0 - i + 1, y0 + h + i - 1, w + 2 * i - 2, 1, rgb);
        fill(x0 - i, y0 - i + 1, 1, h + 2 * i - 2, rgb);
        fill(x0 + w + i - 1, y0 - i + 1, 1, h + 2 * i - 2, rgb);
    }
}

void text(float x, float baseline, const char *s, float em, uint32_t rgb, uint32_t shadow = 0xFFFFFFFFu)
{
    UiSurface surf = {g_canvas, kW, kW, kH};
    if (shadow != 0xFFFFFFFFu) ui_text_draw(&surf, x + 1.0f, baseline + 1.0f, s, em, shadow, 255, nullptr);
    ui_text_draw(&surf, x, baseline, s, em, rgb, 255, nullptr);
}

void text_centred(float cx, float baseline, const char *s, float em, uint32_t rgb)
{
    text(cx - ui_text_width(s, em) * 0.5f, baseline, s, em, rgb);
}

const char *const kLabels[kRows] = {"60 FPS", "ANTI-ALIASING", "FPS COUNTER", "ANALOG"};
constexpr int kFsaaRow = 1, kAnalogRow = 3;

bool *row_value(int i)
{
    return i == 0 ? &g_opt.fps60 : i == 1 ? &g_opt.fsaa : i == 2 ? &g_opt.counter : &g_opt.analog;
}

int row_y(int i)
{
    return kRowY0 + i * kRowStep;
}

void compose(int cur)
{
    const bool art = g_art.state > 0;
    if (art) {
        memcpy(g_canvas, g_art.bg.px.data(), sizeof g_canvas);
    } else {
        fill(0, 0, kW, kH, 0xF2DFA0u);
        text_centred(kW * 0.5f, 40.0f, "SETTINGS", 30.0f, 0x6B3A12u);
        fill(kBackX, kBackY, kBackW, kBackH, 0xE79A3Au);
        text_centred(kBackX + kBackW * 0.5f, kBackY + 28.0f, "BACK", 18.0f, 0x6B3A12u);
    }
    for (int i = 0; i < kRows; ++i) {
        const int y = row_y(i);
        const bool sel = i == cur;
        if (art) {
            blit(g_art.plank, kRowX, y, sel ? 1.08f : 0.74f);
        } else {
            fill(kRowX, y, kRowW, kRowH, sel ? 0xE79A3Au : 0xB0702Au);
        }
        if (sel) outline(kRowX, y, kRowW, kRowH, 2, 0xFFF3B0u);
        text((float)kRowX + 14.0f, (float)y + 22.0f, kLabels[i], 15.0f, sel ? 0xFFF8E6u : 0xE6D6B8u,
             0x4A2508u);
        // the value on the key plate at the row's right
        const int kx = kRowX + kRowW - 8 - kKeyW, ky = y + (kRowH - kKeyH) / 2;
        if (art)
            blit(g_art.key, kx, ky, sel ? 1.0f : 0.86f);
        else
            fill(kx, ky, kKeyW, kKeyH, 0xFFF0C8u);
        const bool on = *row_value(i);
        // under 3D anti-aliasing waits: its choice dimmed
        const bool held = g_stereo && i == kFsaaRow;
        text_centred((float)kx + kKeyW * 0.5f, (float)ky + 19.0f, on ? "ON" : "OFF", 15.0f,
                     held ? 0x8C7B62u : on ? 0x1F7A1Fu : 0x8B2B12u);
    }
    if (g_stereo) {
        const char *const note = "3D IS ON: ANTI-ALIASING WAITS";
        const float w = ui_text_width(note, 11.0f);
        text(16.0f, 206.0f, note, w > 216.0f ? 11.0f * 216.0f / w : 11.0f, 0x6B3A12u);
    }
    text(16.0f, 222.0f, "A: CHANGE    B: BACK", 13.0f, 0x6B3A12u);
}

// ---- the script (tests) --------------------------------------------------------

const char *g_script;   // the next token, or null

// the next token's keys; `shot` set for SHOT
u32 script_keys(bool *shot)
{
    *shot = false;
    if (!g_script) return 0;
    if (!*g_script) {
        g_script = nullptr;
        return 0;
    }
    const char *end = strchr(g_script, ',');
    const size_t n = end ? (size_t)(end - g_script) : strlen(g_script);
    auto is = [&](const char *t) { return strlen(t) == n && !strncmp(g_script, t, n); };
    u32 k = 0;
    if (is("UP")) k = KEY_DUP;
    else if (is("DOWN")) k = KEY_DDOWN;
    else if (is("A")) k = KEY_A;
    else if (is("B")) k = KEY_B;
    else if (is("SHOT")) *shot = true;
    g_script = end ? end + 1 : nullptr;
    return k;
}

void shot()
{
    static int n;
    // the frame's transfer and swap first (seq_shot in card3ds.cpp)
    gspWaitForVBlank();
    gspWaitForVBlank();
    char path[64];
    snprintf(path, sizeof path, "sdmc:/sm64ds3ds/game3ds_options_%d.bmp", n++);
    card3ds_screenshot(path, true);
    printf("[options] screenshot %s\n", path);
}

int run_menu(bool scripted)
{
    // The last tick's present may still be running on core 2 (the counter's
    // picture among it): it ends before the menu's text touches ui_text.
    card3ds_wait_idle();
    art_wait();
    const Options was = g_opt;
    int cur = 0;
    bool dirty = true;
    printf("[options] menu open%s\n", scripted ? " (GAME3DS_OPTIONS_SCRIPT)" : "");
    game3ds_watchdog_away(1);   // the game waits here: not a hang
    game3ds_lid_plain();        // no lid sounds in here: a closed lid sleeps at once
    for (;;) {
        if (!aptMainLoop()) break;
        hidScanInput();
        if (stereo_check()) dirty = true;
        bool want_shot = false;
        u32 down = hidKeysDown();
        if (scripted) {
            if (!g_script) break;   // the script played out: closed
            down = script_keys(&want_shot);
        }
        bool close = false, flip = false;
        if (down & KEY_UP) cur = (cur + kRows - 1) % kRows;
        if (down & KEY_DOWN) cur = (cur + 1) % kRows;
        if (down & (KEY_A | KEY_LEFT | KEY_RIGHT)) flip = true;
        if (down & (KEY_B | KEY_SELECT | KEY_START)) close = true;
        if (down & KEY_TOUCH) {
            touchPosition t;
            hidTouchRead(&t);
            for (int i = 0; i < kRows; ++i)
                if (t.px >= kRowX && t.px < kRowX + kRowW && t.py >= row_y(i) && t.py < row_y(i) + kRowH) {
                    cur = i;
                    flip = true;
                }
            if (t.px >= kBackX && t.px < kBackX + kBackW && t.py >= kBackY && t.py < kBackY + kBackH)
                close = true;
        }
        if (down & (KEY_UP | KEY_DOWN)) dirty = true;
        if (flip) {
            *row_value(cur) = !*row_value(cur);
            // the controls are read each frame (game3ds_options_analog)
            if (cur == kAnalogRow)
                print_analog();
            else
                apply();
            dirty = true;
        }
        if (close) break;
        if (dirty) {
            compose(cur);
            dirty = false;
        }
        // every frame (the HOME menu may have taken the screen meanwhile)
        card3ds_show_bottom(g_canvas, kW);
        if (want_shot) shot();
    }
    card3ds_options_closed();
    game3ds_watchdog_away(0);
    if (g_opt.fps60 != was.fps60 || g_opt.fsaa != was.fsaa || g_opt.counter != was.counter ||
        g_opt.analog != was.analog)
        save();
    printf("[options] menu closed\n");
    return 1;
}

}  // namespace

// One options file's lines over the environment. "On" keeps an env.txt value.
// Old antialiasing lines are ignored; anti_aliasing is the current one. The
// build's own options.txt also says which console the build was made for
// (console=new or old), which only goes in the log: the game sizes itself to
// the console it runs on either way.
static void read_options(const char *path)
{
    if (FILE *f = fopen(path, "r")) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            if (!strcmp(line, "console")) {
                const bool old = !strncmp(eq + 1, "old", 3);
                bool n3ds = false;
                APT_CheckNew3DS(&n3ds);
                printf("[options] built for %s; running on %s\n", old ? "an Old 3DS" : "a New 3DS",
                       n3ds ? "a New 3DS" : "an Old 3DS");
                continue;
            }
            const bool on = atoi(eq + 1) != 0;
            const char *env = !strcmp(line, "60fps")             ? "GAME3DS_INTERP"
                              : !strcmp(line, "anti_aliasing")   ? "GAME3DS_FSAA"
                              : !strcmp(line, "fps_counter")     ? "GAME3DS_FPS_COUNTER"
                              : !strcmp(line, "analog_controls") ? "GAME3DS_ANALOG"
                                                                 : nullptr;
            if (!env) continue;
            if (!on)
                setenv(env, "0", 1);
            else if (!env_on(env))
                setenv(env, "1", 1);
            printf("[options] %s=%s (%s)\n", env, getenv(env), path);
        }
        fclose(f);
    }
}

// After env.txt is loaded and before anything reads these: the build's
// options.txt (the builder's settings), then the player's saved one over it.
extern "C" void game3ds_options_load(void)
{
    read_options("romfs:/options.txt");
    read_options(kPath);
    g_opt.fps60 = env_on("GAME3DS_INTERP");
    g_opt.fsaa = env_on("GAME3DS_FSAA");
    g_opt.counter = env_on("GAME3DS_FPS_COUNTER");
    // analog unless GAME3DS_ANALOG=0
    g_opt.analog = !getenv("GAME3DS_ANALOG") || env_on("GAME3DS_ANALOG");
    print_analog();
}

// The ANALOG switch, read each frame: 1 analog, 0 traditional.
extern "C" int game3ds_options_analog(void)
{
    return g_opt.analog ? 1 : 0;
}

// Called on each button read, after hidScanInput. The first call once the card
// is up hands it the FPS counter; SELECT (or GAME3DS_OPTIONS_AT) opens the
// menu. Returns 1 if the menu ran and the buttons should be read again.
extern "C" int game3ds_options_poll(void)
{
    if (!port_gpu_raster_active()) return 0;
    static bool applied;
    if (!applied) {
        applied = true;
        if (g_opt.counter) ui_text_ready();   // its font here, not inside a present on core 2
        if (g_opt.counter || g_opt.fsaa) apply();
        art_start();
    }
    stereo_check();
    static int reads;
    static const int at = getenv("GAME3DS_OPTIONS_AT") ? atoi(getenv("GAME3DS_OPTIONS_AT")) : 0;
    const bool scripted = at > 0 && ++reads == at;
    if (scripted) g_script = getenv("GAME3DS_OPTIONS_SCRIPT") ? getenv("GAME3DS_OPTIONS_SCRIPT") : "";
    if (!scripted && !(hidKeysDown() & KEY_SELECT)) return 0;
    return run_menu(scripted);
}
