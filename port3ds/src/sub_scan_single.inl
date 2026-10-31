// Included after sample_line inside ppu_sub.cpp's private namespace.
extern "C" unsigned long long svcGetSystemTick(void);   // libctru, for GAME3DS_SUB_PROF
#include "sub_affine.h"
// Scans by path, for game/sub_async.cpp's [perf] line: single BG, several BGs,
// alpha mode, the general walk (scan_fast's own, a gccfix rule).
extern "C" unsigned game3ds_sub_paths[4];

// Faster sample_line for text BGs, a tile at a time: read the screen entry and
// the tile row once and take every pixel from them, instead of a byte read per
// pixel. Same flips, transparency and palettes. The map wraps on a multiple of
// 8, so a tile never straddles the wrap. Other BG types use sample_line.
// GAME3DS_SUB_TILE_OFF=1 always uses sample_line.
void sample_line_fast(const BgLayer &c, int y, const uint32_t *pal32, uint32_t *col,
                      uint8_t *ok)
{
    static const bool off = std::getenv("GAME3DS_SUB_TILE_OFF") != nullptr;
    if (c.kind != BG_TEXT || off) {
        sample_line(c, y, pal32, col, ok);
        return;
    }
    const int py = (y + c.vofs) & (c.map_h * 8 - 1);
    const int ty = py >> 3, fy0 = py & 7;
    const int blocks_w = c.map_w >> 5;
    const uint32_t rowbase = c.screen + ((ty >> 5) * blocks_w) * 0x800 + (ty & 31) * 32 * 2;
    const int wm = c.map_w * 8 - 1;
    for (int x = 0; x < 256;) {
        const int px = (x + c.hofs) & wm;
        const int tx = px >> 3;
        const uint16_t se = rd16(rowbase + (tx >> 5) * 0x800 + (tx & 31) * 2);
        const int fy = (se & 0x800) ? 7 - fy0 : fy0;
        const bool hflip = (se & 0x400) != 0;
        const uint32_t tile = se & 0x3FF;
        const int fx0 = px & 7;
        int n = 8 - fx0;
        if (n > 256 - x) n = 256 - x;
        uint8_t idx[8];
        if (c.bpp8) {
            std::memcpy(idx, reinterpret_cast<const void *>(c.chars + tile * 64 + fy * 8), 8);
        } else {
            uint8_t pair[4];
            std::memcpy(pair, reinterpret_cast<const void *>(c.chars + tile * 32 + fy * 4), 4);
            for (int i = 0; i < 4; ++i) {
                idx[2 * i] = pair[i] & 0xF;
                idx[2 * i + 1] = pair[i] >> 4;
            }
        }
        const uint32_t slot = (se >> 12) & 0xF;
        for (int i = 0; i < n; ++i) {
            const int fx = hflip ? 7 - (fx0 + i) : fx0 + i;
            const uint32_t v = idx[fx];
            if (!v) {
                ok[x + i] = 0;
                continue;
            }
            ok[x + i] = 1;
            if (!c.bpp8)
                col[x + i] = pal32[v + slot * 16];
            else if (c.ext)
                col[x + i] = bgr555(rd16(c.ext + (slot * 256 + v) * 2));
            else
                col[x + i] = pal32[v];
        }
        x += n;
    }
}

// Text BGs kept as pictures. Even the fast path read every tile of every line
// each scan (~8-14 ms on the menus). Tiles and maps rarely change between
// frames, only the scroll does, so each text BG is kept as two whole-map
// pictures:
//   - palette indices (0 = transparent), from the map and tiles
//   - colours and opacity, from the indices and the palette
// A line is then two copies out of the colour picture at the scroll offset, or a
// per-pixel palette lookup while the palette is changing.
//
// Inputs are compared (not hashed) against saved copies: the map, the tiles it
// uses, and the palette. A changed map only redraws the changed tiles. A BG that
// changes two scans in a row is sampled directly until it settles, so one
// rewritten every frame doesn't cost a full redraw each time.
// GAME3DS_SUB_PLANE_OFF=1 samples every line directly.
struct Plane {
    // the indices' inputs
    uint32_t screen, chars, ext;
    bool bpp8;
    int map_w, map_h;              // tiles
    uint8_t *map, *chr;            // the saved map and tile data
    uint32_t map_len, chr_len;
    size_t map_cap, chr_cap;
    bool saved;                    // map/chr are the last scan's
    bool ix_valid;                 // ix is a picture of them
    bool ix_changed_last;          // the last scan found new tiles or layout
    // the colours' inputs
    bool pal_saved, pal_ext;       // pal (or extc) holds the palette px was made with
    bool px_valid;
    bool pal_changed_last;
    uint32_t pal[256];
    uint8_t extc[0x2000];
    uint32_t lut[4096];            // extc as colours
    // the pictures
    uint16_t *ix;
    uint32_t *px;                  // 0 where transparent
    uint8_t *ok;                   // 1 where opaque
    size_t cap;                    // pixels each holds
    unsigned gen;                  // a new number whenever ix or px may change (BgHeld's key)
};
Plane g_plane[4];
unsigned g_plane_gen;   // the last number handed out
inline void plane_touch(Plane &P) { P.gen = ++g_plane_gen; }
// this scan, per BG: 0 sample directly, 1 the colour picture, 2 the indices
// through g_plane_lut
int g_plane_mode[4];
const uint32_t *g_plane_lut[4];
// GAME3DS_SUB_PROF: per scan, the prepare's time and what it found
unsigned long long g_plane_ticks;
unsigned g_plane_scans, g_plane_hit, g_plane_tiles, g_plane_full, g_plane_draws, g_plane_recolors,
    g_plane_lookups;

// bytes_equal's word compare (it is defined below this file's include): every
// input here is word aligned and a whole number of words long
bool plane_same(const void *a, const void *b, size_t len)
{
    const uint32_t *p = static_cast<const uint32_t *>(a);
    const uint32_t *q = static_cast<const uint32_t *>(b);
    size_t n = len >> 2;
    for (; n >= 8; n -= 8, p += 8, q += 8)
        if (((p[0] ^ q[0]) | (p[1] ^ q[1]) | (p[2] ^ q[2]) | (p[3] ^ q[3]) |
             (p[4] ^ q[4]) | (p[5] ^ q[5]) | (p[6] ^ q[6]) | (p[7] ^ q[7])) != 0)
            return false;
    for (; n; --n)
        if (*p++ != *q++) return false;
    return true;
}

bool plane_enabled()
{
    static const bool on = std::getenv("GAME3DS_SUB_PLANE_OFF") == nullptr;
    return on;
}

// Largest text BG kept as a picture, in pixels (GAME3DS_SUB_PLANE_MAX_PX;
// main_game.cpp sets 65536 on Old 3DS). Bigger ones are sampled directly.
// Unset means no limit.
size_t plane_max_px()
{
    static const size_t max = [] {
        const char *e = std::getenv("GAME3DS_SUB_PLANE_MAX_PX");
        return e && *e ? (size_t)std::strtoul(e, nullptr, 0) : (size_t)-1;
    }();
    return max;
}

