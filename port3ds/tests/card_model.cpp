// Test of ntr/gx.h's card protocol on 3DS.
//
// Same model pipeline as view_model.cpp, but each frame runs the way the HAL's
// scene loop does: gx_reset, the render walk, gx_card_present_allow +
// gx_clear_fill, gx_render (which hands the frame to card3ds), then 2D written
// into the framebuffer, then card3ds_present. The 2D is a fake HUD: a front
// panel that must cover Mario and a behind bar that must only show around him.
//
// Screenshots: sdmc:/sm64ds3ds/card_model.bmp (no fade) and
// card_model_fade.bmp (faded to black at evy 8).
//
// The model is loaded through the game's own code (SharedFilePtr,
// Model::LoadFile, DoSetFile) and pushed through ntr/gx.cpp every frame by
// Model::Render, spinning on Y. After 90 frames the top screen is saved and
// the run reports RESULT; it keeps spinning until START.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Heap.h"
#include "Model.h"
#include "types.h"

#include "ntr/gx.h"
#include "ntr/mmio.h"
#include "ntr/ppu.h"

#include "card3ds.h"
#include "gpu3ds.h"
#include "hal/gpu_raster.h"
#include "host3ds.h"

extern "C" {
void _ZN5ModelC1Ev(void *self);
struct SharedFilePtrC { u16 fileID; u8 numRefs; void *filePtr; };
SharedFilePtrC *_ZN13SharedFilePtr9ConstructEj(SharedFilePtrC *self, u32 ov0FileID);
extern Matrix4x3 data_0209b3ec;     /* camera matrix Model::Render composes */
}

namespace {

void reset_scene()
{
    ntr::gx_reset();
    NTR_MMIO(uint32_t, 0x04000440) = 0;   /* projection = identity */
    NTR_MMIO(uint32_t, 0x04000454) = 0;
    NTR_MMIO(uint32_t, 0x04000440) = 1;   /* position = identity */
    NTR_MMIO(uint32_t, 0x04000454) = 0;
    NTR_MMIO(uint32_t, 0x04000580) = 0u | (0u << 8) | (255u << 16) | (191u << 24);
    ntr::gx_set_light(0, -0.4f, -0.6f, -0.7f, 0x7FFF);
    ntr::gx_enable_lights(0x1);
}

// The fit projection smoke_model uses: scale and centre the model's
// identity-projected screen box into the middle 80% of the DS screen.
struct Fit { int32_t m[16]; };

Fit fit_for(const ntr::GxTriangle *t, size_t n)
{
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    for (size_t i = 0; i < n; ++i)
        for (int v = 0; v < 3; ++v) {
            minx = fminf(minx, t[i].v[v].x); maxx = fmaxf(maxx, t[i].v[v].x);
            miny = fminf(miny, t[i].v[v].y); maxy = fmaxf(maxy, t[i].v[v].y);
        }
    const float cx0 = 2.0f * minx / ntr::SCREEN_W - 1.0f;
    const float cx1 = 2.0f * maxx / ntr::SCREEN_W - 1.0f;
    const float cy0 = 1.0f - 2.0f * maxy / ntr::SCREEN_H;
    const float cy1 = 1.0f - 2.0f * miny / ntr::SCREEN_H;
    const float w = fmaxf(cx1 - cx0, 1e-6f), h = fmaxf(cy1 - cy0, 1e-6f);
    const float s = fminf(1.6f / w, 1.6f / h);
    const float tx = -0.5f * (cx0 + cx1) * s, ty = -0.5f * (cy0 + cy1) * s;
    // Scale z too: the identity-projected model spans about -5..5 in z, and the
    // GPU clips to [0,1] where gx.cpp's CPU raster doesn't clip on depth. A real
    // game projection keeps z in range; this test one needs telling. Spinning on Y
    // turns x extent into depth, so size z by the largest extent on any axis.
    float r = 1e-6f;
    for (size_t i = 0; i < n; ++i)
        for (int v = 0; v < 3; ++v) {
            r = fmaxf(r, fabsf(2.0f * t[i].v[v].x / ntr::SCREEN_W - 1.0f));
            r = fmaxf(r, fabsf(1.0f - 2.0f * t[i].v[v].y / ntr::SCREEN_H));
            r = fmaxf(r, fabsf(2.0f * t[i].v[v].z - 1.0f));
        }
    const float zs = 0.9f / r, zt = 0.0f;
    Fit f = {{(int32_t)(s * 4096), 0, 0, 0, 0, (int32_t)(s * 4096), 0, 0,
              0, 0, (int32_t)(zs * 4096), 0,
              (int32_t)(tx * 4096), (int32_t)(ty * 4096), (int32_t)(zt * 4096), 4096}};
    return f;
}

void load_projection(const Fit &f)
{
    NTR_MMIO(uint32_t, 0x04000440) = 0;
    for (int i = 0; i < 16; ++i) NTR_MMIO(uint32_t, 0x04000458) = (uint32_t)f.m[i];
    NTR_MMIO(uint32_t, 0x04000440) = 1;
    NTR_MMIO(uint32_t, 0x04000454) = 0;
}

void set_yaw(Model *model, float a)
{
    int *m = (int *)&model->mat4x3;
    memset(m, 0, sizeof model->mat4x3);
    const int c = (int)(cosf(a) * 4096), s = (int)(sinf(a) * 4096);
    m[0] = c;   m[2] = -s;
    m[4] = 0x1000;
    m[6] = s;   m[8] = c;
}

}  // namespace

