// Test of gpu3ds::readback on its own (what the card's settle uses): draw a
// held frame offscreen and read it back. The frame is built like the title's:
// a far full-screen backdrop, a coplanar translucent textured quad over it, a
// nearer opaque textured triangle (mode 2) in front, and two corner triangles
// to pin the orientation. Each has to show up in the right place.
#include <stdio.h>
#include <string.h>

#include "ntr/gx.h"
#include "ntr/ppu.h"

#include "gpu3ds.h"

namespace {

ntr::GxTriangle tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb,
                    float z = 0.5f, float w = 1.0f)
{
    ntr::GxTriangle t;
    memset(&t, 0, sizeof t);
    const float xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    for (int k = 0; k < 3; ++k) {
        t.v[k].x = xs[k];
        t.v[k].y = ys[k];
        t.v[k].z = z;
        t.v[k].w = w;
        t.v[k].color = argb;
    }
    t.cull = 3;      // both faces
    t.alpha = 31;
    return t;
}

void textured(ntr::GxTriangle &t, const uint32_t *tex, int tw, int th)
{
    t.tex = tex;
    t.tw = tw;
    t.th = th;
    t.tex_id = 1;
    t.wrap = 3;
    for (int k = 0; k < 3; ++k) {
        t.v[k].u = t.v[k].x;   // one texel per pixel, repeating
        t.v[k].v = t.v[k].y;
    }
}

int check(const uint32_t *pic, int stride, int x, int y, uint32_t want, const char *what)
{
    const uint32_t got = pic[(size_t)y * stride + x];
    const bool ok = got == want;
    printf("  (%3d,%3d) %-16s %08lx %s (want %08lx)\n", x, y, what, (unsigned long)got,
           ok ? "ok" : "WRONG", (unsigned long)want);
    return ok ? 0 : 1;
}

uint32_t g_tex[16 * 16];

}  // namespace

int main()
{
    if (!gpu3ds::init()) { printf("gpu3ds::init failed\n"); return 2; }
    for (int i = 0; i < 16 * 16; ++i) g_tex[i] = 0xFF0000FFu;   // opaque blue

    ntr::GxTriangle tris[8] = {
        // the backdrop: far, big w, full screen (two triangles)
        tri(0, 0, 256, 0, 0, 192, 0xFF808080u, 0.982f, 58.6f),
        tri(256, 0, 256, 192, 0, 192, 0xFF808080u, 0.982f, 58.6f),
        // the corners
        tri(0, 0, 60, 0, 0, 40, 0xFFFF0000u, 0.9f, 30.0f),
        tri(255, 191, 196, 191, 255, 151, 0xFF00FF00u, 0.9f, 30.0f),
        // the "head": near, small w, textured, toon mode, opaque
        tri(100, 60, 160, 60, 100, 130, 0xFFFFFFFFu, 0.516f, 2.4f),
        tri(160, 60, 160, 130, 100, 130, 0xFFFFFFFFu, 0.516f, 2.4f),
        // z order against z*w order: the near one (z 0.3) has the big w
        tri(20, 100, 60, 100, 20, 140, 0xFFFFFF00u, 0.6f, 1.0f),
        tri(20, 100, 60, 100, 20, 140, 0xFF00FFFFu, 0.3f, 10.0f),
    };
    textured(tris[4], g_tex, 16, 16);
    textured(tris[5], g_tex, 16, 16);
    tris[4].mode = tris[5].mode = 2;

    int bad = 0;
    // Passes 1-2 come after a screen frame, as in the game (a settle sits
    // between two presents).
    for (int pass = 0; pass < 4; ++pass) {
        if (pass == 3) {   // the same frame with every w = 1
            for (auto &t : tris)
                for (auto &v : t.v) v.w = 1.0f;
            printf("w = 1 everywhere:\n");
        }
        if (pass >= 1) {
            gpu3ds::frame_begin(0xFF203040u);
            gpu3ds::draw(tris, 8);
            gpu3ds::frame_end();
        }
        int stride = 0;
        const uint32_t *pic = gpu3ds::readback(tris, 8, &stride);
        if (!pic) { printf("readback returned null\n"); return 1; }
        const auto &st = gpu3ds::stats();
        printf("pass %d: stride %d; %u in, %u drawn, %u culled, %u draw calls, %u uploads\n",
               pass, stride, st.tris_in, st.tris_drawn, st.tris_culled, st.draw_calls,
               st.tex_uploads);
        bad += check(pic, stride, 10, 10, 0xFFFF0000u, "red corner");
        bad += check(pic, stride, 250, 186, 0xFF00FF00u, "green corner");
        bad += check(pic, stride, 128, 96, 0xFF0000FFu, "head (textured)");
        bad += check(pic, stride, 200, 30, 0xFF808080u, "backdrop");
        bad += check(pic, stride, 25, 105, 0xFF00FFFFu, "near, big w");
    }
    gpu3ds::shutdown();
    printf("readback: %d wrong\n", bad);
    return bad ? 1 : 0;
}