// A plane's buffers back to the heap; its next scan starts over. Returns the
// bytes.
size_t plane_free(Plane &P)
{
    const size_t bytes = P.cap * (sizeof *P.ix + sizeof *P.px + sizeof *P.ok) + P.map_cap + P.chr_cap;
    if (!bytes && !P.saved) return 0;
    std::free(P.ix); std::free(P.px); std::free(P.ok); std::free(P.map); std::free(P.chr);
    P.ix = nullptr; P.px = nullptr; P.ok = nullptr; P.map = P.chr = nullptr;
    P.cap = P.map_cap = P.chr_cap = 0;
    P.saved = P.ix_valid = P.px_valid = P.pal_saved = false;
    P.ix_changed_last = P.pal_changed_last = false;
    plane_touch(P);
    return bytes;
}

inline uint32_t plane_map_len(const BgLayer &c)
{
    return (uint32_t)((c.map_w >> 5) * (c.map_h >> 5)) * 0x800u;
}

// The colours the indices name: the extended slots, or pal32 itself
inline const uint32_t *plane_colours(const Plane &P)
{
    return P.bpp8 && P.ext ? P.lut : P.pal;
}

// One map entry's tile into the indices at tile (tx, ty) -- sample_line_fast's
// flips and transparency -- and into the colours when they are a picture
void plane_tile(Plane &P, int tx, int ty, uint16_t se)
{
    plane_touch(P);
    const int w = P.map_w * 8;
    const bool hflip = (se & 0x400) != 0, vflip = (se & 0x800) != 0;
    const uint32_t tile = se & 0x3FF, slot = (se >> 12) & 0xF;
    const uint32_t base = !P.bpp8 ? slot * 16 : P.ext ? slot * 256 : 0;
    const uint32_t *colours = plane_colours(P);
    for (int fy = 0; fy < 8; ++fy) {
        const int sy = vflip ? 7 - fy : fy;
        uint8_t idx[8];
        if (P.bpp8) {
            std::memcpy(idx, reinterpret_cast<const void *>(P.chars + tile * 64 + sy * 8), 8);
        } else {
            uint8_t pair[4];
            std::memcpy(pair, reinterpret_cast<const void *>(P.chars + tile * 32 + sy * 4), 4);
            for (int i = 0; i < 4; ++i) {
                idx[2 * i] = pair[i] & 0xF;
                idx[2 * i + 1] = pair[i] >> 4;
            }
        }
        const size_t at = (size_t)(ty * 8 + fy) * w + tx * 8;
        uint16_t *ix = P.ix + at;
        for (int i = 0; i < 8; ++i) {
            const uint32_t v = idx[hflip ? 7 - i : i];
            ix[i] = (uint16_t)(v ? base + v : 0);
        }
        if (!P.px_valid) continue;
        uint32_t *px = P.px + at;
        uint8_t *ok = P.ok + at;
        for (int i = 0; i < 8; ++i) {
            px[i] = ix[i] ? colours[ix[i]] : 0;
            ok[i] = ix[i] != 0;
        }
    }
}

// The map entry at byte offset `off` of the map: its tile coordinates
inline void plane_entry_xy(const Plane &P, uint32_t off, int &tx, int &ty)
{
    const uint32_t e = off >> 1, block = e >> 10, in = e & 1023;
    const int blocks_w = P.map_w >> 5;
    tx = (int)(block % (uint32_t)blocks_w) * 32 + (int)(in & 31);
    ty = (int)(block / (uint32_t)blocks_w) * 32 + (int)(in >> 5);
}

void plane_draw(Plane &P)
{
    P.px_valid = false;
    for (uint32_t off = 0; off < P.map_len; off += 2) {
        int tx, ty;
        plane_entry_xy(P, off, tx, ty);
        plane_tile(P, tx, ty, *reinterpret_cast<const uint16_t *>(P.map + off));
    }
    P.ix_valid = true;
    ++g_plane_draws;
}

void plane_recolor(Plane &P)
{
    plane_touch(P);
    const uint32_t *colours = plane_colours(P);
    const size_t n = (size_t)P.map_w * 8 * P.map_h * 8;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t v = P.ix[i];
        P.px[i] = v ? colours[v] : 0;
        P.ok[i] = v != 0;
    }
    P.px_valid = true;
    ++g_plane_recolors;
}

// The highest tile a map names, and so how much tile data it reads
uint32_t plane_chr_len(const uint8_t *map, uint32_t map_len, bool bpp8)
{
    uint32_t top = 0;
    for (uint32_t off = 0; off < map_len; off += 2) {
        const uint32_t t = *reinterpret_cast<const uint16_t *>(map + off) & 0x3FFu;
        if (t > top) top = t;
    }
    return (top + 1) * (bpp8 ? 64u : 32u);
}

template <typename T> bool plane_grow(T *&p, size_t &cap, size_t want)
{
    if (want <= cap && p) return true;
    T *q = static_cast<T *>(std::realloc(p, want * sizeof(T)));
    if (!q) return false;
    p = q;
    cap = want;
    return true;
}

// The indices of BG c for this scan; false: sample this scan directly
bool plane_indices(Plane &P, const BgLayer &c)
{
    const uint32_t map_len = plane_map_len(c);
    const bool layout = P.saved && P.screen == c.screen && P.chars == c.chars && P.ext == c.ext &&
                        P.bpp8 == c.bpp8 && P.map_w == c.map_w && P.map_h == c.map_h;
    const uint8_t *map = reinterpret_cast<const uint8_t *>(c.screen);
    const bool map_same = layout && plane_same(map, P.map, map_len);
    bool chr_same = layout;
    if (chr_same && !map_same)   // the tiles the new map reads must be in the saved copy
        chr_same = plane_chr_len(map, map_len, c.bpp8) <= P.chr_len;
    if (chr_same)
        chr_same = plane_same(reinterpret_cast<const void *>(c.chars), P.chr, P.chr_len);
    if (map_same && chr_same) {
        if (!P.ix_valid) plane_draw(P);   // held still since the last change
        P.ix_changed_last = false;
        ++g_plane_hit;
        return true;
    }
    if (chr_same && P.ix_valid) {
        // Only map entries changed: redraw those tiles and save the new entries. Each
        // word is read once, since the game may be writing the map while we scan on
        // core 2, and the saved entry has to be the one we drew.
        const volatile uint32_t *now = reinterpret_cast<const volatile uint32_t *>(map);
        uint32_t *was = reinterpret_cast<uint32_t *>(P.map);
        for (uint32_t w = 0; w < map_len / 4; ++w) {
            const uint32_t v = now[w];
            if (v == was[w]) continue;
            for (int h = 0; h < 2; ++h) {
                const uint16_t se = (uint16_t)(v >> (16 * h));
                if (se == (uint16_t)(was[w] >> (16 * h))) continue;
                int tx, ty;
                plane_entry_xy(P, w * 4 + h * 2, tx, ty);
                plane_tile(P, tx, ty, se);
                ++g_plane_tiles;
            }
            was[w] = v;
        }
        P.ix_changed_last = false;
        return true;
    }
    // A new layout or new tiles: save the inputs; draw the indices now unless
    // the last scan saw a change too.
    ++g_plane_full;
    plane_touch(P);
    P.saved = P.ix_valid = P.px_valid = false;
    const size_t pixels = (size_t)c.map_w * 8 * c.map_h * 8;
    if (pixels > P.cap) {
        size_t a = P.cap, b = P.cap, d = P.cap;
        if (!plane_grow(P.ix, a, pixels) || !plane_grow(P.px, b, pixels) ||
            !plane_grow(P.ok, d, pixels))
            return false;
        P.cap = pixels;
    }
    const uint32_t chr_len = plane_chr_len(map, map_len, c.bpp8);
    if (!plane_grow(P.map, P.map_cap, map_len) || !plane_grow(P.chr, P.chr_cap, chr_len))
        return false;
    P.screen = c.screen; P.chars = c.chars; P.ext = c.ext; P.bpp8 = c.bpp8;
    P.map_w = c.map_w; P.map_h = c.map_h;
    P.map_len = map_len; P.chr_len = chr_len;
    std::memcpy(P.map, map, map_len);
    std::memcpy(P.chr, reinterpret_cast<const void *>(c.chars), chr_len);
    P.saved = true;
    const bool twice = P.ix_changed_last;
    P.ix_changed_last = true;
    if (twice) return false;   // drawn once it holds still
    plane_draw(P);
    return true;
}

