// Included before raster_obj inside ppu_sub.cpp's private namespace (via a
// gccfix rule); raster_obj calls obj_plain for every sprite that isn't affine,
// a bitmap or a window.

// Plain sprites a tile row at a time. raster_obj's general loop works out each
// sprite pixel separately (bounds, tile slot, address, byte read, colour
// conversion), which was most of the opening cutscene's bottom-screen scan.
// Here each 8-pixel run reads its tile row once and each palette entry is
// converted the first time it's used. Same result pixel for pixel:
//   - same slot and cell formula, including 2D mapping
//   - hflip shows texel w-1-sx, so tile columns and pixels run reversed
//     (w is a multiple of 8); vflip the same for rows
//   - index 0 is transparent; a pixel with a higher-priority sprite is kept,
//     ties are overwritten (the 127 -> 0 walk)
//   - semi is OBJ mode 1; rows written are marked in g_obj_rows
// GAME3DS_OBJ_FAST_OFF=1 uses the general loop for every sprite (affine too).
bool obj_fast_enabled()
{
    static const bool on = std::getenv("GAME3DS_OBJ_FAST_OFF") == nullptr;
    return on;
}

void obj_plain(ObjPixel (*obj)[256], int x, int y, int w, int h, bool hflip, bool vflip,
               bool c256, uint32_t tile, uint32_t pal, uint8_t prio, bool semi, bool map1d,
               uint32_t boundary, uint32_t objext)
{
    uint32_t lut[256];   // the sprite's colours as used, 0 = not converted yet
    std::memset(lut, 0, (c256 ? 256 : 16) * sizeof lut[0]);
    const int tiles_w = w / 8;
    const uint32_t base = kObjVram + tile * boundary;
    for (int sy = 0; sy < h; ++sy) {
        const int py = y + sy;
        if (py < 0 || py >= 192) continue;
        const int ty = vflip ? h - 1 - sy : sy;
        const int trow = ty >> 3, fy = ty & 7;
        ObjPixel *row = obj[py];
        bool wrote = false;
        for (int tc = 0; tc < tiles_w; ++tc) {
            const int px0 = x + tc * 8;
            if (px0 + 8 <= 0 || px0 >= 256) continue;
            const int tcol = hflip ? tiles_w - 1 - tc : tc;
            const uint32_t slot =
                map1d ? (c256 ? (uint32_t)(trow * tiles_w + tcol) * 2u
                              : (uint32_t)(trow * tiles_w + tcol))
                      : (uint32_t)(trow * 32 + (c256 ? tcol * 2 : tcol));
            const uint32_t cell = base + slot * 32u;
            uint8_t idx[8];
            if (c256) {
                std::memcpy(idx, reinterpret_cast<const void *>(cell + fy * 8u), 8);
            } else {
                uint8_t pair[4];
                std::memcpy(pair, reinterpret_cast<const void *>(cell + fy * 4u), 4);
                for (int i = 0; i < 4; ++i) {
                    idx[2 * i] = pair[i] & 0xF;
                    idx[2 * i + 1] = pair[i] >> 4;
                }
            }
            for (int i = 0; i < 8; ++i) {
                const int px = px0 + i;
                if (px < 0 || px >= 256) continue;
                const uint32_t v = idx[hflip ? 7 - i : i];
                if (!v) continue;
                ObjPixel &o = row[px];
                if (o.hit && prio > o.prio) continue;
                uint32_t c = lut[v];
                if (!c) {
                    c = c256 ? (objext ? bgr555(rd16(objext + (pal * 256u + v) * 2u))
                                       : bgr555(rd16(kObjPltt + v * 2u)))
                             : bgr555(rd16(kObjPltt + (pal * 16u + v) * 2u));
                    lut[v] = c;
                }
                o.color = c;
                o.prio = prio;
                o.hit = 1;
                o.semi = semi ? 1 : 0;
                wrote = true;
            }
        }
        if (wrote) g_obj_rows[py] = 1;
    }
}

