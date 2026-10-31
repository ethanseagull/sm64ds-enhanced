// Engine B's affine BG drawn by the GPU (the gameplay minimap).
//
// The single painter (scan_single_bg) paints only the sprites into the picture
// buffer and describes the affine BG here instead. The card clears the bottom
// screen to the backdrop, draws the BG picture as a quad rotated by its matrix
// (gpu3ds::affine_draw), and lays the sprites on top. sub_async.cpp keeps one
// of these per picture buffer.
#pragma once

#include <stdint.h>

struct SubAffine {
    int on;                     // the buffer's picture is the sprites alone
    int map_w, map_h;           // the BG's picture, pixels (powers of two)
    int wrap;                   // the display-area overflow: repeat, else transparent
    int32_t refx, refy;         // 8.8 fixed, at the screen's top-left pixel
    int32_t pa, pb, pc, pd;     // 8.8 fixed
    uint32_t backdrop;          // 0xFFRRGGBB
    const uint32_t *pic;        // map_w x map_h, 0 where transparent
    unsigned ver;               // a new number whenever pic's pixels change
};

// The painter's calls (weak, so a build without sub_async.cpp scans as before):
// get the descriptor for fb (null for inline scans or with the GPU path off),
// then say whether the buffer holds only the sprites (copying pic if its
// version is new, since the next scan may rebuild it).
extern "C" SubAffine *game3ds_sub_affine_begin(const void *fb) __attribute__((weak));
extern "C" void game3ds_sub_affine_done(const void *fb, int ok) __attribute__((weak));