// Affine BGs as a picture (g_plane_mode 3). The gameplay minimap is one
// extended-affine BG (128x128, 8bpp, rotated and zoomed with the camera), and
// sample_line read map, tile and palette from VRAM for every screen pixel,
// most of the scan's time. Instead keep the map's colours as one picture and
// run sample_line's own arithmetic per line with a single read per pixel. The
// inputs are compared each scan; a change remakes the picture unless it also
// changed last scan and is bigger than a screen. Maps over 256x256 are sampled
// directly. GAME3DS_SUB_PLANE_OFF=1 samples every line directly.
struct AffPlane {
    uint32_t screen, chars, ext;
    int kind, map_w, map_h;
    bool bpp8, saved, valid, changed_last;
    uint8_t *map, *chr, *pal;   // the saved inputs
    uint32_t map_len, chr_len, pal_len;
    size_t map_cap, chr_cap, pal_cap;
    uint32_t *px;               // the colours, 0 where transparent
    uint8_t *ok;                // 1 where opaque
    size_t cap;
    unsigned ver;               // px's number (SubAffine::ver)
};
AffPlane g_aff[4];
unsigned g_aff_ver;                // the last number handed out
unsigned g_aff_made, g_aff_held;   // GAME3DS_SUB_PROF, per 120 scans
unsigned g_aff_gpu, g_aff_cpu;     // GAME3DS_SUB_PROF: scans by the GPU path, and refused it

// The highest tile an affine map names: its entries are bytes (BG_AFFINE) or
// 16-bit (the extended BG's, tile in the low 10 bits)
uint32_t aff_chr_len(const uint8_t *map, uint32_t map_len, int kind, bool bpp8)
{
    uint32_t top = 0;
    if (kind == BG_AFFINE) {
        for (uint32_t i = 0; i < map_len; ++i)
            if (map[i] > top) top = map[i];
    } else {
        for (uint32_t off = 0; off < map_len; off += 2) {
            const uint32_t t = *reinterpret_cast<const uint16_t *>(map + off) & 0x3FFu;
            if (t > top) top = t;
        }
    }
    return (top + 1) * (bpp8 ? 64u : 32u);
}

// Mode 3 for affine BG c this scan, or false: sample its lines directly
bool aff_prepare(const BgLayer &c, int bg, const uint32_t *pal32)
{
    AffPlane &A = g_aff[bg];
    const size_t pixels = (size_t)c.map_w * c.map_h;
    if (pixels > 256u * 256u) return false;
    const int tw = c.map_w >> 3, th = c.map_h >> 3;
    const uint32_t map_len = (uint32_t)(tw * th) * (c.kind == BG_AFFINE ? 1u : 2u);
    const uint8_t *map = reinterpret_cast<const uint8_t *>(c.screen);
    const uint32_t chr_len = aff_chr_len(map, map_len, c.kind, c.bpp8);
    const bool ext = c.bpp8 && c.ext;
    const void *pal = ext ? reinterpret_cast<const void *>(c.ext) : static_cast<const void *>(pal32);
    const uint32_t pal_len = ext ? 0x2000u : 256u * 4u;
    const bool same = A.saved && A.valid && A.screen == c.screen && A.chars == c.chars && A.ext == c.ext &&
                      A.kind == c.kind && A.bpp8 == c.bpp8 && A.map_w == c.map_w && A.map_h == c.map_h &&
                      chr_len <= A.chr_len && pal_len == A.pal_len && plane_same(map, A.map, map_len) &&
                      plane_same(reinterpret_cast<const void *>(c.chars), A.chr, chr_len) &&
                      plane_same(pal, A.pal, pal_len);
    if (same) {
        A.changed_last = false;
        ++g_aff_held;
        return true;
    }
    const bool twice = A.changed_last;
    A.changed_last = true;
    A.valid = false;
    if (!plane_grow(A.map, A.map_cap, map_len) || !plane_grow(A.chr, A.chr_cap, chr_len) ||
        !plane_grow(A.pal, A.pal_cap, pal_len))
        return A.saved = false;
    // saved before the picture is made from VRAM: a write in between makes
    // the next scan's compare fail, and the picture is made again
    std::memcpy(A.map, map, map_len);
    std::memcpy(A.chr, reinterpret_cast<const void *>(c.chars), chr_len);
    std::memcpy(A.pal, pal, pal_len);
    A.screen = c.screen; A.chars = c.chars; A.ext = c.ext; A.kind = c.kind; A.bpp8 = c.bpp8;
    A.map_w = c.map_w; A.map_h = c.map_h;
    A.map_len = map_len; A.chr_len = chr_len; A.pal_len = pal_len;
    A.saved = true;
    if (twice && pixels > (size_t)SUB_W * SUB_H) return false;
    if (pixels > A.cap) {
        size_t a = A.cap, b = A.cap;
        if (!plane_grow(A.px, a, pixels) || !plane_grow(A.ok, b, pixels)) return false;
        A.cap = pixels;
    }
    for (int py = 0; py < c.map_h; ++py)
        for (int px = 0; px < c.map_w; ++px) {
            uint16_t se;
            uint32_t tile;
            int fx = px & 7, fy = py & 7;
            if (c.kind == BG_AFFINE) {
                tile = rd8(c.screen + (py >> 3) * tw + (px >> 3));
                se = 0;
            } else {
                se = rd16(c.screen + ((py >> 3) * tw + (px >> 3)) * 2);
                tile = se & 0x3FF;
                if (se & 0x400) fx = 7 - fx;
                if (se & 0x800) fy = 7 - fy;
            }
            const size_t at = (size_t)py * c.map_w + px;
            uint32_t col = 0;
            A.ok[at] = tile_px(c, pal32, tile, fx, fy, se, col) ? 1 : 0;
            A.px[at] = A.ok[at] ? col : 0;
        }
    A.valid = true;
    A.ver = ++g_aff_ver;
    ++g_aff_made;
    return true;
}

