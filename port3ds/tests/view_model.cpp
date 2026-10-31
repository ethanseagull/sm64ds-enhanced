// First light: a game model drawn by the 3DS GPU.
//
// The model is loaded through the game's own code (SharedFilePtr,
// Model::LoadFile, DoSetFile) and pushed through ntr/gx.cpp every frame by
// Model::Render. Instead of gx.cpp's CPU rasterizer the triangles go to gpu3ds
// (citro3d) on the top screen, spinning on Y.
//
// After 90 frames the top screen is saved to sdmc:/sm64ds3ds/view_model.bmp
// and the run reports RESULT; it keeps spinning until START.
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

#include "gpu3ds.h"
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

int sm64ds_smoke_main(void)
{
    setenv("SM64DS_TEX_LOG", "1", 1);   // gx.cpp: one line per distinct texture bind
    if (!ntr::io_init()) { printf("io_init failed\n"); return 2; }
    if (!Heap::SetupRootHeap()) { printf("SetupRootHeap failed\n"); return 2; }
    if (!gpu3ds::init()) { printf("gpu3ds::init failed\n"); return 2; }

    memset(&data_0209b3ec, 0, sizeof data_0209b3ec);
    ((int *)&data_0209b3ec)[0] = 0x1000;
    ((int *)&data_0209b3ec)[4] = 0x1000;
    ((int *)&data_0209b3ec)[8] = 0x1000;

    const u32 handle = 1214;   /* data/player/b_mario_all.bmd */
    SharedFilePtrC ptr;
    _ZN13SharedFilePtr9ConstructEj(&ptr, handle);
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
    printf("view_model: %u triangles at rest\n", (unsigned)n);
    {
        unsigned with_tex = 0, with_id = 0;
        for (size_t i = 0; i < n; ++i) { with_tex += t[i].tex != nullptr; with_id += t[i].tex_id != 0; }
        float zmin = 1e30f, zmax = -1e30f, wmin = 1e30f, wmax = -1e30f;
        for (size_t i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) {
                zmin = fminf(zmin, t[i].v[k].z); zmax = fmaxf(zmax, t[i].v[k].z);
                wmin = fminf(wmin, t[i].v[k].w); wmax = fmaxf(wmax, t[i].v[k].w);
            }
        printf("  z %.3f .. %.3f   w %.3f .. %.3f\n", zmin, zmax, wmin, wmax);
        printf("  with tex %u, with tex_id %u, first dbg_tex %08x tw %d th %d\n",
               with_tex, with_id, n ? (unsigned)t[0].dbg_tex : 0u, n ? t[0].tw : 0, n ? t[0].th : 0);
    }
    const Fit fit = fit_for(t, n);

    int rc = 1;
    const uint64_t t0 = host3ds_ms();
    for (int frame = 0; host3ds_frame_continue(); ++frame) {

        // Hold yaw 0 for the first frames: frame 3 is the GPU/CPU comparison.
        set_yaw(model, frame < 5 ? 0.0f : (frame - 5) * 0.03f);
        reset_scene();
        load_projection(fit);
        model->Model::Render(NULL);
        t = ntr::gx_polygons(n);

        gpu3ds::frame_begin(0xFF203048u);
        gpu3ds::draw(t, n);
        gpu3ds::frame_end();

        if (frame == 3) {
            // The same triangles through gx.cpp's CPU rasteriser, the PC
            // port's byte-exact reference, for a side-by-side check.
            static ntr::Framebuffer fb;
            for (int y = 0; y < ntr::SCREEN_H; ++y)
                for (int x = 0; x < ntr::SCREEN_W; ++x) fb.px[y][x] = 0xFF203048u;
            ntr::gx_render(fb);
            ntr::ppu_write_bmp("sdmc:/sm64ds3ds/view_model_cpu.bmp", fb);
        }
        if (frame == 4) {   // presented by now: the frame-3 picture
            const bool shot = gpu3ds::screenshot_bmp("sdmc:/sm64ds3ds/view_model_gpu0.bmp");
            printf("yaw-0 GPU screenshot: %s\n", shot ? "ok" : "FAILED");
        }

        if (frame == 90) {
            const gpu3ds::Stats &st = gpu3ds::stats();
            const double ms = (double)(host3ds_ms() - t0) / 91.0;
            printf("frame 90: %u tris in, %u drawn, %u culled, %u draw calls, "
                   "%u textures cached, %u z clamped\n", st.tris_in, st.tris_drawn,
                   st.tris_culled, st.draw_calls, st.tex_cached, st.z_clamped);
            printf("average frame time: %.1f ms\n", ms);
            const bool shot = gpu3ds::screenshot_bmp("sdmc:/sm64ds3ds/view_model.bmp");
            printf("screenshot: %s\n", shot ? "sdmc:/sm64ds3ds/view_model.bmp" : "FAILED");
            rc = (st.tris_drawn > 0 && shot) ? 0 : 1;
            printf("RESULT %s\n", rc == 0 ? "PASS" : "FAIL");
            fflush(stdout);
        }
    }
    gpu3ds::shutdown();
    return rc;
}