// Affine sprites (not bitmaps or windows) the same way. The title's star and
// its ~35 rotating 16x16 sparkles were drawn a pixel at a time by the general
// loop, which held the star at 30 Hz. Same result pixel for pixel:
//   - texel = ((pa * cx + pb * cy) >> 8) + w/2, ((pc * cx + pd * cy) >> 8) + h/2,
//     with cx, cy from the box centre; the sums step by pa and pc along a row
//   - texels outside the sprite are transparent; slot and cell as the loop
//   - index 0, priority, semi and g_obj_rows as obj_plain
// The sprite is unpacked into an index picture once and its colours converted
// once each.
void obj_affine(ObjPixel (*obj)[256], uint32_t oam_b, int grp, int x, int y, int w, int h, bool dbl,
                bool c256, uint32_t tile, uint32_t pal, uint8_t prio, bool semi, bool map1d,
                uint32_t boundary, uint32_t objext)
{
    const int pa = (int16_t)rd16(oam_b + (grp * 4 + 0) * 8u + 6);
    const int pb = (int16_t)rd16(oam_b + (grp * 4 + 1) * 8u + 6);
    const int pc = (int16_t)rd16(oam_b + (grp * 4 + 2) * 8u + 6);
    const int pd = (int16_t)rd16(oam_b + (grp * 4 + 3) * 8u + 6);
    const int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
    const int sx0 = x < 0 ? -x : 0, sx1 = 256 - x < bw ? 256 - x : bw;
    if (sx0 >= sx1 || y >= 192 || y + bh <= 0) return;
    const int tiles_w = w / 8, tiles_h = h / 8;
    const uint8_t *const base = reinterpret_cast<const uint8_t *>(kObjVram + tile * boundary);
    // a cell's byte offset from base, by tile row and column (the loop's slot * 32)
    const int row_bytes = map1d ? tiles_w * (c256 ? 64 : 32) : 32 * 32;
    const int col_bytes = c256 ? 64 : 32;
    // The sprite as a w x h index picture, unpacked once from OBJ VRAM a tile row at
    // a time, so each pixel is a single read at (ty << wsh) + tx.
    const int wsh = __builtin_ctz((unsigned)w);
    uint8_t img[64 * 64];
    {
        uint8_t cells[8 * 64];   // one tile row, 8 tiles at 8bpp at most
        const int row_local = tiles_w * col_bytes;
        for (int tr = 0; tr < tiles_h; ++tr) {
            std::memcpy(cells, base + tr * row_bytes, row_local);
            for (int fy = 0; fy < 8; ++fy) {
                uint8_t *dst = img + ((tr * 8 + fy) << wsh);
                for (int tc = 0; tc < tiles_w; ++tc) {
                    const uint8_t *src = cells + tc * col_bytes;
                    if (c256) {
                        std::memcpy(dst + tc * 8, src + fy * 8, 8);
                    } else {
                        for (int i = 0; i < 4; ++i) {
                            const uint8_t b = src[fy * 4 + i];
                            dst[tc * 8 + 2 * i] = b & 0xF;
                            dst[tc * 8 + 2 * i + 1] = b >> 4;
                        }
                    }
                }
            }
        }
    }
    // its colours: a 16-colour sprite's bank whole, a 256-colour one's as used
    uint32_t lut[256];   // 0 = not converted yet
    if (c256)
        std::memset(lut, 0, sizeof lut);
    else
        for (uint32_t v = 1; v < 16; ++v) lut[v] = bgr555(rd16(kObjPltt + (pal * 16u + v) * 2u));
    ObjPixel put;   // what a written pixel holds but its colour
    std::memset(&put, 0, sizeof put);
    put.prio = prio;
    put.hit = 1;
    put.semi = semi ? 1 : 0;
    const int hw = w / 2, hh = h / 2;
    for (int sy = 0; sy < bh; ++sy) {
        const int py = y + sy;
        if (py < 0 || py >= 192) continue;
        const int cy = sy - bh / 2;
        const int cx = sx0 - bw / 2;
        int X = pa * cx + pb * cy, Y = pc * cx + pd * cy;
        ObjPixel *row = obj[py] + x;
        bool wrote = false;
        for (int sx = sx0; sx < sx1; ++sx, X += pa, Y += pc) {
            const int tx = (X >> 8) + hw, ty = (Y >> 8) + hh;
            if ((unsigned)tx >= (unsigned)w || (unsigned)ty >= (unsigned)h) continue;
            const uint32_t v = img[(ty << wsh) + tx];
            if (!v) continue;
            ObjPixel &o = row[sx];
            if (o.hit && prio > o.prio) continue;
            uint32_t c = lut[v];
            if (!c) {
                c = objext ? bgr555(rd16(objext + (pal * 256u + v) * 2u)) : bgr555(rd16(kObjPltt + v * 2u));
                lut[v] = c;
            }
            put.color = c;
            o = put;
            wrote = true;
        }
        if (wrote) g_obj_rows[py] = 1;
    }
}