// sample_line's affine line, out of the picture
inline void aff_line(const BgLayer &c, const AffPlane &A, int y, uint32_t *col, uint8_t *ok)
{
    // (a colour is never 0 -- bgr555 sets the alpha byte -- so px alone says
    // opaque; the sizes are powers of two)
    const unsigned w = (unsigned)c.map_w, h = (unsigned)c.map_h;
    const int sh = __builtin_ctz(w);
    const uint32_t *pic = A.px;
    int sx = c.refx + c.pb * y, sy = c.refy + c.pd * y;
    if (c.wrap) {
        for (int x = 0; x < 256; ++x, sx += c.pa, sy += c.pc) {
            const uint32_t v = pic[(((unsigned)(sy >> 8) & (h - 1)) << sh) + ((unsigned)(sx >> 8) & (w - 1))];
            col[x] = v;
            ok[x] = v != 0;
        }
        return;
    }
    for (int x = 0; x < 256; ++x, sx += c.pa, sy += c.pc) {
        const unsigned px = (unsigned)(sx >> 8), py = (unsigned)(sy >> 8);
        if (px >= w || py >= h) {
            ok[x] = 0;
            continue;
        }
        const uint32_t v = pic[(py << sh) + px];
        col[x] = v;
        ok[x] = v != 0;
    }
}

// Decide BG `bg`'s source for this scan (g_plane_mode / g_plane_lut).
void plane_prepare_one(const BgLayer &c, int bg, const uint32_t *pal32)
{
    g_plane_mode[bg] = 0;
    if (!plane_enabled()) return;
    if (c.kind == BG_AFFINE || c.kind == BG_EXT_AFFINE) {
        if (aff_prepare(c, bg, pal32)) g_plane_mode[bg] = 3;
        return;
    }
    if (c.kind != BG_TEXT) return;
    Plane &P = g_plane[bg];
    if ((size_t)c.map_w * 8 * c.map_h * 8 > plane_max_px()) {
        plane_free(P);   // sampled directly; a bigger picture held from before goes
        return;
    }
    if (!plane_indices(P, c)) return;
    const bool ext = c.bpp8 && c.ext;
    const bool pal_same = P.pal_saved && P.pal_ext == ext &&
                          (ext ? plane_same(reinterpret_cast<const void *>(c.ext), P.extc, sizeof P.extc)
                               : plane_same(pal32, P.pal, sizeof P.pal));
    if (pal_same) {
        P.pal_changed_last = false;
        if (!P.px_valid) plane_recolor(P);
        g_plane_mode[bg] = 1;
        return;
    }
    // a new palette: the colour picture is redrawn now unless the last scan
    // had a new one too; meanwhile the line is looked up through it
    P.pal_saved = true;
    P.pal_ext = ext;
    P.px_valid = false;
    if (ext) {
        std::memcpy(P.extc, reinterpret_cast<const void *>(c.ext), sizeof P.extc);
        const uint16_t *e = reinterpret_cast<const uint16_t *>(P.extc);
        for (int i = 0; i < 4096; ++i) P.lut[i] = bgr555(e[i]);
    } else {
        std::memcpy(P.pal, pal32, sizeof P.pal);
    }
    const bool twice = P.pal_changed_last;
    P.pal_changed_last = true;
    if (!twice) {
        plane_recolor(P);
        g_plane_mode[bg] = 1;
        return;
    }
    ++g_plane_lookups;
    g_plane_mode[bg] = 2;
    g_plane_lut[bg] = plane_colours(P);
}

void plane_prepare(const BgLayer *bgs, const int *flat, int nflat, const uint32_t *pal32)
{
    static const bool prof = std::getenv("GAME3DS_SUB_PROF") != nullptr;
    const unsigned long long t0 = prof ? svcGetSystemTick() : 0;
    for (int i = 0; i < 4; ++i) g_plane_mode[i] = 0;
    for (int k = 0; k < nflat; ++k) plane_prepare_one(bgs[flat[k]], flat[k], pal32);
    if (!prof) return;
    g_plane_ticks += svcGetSystemTick() - t0;
    if (++g_plane_scans % 120 == 0) {
        std::fprintf(stderr, "[subprof] BG pictures per scan: prepare %.2f ms; per 120 scans: "
                     "%u held, %u tiles redrawn, %u new tiles (%u drawn), %u recoloured, "
                     "%u looked up; affine pictures %u held, %u made; by the GPU %u, refused %u\n",
                     g_plane_ticks / 268111.856 / 120, g_plane_hit, g_plane_tiles, g_plane_full,
                     g_plane_draws, g_plane_recolors, g_plane_lookups, g_aff_held, g_aff_made,
                     g_aff_gpu, g_aff_cpu);
        g_plane_ticks = 0;
        g_plane_hit = g_plane_tiles = g_plane_full = g_plane_draws = g_plane_recolors =
            g_plane_lookups = g_aff_held = g_aff_made = g_aff_gpu = g_aff_cpu = 0;
    }
}

// A line of BG `bg`, out of its pictures when it has them this scan
inline void sample_bg(const BgLayer &c, int bg, int y, const uint32_t *pal32, uint32_t *col,
                      uint8_t *ok)
{
    const int mode = g_plane_mode[bg];
    if (!mode) {
        sample_line_fast(c, y, pal32, col, ok);
        return;
    }
    if (mode == 3) {
        aff_line(c, g_aff[bg], y, col, ok);
        return;
    }
    const Plane &P = g_plane[bg];
    const int w = P.map_w * 8;
    const int py = (y + c.vofs) & (P.map_h * 8 - 1);
    const int x0 = c.hofs & (w - 1);
    const int n1 = w - x0 < 256 ? w - x0 : 256;
    const size_t row = (size_t)py * w;
    if (mode == 1) {
        std::memcpy(col, P.px + row + x0, (size_t)n1 * 4);
        std::memcpy(ok, P.ok + row + x0, (size_t)n1);
        if (n1 < 256) {
            std::memcpy(col + n1, P.px + row, (size_t)(256 - n1) * 4);
            std::memcpy(ok + n1, P.ok + row, (size_t)(256 - n1));
        }
        return;
    }
    const uint32_t *lut = g_plane_lut[bg];
    const uint16_t *ix = P.ix + row + x0;
    for (int x = 0; x < 256; ++x) {
        if (x == n1) ix = P.ix + row - x;
        const uint32_t v = ix[x];
        col[x] = lut[v];
        ok[x] = v != 0;
    }
}