namespace {

// A stand-in for the HAL's 2D compositor: write only the pixels the "2D layer"
// owns, with the overlay verdict in the alpha byte (ntr/gx.h GX_OV_*).
void draw_test_2d(ntr::Framebuffer &fb)
{
    // FRONT: a panel in the top-left, with a border, over everything.
    for (int y = 8; y < 40; ++y)
        for (int x = 8; x < 88; ++x) {
            const bool edge = y < 10 || y >= 38 || x < 10 || x >= 86;
            fb.px[y][x] = (GX_OV_FRONT << 24) | (edge ? 0xFFFFFFu : 0x2060E0u);
        }
    // BEHIND: a yellow bar across the middle, behind the 3D layer.
    for (int y = 120; y < 136; ++y)
        for (int x = 0; x < 256; ++x)
            fb.px[y][x] = (GX_OV_BEHIND << 24) | 0xF0D020u;
}

}  // namespace

int sm64ds_smoke_main(void)
{
    if (!ntr::io_init()) { printf("io_init failed\n"); return 2; }
    if (!Heap::SetupRootHeap()) { printf("SetupRootHeap failed\n"); return 2; }
    if (!card3ds_init()) { printf("card3ds_init failed\n"); return 2; }

    memset(&data_0209b3ec, 0, sizeof data_0209b3ec);
    ((int *)&data_0209b3ec)[0] = 0x1000;
    ((int *)&data_0209b3ec)[4] = 0x1000;
    ((int *)&data_0209b3ec)[8] = 0x1000;

    SharedFilePtrC ptr;
    _ZN13SharedFilePtr9ConstructEj(&ptr, 1214);   /* data/player/b_mario_all.bmd */
    static char storage[0x50];
    Model *model = (Model *)storage;
    _ZN5ModelC1Ev(storage);
    void *file = Model::LoadFile(*(SharedFilePtr *)&ptr);
    if (!file || model->Model::DoSetFile((char *)file, 0, -1) != 1) {
        printf("model load failed\n");
        return 2;
    }

    set_yaw(model, 0.0f);
    reset_scene();
    model->Model::Render(NULL);
    size_t n = 0;
    const ntr::GxTriangle *t = ntr::gx_polygons(n);
    const Fit fit = fit_for(t, n);

    static ntr::Framebuffer fb;
    int rc = 1, shots = 0;
    for (int frame = 0; host3ds_frame_continue(); ++frame) {
        set_yaw(model, frame < 8 ? 0.0f : (frame - 8) * 0.03f);
        reset_scene();                       /* gx_reset + lights */
        load_projection(fit);
        model->Model::Render(NULL);          /* the game's render walk */
        ntr::gx_card_present_allow(1);
        ntr::gx_clear_fill(fb);              /* transparent overlay if kept */
        ntr::gx_render(fb);                  /* the card keeps the frame */
        draw_test_2d(fb);
        /* the HAL's fade call: the message compositor makes it every frame */
        if (frame >= 5) port_gpu_card_set_fade(8, 0);
        card3ds_present(fb);

        if (frame == 4) shots += card3ds_screenshot("sdmc:/sm64ds3ds/card_model.bmp");
        if (frame == 7) shots += card3ds_screenshot("sdmc:/sm64ds3ds/card_model_fade.bmp");
        if (frame == 7) {
            const bool kept = ntr::gx_card_overlay_active();
            printf("card_model: frame kept on the card: %s, screenshots %d/2\n",
                   ntr::gx_card_overlay_active() ? "yes" : "no", shots);
            rc = (shots == 2 && kept) ? 0 : 1;
            printf("RESULT %s\n", rc == 0 ? "PASS" : "FAIL");
            fflush(stdout);
        }
    }
    card3ds_shutdown();
    return rc;
}