// GAME3DS_SUB_PROF=1: a painter's BG line sampling and the rest, every 60
// scans ([subprof] lines; ~400 clock reads a scan)
struct PainterProf {
    const char *name;
    unsigned long long samp, pix, t0, ts, a;
    unsigned calls;
    bool on() const
    {
        static const bool prof = std::getenv("GAME3DS_SUB_PROF") != nullptr;
        return prof;
    }
    void begin() { if (on()) { t0 = svcGetSystemTick(); ts = 0; } }
    void sample_begin() { if (on()) a = svcGetSystemTick(); }
    void sample_end() { if (on()) ts += svcGetSystemTick() - a; }
    void end(const BgLayer *bgs, const int *flat, int nflat)
    {
        if (!on()) return;
        samp += ts;
        pix += svcGetSystemTick() - t0 - ts;
        if (++calls % 60) return;
        std::fprintf(stderr, "[subprof] %s painter per scan: sample %.2f ms, pixels %.2f ms (nflat %d kinds %d %d %d %d)\n",
                     name, samp / 268111.856 / 60, pix / 268111.856 / 60, nflat,
                     nflat > 0 ? bgs[flat[0]].kind : -1, nflat > 1 ? bgs[flat[1]].kind : -1,
                     nflat > 2 ? bgs[flat[2]].kind : -1, nflat > 3 ? bgs[flat[3]].kind : -1);
        samp = pix = 0;
    }
};
PainterProf g_prof_single = {"single"}, g_prof_multi = {"multi"}, g_prof_alpha = {"alpha"};

// Let the GPU draw the minimap's affine BG (sub_affine.h). Reading it through
// its matrix per pixel took 21-35 ms a scan on Old 3DS. The GPU can draw it
// rotated for free, so here we paint only the sprites (0 elsewhere) and
// describe the BG. Only used when it gives the same result: every sprite in
// front of the BG and not blended, no master brightness, and a descriptor for
// this buffer. GAME3DS_SUB_GPU_AFFINE=0 always uses the CPU.
bool scan_single_gpu(SubFramebuffer &fb, const BgLayer &bg, int bg_id, uint32_t backdrop,
                     const Blend &blend, ObjPixel (*objects)[256])
{
    static const bool on = !(std::getenv("GAME3DS_SUB_GPU_AFFINE") && std::getenv("GAME3DS_SUB_GPU_AFFINE")[0] == '0');
    if (!on || !game3ds_sub_affine_begin) return false;
    const AffPlane &A = g_aff[bg_id];
    const bool rows_known = objects == g_obj;
    for (int y = 0; y < SUB_H; ++y) {
        if (rows_known && !g_obj_rows[y]) continue;
        for (int x = 0; x < SUB_W; ++x) {
            const ObjPixel &o = objects[y][x];
            if (o.hit && (o.prio > bg.prio || (o.semi && !blend.off))) {
                ++g_aff_cpu;
                return false;
            }
        }
    }
    SubAffine *d = game3ds_sub_affine_begin(&fb);
    if (!d) return false;
    for (int y = 0; y < SUB_H; ++y) {
        if (rows_known && !g_obj_rows[y]) {
            std::memset(fb.px[y], 0, sizeof fb.px[y]);
            continue;
        }
        for (int x = 0; x < SUB_W; ++x) {
            const ObjPixel &o = objects[y][x];
            fb.px[y][x] = o.hit ? o.color : 0u;
        }
    }
    d->map_w = bg.map_w;
    d->map_h = bg.map_h;
    d->wrap = bg.wrap ? 1 : 0;
    d->refx = bg.refx;
    d->refy = bg.refy;
    d->pa = bg.pa;
    d->pb = bg.pb;
    d->pc = bg.pc;
    d->pd = bg.pd;
    d->backdrop = backdrop | 0xFF000000u;
    d->pic = A.px;
    d->ver = A.ver;
    game3ds_sub_affine_done(&fb, 1);
    ++g_aff_gpu;
    return true;
}

// The gameplay minimap is one BG plus sprites. Resolve that case directly;
// windows and BG colour effects continue through the general layer walk.
template <bool Brightness>
void scan_single_bg(SubFramebuffer &fb, const BgLayer *bg, int bg_id,
                    const uint32_t *palette, uint32_t backdrop,
                    const Blend &blend, const Bright &bright,
                    ObjPixel (*objects)[256])
{
    ++game3ds_sub_paths[0];
    uint32_t colors[256];
    uint8_t hits[256];
    plane_prepare(bg ? bg - bg_id : nullptr, &bg_id, bg ? 1 : 0, palette);   // bg is &bgs[bg_id]
    if (!Brightness && bg && g_plane_mode[bg_id] == 3 && scan_single_gpu(fb, *bg, bg_id, backdrop, blend, objects))
        return;
    g_prof_single.begin();
    for (int y = 0; y < SUB_H; ++y) {
        g_prof_single.sample_begin();
        if (bg) sample_bg(*bg, bg_id, y, palette, colors, hits);
        g_prof_single.sample_end();
        if (objects == g_obj && !g_obj_rows[y]) {   // no sprite pixel on this row
            for (int x = 0; x < SUB_W; ++x) {
                const uint32_t color = bg && hits[x] ? colors[x] : backdrop;
                fb.px[y][x] = Brightness ? apply_bright(color, bright) : color;
            }
            continue;
        }
        for (int x = 0; x < SUB_W; ++x) {
            const bool background = bg && hits[x];
            uint32_t color = background ? colors[x] : backdrop;
            const ObjPixel &object = objects[y][x];
            if (object.hit && (!background || object.prio <= bg->prio)) {
                color = object.semi && !blend.off
                    ? blend_apply(blend, 0x3f, object.color, 4, true,
                                  color, background ? bg_id : 5)
                    : object.color;
            }
            fb.px[y][x] = Brightness ? apply_bright(color, bright) : color;
        }
    }
    g_prof_single.end(bg ? bg - bg_id : nullptr, &bg_id, bg ? 1 : 0);
}

bool scan_single_enabled()
{
    static const bool enabled = std::getenv("GAME3DS_SUB_SINGLE_OFF") == nullptr;
    return enabled;
}

// Keep the background between scans (scan_multi_bg). The title's logo screen is
// two still BGs under a moving star and sparkles, and every scan recomposed all
// the pixels even though only the sprites changed. When every BG is a colour
// picture and nothing the BG pixels depend on has changed (content, scroll,
// order, priorities, backdrop, effects, brightness), reuse the held line and
// only redo the sprite pixels. It's built after two scans in a row with the
// same key, so a fade (new key every scan) never pays for it.
// GAME3DS_SUB_HELD_OFF=1 composes every scan.
struct BgKey {
    int nflat, id[4], pr[4], hofs[4], vofs[4];
    unsigned gen[4];
    uint32_t backdrop, bd;
    int effect, mode, evy, bright, bfactor, bmode;
    unsigned first;
};
struct BgHeld {
    BgKey key, last;              // the picture's key; the last scan's
    bool valid;
    uint32_t out[SUB_H][SUB_W];   // the pixel with no sprite over it, finished
    uint32_t c[SUB_H][SUB_W];     // its top BG's colour (the backdrop where none)
    uint8_t pt[SUB_H][SUB_W];     // that layer's priority << 4 | its id (4 | 5 the backdrop)
    bool strips_ok;               // `strips` are the sky strips of this key (sky_strips)
    uint32_t strips[SUB_H * 64];
};
BgHeld *g_held;
unsigned g_held_used, g_held_made;   // GAME3DS_SUB_PROF, per 60 multi scans

// The held background for this scan's key (`use`: it is whole), one to make
// this scan, or null (its lines composed)
BgHeld *bg_held(const BgKey &key, bool &use)
{
    use = false;
    static const bool off = std::getenv("GAME3DS_SUB_HELD_OFF") != nullptr;
    if (off) return nullptr;
    if (!g_held) {
        g_held = static_cast<BgHeld *>(std::calloc(1, sizeof(BgHeld)));
        if (!g_held) return nullptr;
    }
    BgHeld *h = g_held;
    const bool still = std::memcmp(&key, &h->last, sizeof key) == 0;
    h->last = key;
    if (h->valid && std::memcmp(&key, &h->key, sizeof key) == 0) {
        use = true;
        ++g_held_used;
        return h;
    }
    h->valid = h->strips_ok = false;
    if (!still) return nullptr;   // made once it holds still
    h->key = key;
    ++g_held_made;
    return h;
}

// Free the caches on a scene change (game3ds_sub_release). Each plane grew to
// the biggest map it ever held and never shrank, so the title's 512x512 maps
// stayed around through the level, ~5 MB, which ran the Old 3DS out of heap.
// Only call this with no scan running. Returns the bytes freed.
size_t sub_caches_release()
{
    size_t bytes = 0;
    for (Plane &P : g_plane) bytes += plane_free(P);
    for (AffPlane &A : g_aff) {
        bytes += A.cap * (sizeof *A.px + sizeof *A.ok) + A.map_cap + A.chr_cap + A.pal_cap;
        std::free(A.map); std::free(A.chr); std::free(A.pal); std::free(A.px); std::free(A.ok);
        A.map = A.chr = A.pal = A.ok = nullptr; A.px = nullptr;
        A.cap = A.map_cap = A.chr_cap = A.pal_cap = 0;
        A.saved = A.valid = A.changed_last = false;
    }
    if (g_held) bytes += sizeof *g_held;
    std::free(g_held);
    g_held = nullptr;
    return bytes;
}

// The sky beyond the picture, for the menu side bars (card3ds fill_bars). At
// 1.25x on the 400-wide top screen a 256-wide menu leaves 32 DS pixels a side.
// If one BG is held whole this scan, draw its columns -32..-1 and 256..287
// (its map wraps, so it shows its own continuation) into strips with the
// screen's brightness effects, that BG only, never the logo, text or sprites.
// Use the frontmost BG that's opaque across the strips (not the backmost: the
// logo BG is behind the sky during the star fly-in). Otherwise use the
// backmost BG with the backdrop where it's transparent.
//
// The strips are only used if they match the picture's edge columns closely
// (the sky's dithered gradient does); otherwise the bars stay as before.
// Idea borrowed from another SM64DS 3DS port's "full-width sky" (MIT).
// GAME3DS_SKY_BARS=0 turns it off.
// (Weak, since the smoke tests don't link sub_async.cpp.)
extern "C" uint32_t *game3ds_sub_strips_begin(const void *fb) __attribute__((weak));
extern "C" void game3ds_sub_strips_done(const void *fb, int ok) __attribute__((weak));

// The plane offset of strip pixel (i, y) of BG `bg` (its picture this scan)
inline size_t strip_at(const BgLayer &c, const Plane &P, int i, int y)
{
    const int w = P.map_w * 8, h = P.map_h * 8;
    return (size_t)((y + c.vofs) & (h - 1)) * w + (size_t)((c.hofs + (i < 32 ? i - 32 : 224 + i)) & (w - 1));
}

// BG `bg` opaque in every strip pixel (a transparent one is usually met in the
// first rows: the logo's BG is out at once)
bool strip_covered(const BgLayer &c, int bg)
{
    const int mode = g_plane_mode[bg];
    if (mode != 1 && mode != 2) return false;   // a text picture (an affine BG is no sky)
    const Plane &P = g_plane[bg];
    for (int y = 0; y < SUB_H; ++y)
        for (int i = 0; i < 64; ++i) {
            const size_t at = strip_at(c, P, i, y);
            if (mode == 1 ? !P.ok[at] : !P.ix[at]) return false;
        }
    return true;
}

template <bool Brightness>
void sky_strips(const SubFramebuffer &fb, const BgLayer *bgs, const int *flat, int nflat, bool effect,
                const uint32_t *fx, uint32_t bd, const Blend &blend, const Bright &bright, BgHeld *held, bool use)
{
    static const bool off = std::getenv("GAME3DS_SKY_BARS") && std::atoi(std::getenv("GAME3DS_SKY_BARS")) == 0;
    if (off || !game3ds_sub_strips_begin || !nflat) return;
    // GAME3DS_SUB_PROF=1: how they went, every 60 scans
    static const bool prof = std::getenv("GAME3DS_SUB_PROF") != nullptr;
    static unsigned why[4], calls;   // kept, no picture of the BG, seam left, seam right
    auto tally = [&](int k) {
        ++why[k];
        if (!prof || ++calls % 60) return;
        std::fprintf(stderr, "[subprof] sky strips per 60 scans: kept %u, no plane %u, seam left %u, "
                     "seam right %u\n", why[0], why[1], why[2], why[3]);
        for (unsigned &w : why) w = 0;
    };
    uint32_t *out = game3ds_sub_strips_begin(&fb);
    if (!out) return;   // begin left them not ok
    constexpr int kSeam = 24;
    auto near = [](uint32_t a, uint32_t b) {
        for (int sh = 0; sh < 24; sh += 8) {
            const int d = (int)((a >> sh) & 0xFF) - (int)((b >> sh) & 0xFF);
            if (d > kSeam || d < -kSeam) return false;
        }
        return true;
    };
    // they meet the picture's edge columns (which sprites may cover)
    auto seams = [&]() {
        int seam[2] = {0, 0};
        for (int y = 0; y < SUB_H; ++y) {
            seam[0] += near(out[y * 64 + 31], fb.px[y][0]);
            seam[1] += near(out[y * 64 + 32], fb.px[y][SUB_W - 1]);
        }
        const bool left = seam[0] * 5 >= SUB_H * 4, right = seam[1] * 5 >= SUB_H * 4;
        tally(!left ? 2 : !right ? 3 : 0);
        game3ds_sub_strips_done(&fb, left && right);
    };
    if (use && held->strips_ok) {   // the held background's (BgHeld)
        std::memcpy(out, held->strips, sizeof held->strips);
        return seams();
    }
    int bg = flat[nflat - 1];
    for (int k = 0; k < nflat - 1; ++k)
        if (strip_covered(bgs[flat[k]], flat[k])) {
            bg = flat[k];
            break;
        }
    const int mode = g_plane_mode[bg];
    if (mode != 1 && mode != 2) return tally(1);
    const Plane &P = g_plane[bg];
    const BgLayer &c = bgs[bg];
    const uint32_t *lut = g_plane_lut[bg];
    const bool fx_on = effect && ((blend.first >> bg) & 1u);
    for (int y = 0; y < SUB_H; ++y) {
        uint32_t *o = out + y * 64;
        for (int i = 0; i < 64; ++i) {
            const size_t at = strip_at(c, P, i, y);
            uint32_t col;
            bool hit;
            if (mode == 1) {
                hit = P.ok[at];
                col = P.px[at];
            } else {
                const uint32_t v = P.ix[at];
                hit = v != 0;
                col = lut[v];
            }
            if (!hit)
                col = bd;
            else if (fx_on)
                col = 0xFF000000u | fx[((col >> 16) & 0xFF) >> 3] << 16 | fx[((col >> 8) & 0xFF) >> 3] << 8 |
                      fx[(col & 0xFF) >> 3];
            o[i] = Brightness ? apply_bright(col, bright) : col;
        }
    }
    if (held) {
        std::memcpy(held->strips, out, sizeof held->strips);
        held->strips_ok = true;
    }
    seams();
}

// The menus' case: several BGs and sprites, no window, and no blending between
// layers (BLDCNT mode 0, or brightness modes 2/3 which only touch the top
// layer). The title, file select and opening are like this, and the general
// walk cost ~21 ms a frame there on hardware.
//
// Same result as scan_fast's walk:
//   - each pixel holds the first opaque BG in visit order, with its priority
//     and id (none: backdrop, id 5, priority 4)
//   - a sprite pixel goes on top when its priority isn't behind that BG's
//   - a semi-transparent sprite on top blends with the layer under it
//   - brightness modes then apply to the top layer by id, and the backdrop
//     gets backdrop_effect
// Mode 1 (alpha blending) and windows use the general walk. One pass per line
// after the BG lines are sampled.
template <bool Brightness>
void scan_multi_bg(SubFramebuffer &fb, const BgLayer *bgs, const int *flat, const int *fprio,
                   int nflat, const uint32_t *palette, uint32_t backdrop, const Blend &blend,
                   const Bright &bright, ObjPixel (*objects)[256])
{
    ++game3ds_sub_paths[1];
    // BLDY 0 changes nothing: every colour here is bgr555-expanded, and the
    // effect's 5-bit round trip of one returns it unchanged.
    const bool effect = !blend.off && (blend.mode == 2 || blend.mode == 3) && blend.evy != 0;
    const uint32_t bd = effect ? backdrop_effect(blend, backdrop) : backdrop;
    // Brightness modes work on each 5-bit channel on its own (blend_apply's math):
    // fx[v] is what a channel of value v becomes, expanded to 8 bits, so an
    // affected pixel is three lookups.
    uint32_t fx[32];
    if (effect)
        for (int v = 0; v < 32; ++v) {
            int r = v;
            if (blend.mode == 2) r += ((31 - r) * blend.evy) >> 4;
            else r -= (r * blend.evy) >> 4;
            if (r > 31) r = 31;
            fx[v] = (uint32_t)(r << 3 | r >> 2);
        }
    uint32_t col[4][256];
    uint8_t ok[4][256];
    int id[4], pr[4];
    for (int k = 0; k < nflat; ++k) {
        id[k] = flat[k];
        pr[k] = fprio[k];
    }
    // Everything faded to black (mode 3 at BLDY 16 on every target), like the
    // title's bottom screen behind its star and TOUCH TO START. Then any pixel
    // without a sprite is black regardless of the BGs, and an opaque sprite in
    // front of every BG is just itself, so lines whose sprites are all like that
    // skip BG sampling. GAME3DS_SUB_DARK_OFF=1 samples every line.
    static const bool dark_off = std::getenv("GAME3DS_SUB_DARK_OFF") != nullptr;
    bool dark = !dark_off && effect && blend.mode == 3 && bd == 0xFF000000u;
    for (int v = 0; dark && v < 32; ++v) dark = fx[v] == 0;
    unsigned front = 4;
    for (int k = 0; dark && k < nflat; ++k) {
        dark = (blend.first >> id[k]) & 1u;
        if ((unsigned)pr[k] < front) front = (unsigned)pr[k];
    }
    const uint32_t dark_px = Brightness ? apply_bright(0xFF000000u, bright) : 0xFF000000u;
    const bool obj_dark = (blend.first >> 4) & 1u;   // the sprites are a 1st target too
    plane_prepare(bgs, flat, nflat, palette);
    // the held background (BgHeld): every BG a colour picture this scan
    BgHeld *held = nullptr;
    bool use = false;
    if (!dark) {
        bool pictures = true;
        for (int k = 0; k < nflat; ++k) pictures = pictures && g_plane_mode[id[k]] == 1;
        if (pictures) {
            BgKey key;
            std::memset(&key, 0, sizeof key);
            key.nflat = nflat;
            for (int k = 0; k < nflat; ++k) {
                key.id[k] = id[k];
                key.pr[k] = pr[k];
                key.hofs[k] = bgs[id[k]].hofs;
                key.vofs[k] = bgs[id[k]].vofs;
                key.gen[k] = g_plane[id[k]].gen;
            }
            key.backdrop = backdrop;
            key.bd = bd;
            key.effect = effect;
            key.mode = blend.mode;
            key.evy = blend.evy;
            key.first = blend.first;
            key.bright = Brightness;
            key.bfactor = bright.factor;
            key.bmode = bright.mode;
            held = bg_held(key, use);
        }
    }
    // a pixel finished: the effect on its top layer `ti` (5 the backdrop),
    // then the master brightness
    auto finish = [&](uint32_t c, unsigned ti) {
        if (effect) {
            if (ti == 5)
                c = bd;
            else if (ti < 5 && (blend.first & (1u << ti)))
                c = 0xFF000000u | fx[((c >> 16) & 0xFF) >> 3] << 16 | fx[((c >> 8) & 0xFF) >> 3] << 8 |
                    fx[(c & 0xFF) >> 3];
        }
        return Brightness ? apply_bright(c, bright) : c;
    };
    // sprite pixel o over the top BG (colour c, priority tp, id ti), finished
    auto over = [&](const ObjPixel &o, uint32_t c, unsigned tp, unsigned ti) {
        if (o.prio <= tp) {
            if (o.semi && !blend.off) {
                // blended now; no brightness on top of it (blend_apply's order)
                c = blend_apply(blend, 0x3f, o.color, 4, true, c, (int)ti);
                ti = 6;
            } else {
                c = o.color;
                ti = 4;
            }
        }
        return finish(c, ti);
    };
    g_prof_multi.begin();
    for (int y = 0; y < SUB_H; ++y) {
        if (dark) {
            const ObjPixel *orow = objects == g_obj && !g_obj_rows[y] ? nullptr : objects[y];
            bool plain = true;
            for (int x = 0; orow && plain && x < SUB_W; ++x)
                plain = !orow[x].hit || (!orow[x].semi && orow[x].prio <= front);
            if (plain) {
                uint32_t *dst = fb.px[y];
                for (int x = 0; x < SUB_W; ++x) {
                    if (!orow || !orow[x].hit) {
                        dst[x] = dark_px;
                        continue;
                    }
                    const uint32_t c = obj_dark ? 0xFF000000u : orow[x].color;
                    dst[x] = Brightness ? apply_bright(c, bright) : c;
                }
                continue;
            }
        }
        const ObjPixel *row = objects == g_obj && !g_obj_rows[y] ? nullptr : objects[y];
        uint32_t *dst = fb.px[y];
        if (use) {
            std::memcpy(dst, held->out[y], sizeof held->out[y]);
            if (!row) continue;
            const uint32_t *hc = held->c[y];
            const uint8_t *hp = held->pt[y];
            for (int x = 0; x < SUB_W; ++x)
                if (row[x].hit && row[x].prio <= (unsigned)(hp[x] >> 4))
                    dst[x] = over(row[x], hc[x], hp[x] >> 4, hp[x] & 15u);
            continue;
        }
        g_prof_multi.sample_begin();
        for (int k = 0; k < nflat; ++k) sample_bg(bgs[flat[k]], flat[k], y, palette, col[k], ok[k]);
        g_prof_multi.sample_end();
        for (int x = 0; x < SUB_W; ++x) {
            uint32_t c = backdrop;
            unsigned tp = 4, ti = 5;
            for (int k = 0; k < nflat; ++k)
                if (ok[k][x]) {
                    c = col[k][x];
                    tp = (unsigned)pr[k];
                    ti = (unsigned)id[k];
                    break;
                }
            if (held) {
                held->c[y][x] = c;
                held->pt[y][x] = (uint8_t)(tp << 4 | ti);
                held->out[y][x] = finish(c, ti);
            }
            dst[x] = row && row[x].hit ? over(row[x], c, tp, ti) : finish(c, ti);
        }
    }
    if (held) held->valid = true;
    if (g_prof_multi.on() && (g_prof_multi.calls + 1) % 60 == 0) {
        std::fprintf(stderr, "[subprof] held background per 60 multi scans: used %u, made %u\n", g_held_used,
                     g_held_made);
        g_held_used = g_held_made = 0;
    }
    g_prof_multi.end(bgs, flat, nflat);
    sky_strips<Brightness>(fb, bgs, flat, nflat, effect, fx, bd, blend, bright, held, use);
}

// Alpha mode with no window (BLDCNT mode 1). The opening cutscene fades its
// bottom-screen text panel this way (four BGs and sprites), and the general
// walk took 33-39 ms a frame on hardware.
//
// Same result as the walk: find the first and second opaque layers in visit
// order (sprite placed before the first BG it isn't behind), the backdrop
// where there's nothing, then blend_apply(top, second):
//   - BGs give their first two opaque layers in order (b0, b1)
//   - a sprite is on top if there's no opaque BG or it isn't behind b0, and
//     then b0 (or the backdrop) is second; otherwise it's second if there's no
//     b1 or it isn't behind b1
//   - no layer at all: bd
// One pass per line after the BG lines are sampled.
template <bool Brightness>
void scan_multi_alpha(SubFramebuffer &fb, const BgLayer *bgs, const int *flat, const int *fprio,
                      int nflat, const uint32_t *palette, uint32_t backdrop, uint32_t bd,
                      const Blend &blend, const Bright &bright, ObjPixel (*objects)[256])
{
    ++game3ds_sub_paths[2];
    uint32_t col[4][256];
    uint8_t ok[4][256];
    int id[4], pr[4];
    for (int k = 0; k < nflat; ++k) {
        id[k] = flat[k];
        pr[k] = fprio[k];
    }
    // blend_alpha's arithmetic as tables: a 5-bit channel times EVA / EVB,
    // and (sum >> 4) clamped to 31 and expanded to 8 bits
    int ea[32], eb[32];
    uint8_t clamp8[63];
    for (int v = 0; v < 32; ++v) {
        ea[v] = v * blend.eva;
        eb[v] = v * blend.evb;
    }
    for (int s = 0; s < 63; ++s) {
        const int v = s > 31 ? 31 : s;
        clamp8[s] = (uint8_t)(v << 3 | v >> 2);
    }
    plane_prepare(bgs, flat, nflat, palette);
    g_prof_alpha.begin();
    for (int y = 0; y < SUB_H; ++y) {
        g_prof_alpha.sample_begin();
        for (int k = 0; k < nflat; ++k) sample_bg(bgs[flat[k]], flat[k], y, palette, col[k], ok[k]);
        g_prof_alpha.sample_end();
        const ObjPixel *row = objects == g_obj && !g_obj_rows[y] ? nullptr : objects[y];
        uint32_t *dst = fb.px[y];
        for (int x = 0; x < SUB_W; ++x) {
            // the first two opaque BGs of the visit order (none: the backdrop,
            // id 5, priority 4 -- an id 5 in second place means one BG, at most)
            uint32_t c0 = backdrop, c1 = backdrop;
            unsigned i0 = 5, i1 = 5, p0 = 4, p1 = 4;
            int k = 0;
            for (; k < nflat; ++k)
                if (ok[k][x]) {
                    c0 = col[k][x];
                    i0 = (unsigned)id[k];
                    p0 = (unsigned)pr[k];
                    break;
                }
            for (++k; k < nflat; ++k)
                if (ok[k][x]) {
                    c1 = col[k][x];
                    i1 = (unsigned)id[k];
                    p1 = (unsigned)pr[k];
                    break;
                }
            bool semi = false;
            if (row) {
                const ObjPixel &o = row[x];
                if (o.hit) {
                    if (i0 == 5 || o.prio <= p0) {
                        c1 = c0; i1 = i0;
                        c0 = o.color; i0 = 4; semi = o.semi;
                    } else if (i1 == 5 || o.prio <= p1) {
                        c1 = o.color; i1 = 4;
                    }
                }
            }
            // blend_apply for mode 1 with effects on and no window, using the tables: a
            // semi-transparent sprite on top blends whenever the layer below is a 2nd
            // target; any other top layer only when it's also a 1st target. Pixels no
            // layer drew are bd.
            uint32_t c = c0;
            if (i0 == 5) {
                c = bd;
            } else if ((blend.second >> i1) & 1u && (semi || ((blend.first >> i0) & 1u))) {
                const uint32_t b = c1;
                c = 0xFF000000u |
                    (uint32_t)clamp8[(ea[((c >> 16) & 0xFF) >> 3] + eb[((b >> 16) & 0xFF) >> 3]) >> 4] << 16 |
                    (uint32_t)clamp8[(ea[((c >> 8) & 0xFF) >> 3] + eb[((b >> 8) & 0xFF) >> 3]) >> 4] << 8 |
                    (uint32_t)clamp8[(ea[(c & 0xFF) >> 3] + eb[(b & 0xFF) >> 3]) >> 4];
            }
            dst[x] = Brightness ? apply_bright(c, bright) : c;
        }
    }
    g_prof_alpha.end(bgs, flat, nflat);
}

bool scan_multi_enabled()
{
    static const bool enabled = std::getenv("GAME3DS_SUB_MULTI_OFF") == nullptr;
    return enabled;
}
