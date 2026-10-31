// 60 fps interpolation on the GPU record.
//
// An in-between frame draws tick N's record as-is (vertices, indices,
// textures, 2D) with each matrix blended halfway between its tick N-1 and
// tick N values. Only the matrices are worked out here; card3ds draws the
// record twice, in-between then tick.
//
// Pairing: each matrix id's vertices are one contiguous span in the record,
// and their raw model coordinates are the same every tick for the same object.
// So the span's hash (plus length and projection type) is the key. Ids that
// share a key (coins, trees, text) are told apart by position.
//
// Blending: gx_take_record gives each id's position matrix (pos) and its
// projection/viewport matrix (pws). For a paired id, lerp pos's rotation rows
// and rescale them to the lerped length (a plain lerp of two rotations
// shrinks), lerp the translation, lerp pws. An unpaired id (new, vertex
// animated, or a jump) is moved by half the camera's step instead.
//
// No in-between (the tick is shown twice) when there's no previous record,
// under 85% of vertices pair, or the median paired centroid moved over 40 px
// (a camera cut). A pick among same-key ids that moved over 64 px counts as
// unpaired.
//
// World pairing (default): judging by screen position breaks when the camera
// moves, since everything slides. So read the game's view matrix
// (data_0209b3ec) each tick and pair in world space:
//   - the key also includes the display list the vertices came from and the
//     shape of the matrix commands before them
//   - same-key perspective ids pair by world distance, closest first, so a still
//     object always matches itself; draw order only breaks exact ties
//   - an origin that jumped more than kTeleport is a teleport: unpaired
//   - the camera's step and half step come from the view matrix; a cut is the
//     eye moving over kCutUnits or turning over kCutDeg, and more than twice
//     as far as the tick before
// Ortho ids (2D) pair on screen as before. A tick without a usable view falls
// back to screen pairing. GAME3DS_INTERP_PAIR=5f or =raw picks the older
// methods.
//
// GAME3DS_INTERP_STATS=1 prints a summary every 120 records.

#include "interp3ds.h"

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr float kFx12 = 1.0f / 4096.0f;
constexpr float kMinShare = 0.85f;     // vertices paired, below: no in-between
constexpr float kCutPx = 40.0f;        // median origin move, above: a cut
constexpr float kJumpPx = 64.0f;       // one pair's move, above: unpaired
constexpr float kNearPx = 1024.0f;     // the moves above count within this of the screen
constexpr float kSuspectPx = 12.0f;    // a pair's miss from the camera's prediction, above: suspect
constexpr float kOrderPx = 32.0f;      // pairing by draw order holds while every pair is within this
constexpr size_t kSortCands = 4096;    // a key's candidates, above: paired greedily, not sorted
// World pairing thresholds in view matrix units (from the PC port, measured
// across every level: a following camera moved at most 17.9 and turned at most
// 12 degrees a tick, and anything following play moved under 64).
constexpr float kTeleport = 64.0f;     // a pair's world origin moved over this: unpaired
constexpr double kCutUnits = 40.0;     // the eye's step, above (and jumping): a cut
constexpr double kCutDeg = 18.0;       // the view axis' turn, above (and jumping): a cut

enum PairMode { PAIR_WORLD, PAIR_5F, PAIR_RAW };

PairMode pair_mode()
{
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("GAME3DS_INTERP_PAIR");
        m = !e ? PAIR_WORLD : !strcmp(e, "raw") ? PAIR_RAW : !strcmp(e, "5f") ? PAIR_5F : PAIR_WORLD;
    }
    return (PairMode)m;
}

// a position matrix's row length, as blend_pos measures it
inline float row_len(const float *r)
{
    return sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
}

// a point near enough to the screen for its move to mean anything (one far
// off the side or near the eye plane swings by any amount)
bool near_screen(float sx, float sy)
{
    return fabsf(sx) < kNearPx && fabsf(sy) < kNearPx;
}

// on the screen or within about half a screen of it (the 400-wide build
// included): the points whose median move tells a camera cut from a turn
bool in_view(float sx, float sy)
{
    return fabsf(sx - 128.0f) < 320.0f && fabsf(sy - 96.0f) < 240.0f;
}

struct IdInfo {
    uint32_t key;      // the span's raw x, y, z, its length, the projection kind
    uint32_t nverts;
    float sx, sy;      // its vertices' centroid on the screen (X/w, Y/w), or huge
    float px, py;      // tick N-1 ids: that centroid where tick N's camera would
                       // show it if it stood still (sx, sy without the camera)
    float cx, cy, cz;  // the sum, then the mean, of its raw x, y, z
    uint8_t persp;
    bool used;         // paired already (as a tick N-1 candidate)
    bool contiguous;   // its vertices are one run of the record's (first ..)
    uint32_t first;    // its first vertex
    float wx, wy, wz;  // the world pairing: its centroid in the world
    float ox, oy, oz;  // and its origin (row 3 of pos x V^-1)
    float len[3];      // the lengths of pos's rows 0-2 (blend_pos's, computed once an id)
    const ntr::GxRecRaw *raw;   // its factors, in g_cur_raw / g_prev_raw
};

std::vector<IdInfo> g_prev, g_cur;
std::vector<int16_t> g_prev_xyz, g_cur_xyz;   // each tick's raw x, y, z, 3 a vertex
// each tick's GxRecRaw, taken from the record by a swap (not copied an id at a
// time); g_no_raw stands in for a record without them
std::vector<ntr::GxRecRaw> g_prev_raw, g_cur_raw;
const ntr::GxRecRaw g_no_raw = {};
std::vector<uint8_t> g_mesh;                  // per current id: paired as a mesh
std::vector<int> g_pair;         // per current id: its tick N-1 partner, or -1
std::vector<float> g_moves;
bool g_have_prev;
bool g_prev_world;   // tick N-1 was described for the world pairing (its keys)

// The game's view matrix, data_0209b3ec (4.12 fixed point, row vectors), as a
// 4x4 in pos's units. Weak because the card's test program has no game linked;
// there every tick falls back to screen pairing.
float g_view_cur[16], g_view_prev[16];
bool g_view_cur_ok, g_view_prev_ok;
double g_prev_step, g_prev_turn, g_prev_disp[3];   // the camera's tick before (the cut test)

struct Totals {
    unsigned ticks, blended, none_first, none_few, none_cut;
    unsigned long ids, by_key, by_pos, by_mesh, fresh, verts, verts_paired, cam_ref, suspect, greedy;
    unsigned long by_order;   // keys paired by draw order
    unsigned long world, teleport;   // ticks paired in the world; pairs refused as teleports
    unsigned long long cpu;          // system ticks in interp3ds_take
    unsigned long long ph[4];        // of which: describe, pair, mesh pairs, blend
    std::vector<float> medians;
    std::vector<float> drift;        // the largest still id's world origin, tick to tick
} g_t;

}  // namespace

extern "C" int data_0209b3ec[12] __attribute__((weak));

namespace {

// the view out of `src` (data_0209b3ec as interp3ds_capture_view took it);
// false when there is none or it is not a usable affine matrix
bool read_view(float *v, const int32_t *src)
{
    if (!src) return false;
    float n = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) {
            v[r * 4 + c] = (float)src[r * 3 + c] * kFx12;
            if (r < 3) n += v[r * 4 + c] * v[r * 4 + c];
        }
        v[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
    }
    return n > 0.5f;   // the rotation rows are there (unit rows: 3)
}

// the eye (-t R^T for an orthonormal R) and the view axis (R's column 2)
void view_eye(const float *v, double eye[3], double fwd[3])
{
    for (int j = 0; j < 3; ++j) {
        eye[j] = -((double)v[12] * v[j * 4] + (double)v[13] * v[j * 4 + 1] + (double)v[14] * v[j * 4 + 2]);
        fwd[j] = v[j * 4 + 2];
    }
}

// the PC port's cut test (gx_interp_commit): a jump, not a speed
bool view_cut(const float *vp, const float *vc)
{
    double e0[3], f0[3], e1[3], f1[3];
    view_eye(vp, e0, f0);
    view_eye(vc, e1, f1);
    const double dx = e1[0] - e0[0], dy = e1[1] - e0[1], dz = e1[2] - e0[2];
    const double step = sqrt(dx * dx + dy * dy + dz * dz);
    double dot = f0[0] * f1[0] + f0[1] * f1[1] + f0[2] * f1[2];
    const double l0 = sqrt(f0[0] * f0[0] + f0[1] * f0[1] + f0[2] * f0[2]);
    const double l1 = sqrt(f1[0] * f1[0] + f1[1] * f1[1] + f1[2] * f1[2]);
    if (l0 > 0 && l1 > 0) dot /= l0 * l1;
    dot = dot > 1 ? 1 : dot < -1 ? -1 : dot;
    const double turn = acos(dot) * 57.29577951308232;
    double hd = 1.0;
    const double pl = sqrt(g_prev_disp[0] * g_prev_disp[0] + g_prev_disp[1] * g_prev_disp[1] +
                           g_prev_disp[2] * g_prev_disp[2]);
    if (pl > 1e-9 && step > 1e-9)
        hd = (dx * g_prev_disp[0] + dy * g_prev_disp[1] + dz * g_prev_disp[2]) / (pl * step);
    const bool jump = step > kCutUnits && (step > 2.0 * g_prev_step || hd < 0.5 || step > 10.0 * kCutUnits);
    const bool spin = turn > kCutDeg && (turn > 2.0 * g_prev_turn || turn > 60.0);
    g_prev_step = step;
    g_prev_turn = turn;
    g_prev_disp[0] = dx;
    g_prev_disp[1] = dy;
    g_prev_disp[2] = dz;
    return jump || spin;
}

void view_forget()
{
    g_prev_step = g_prev_turn = 0.0;
    g_prev_disp[0] = g_prev_disp[1] = g_prev_disp[2] = 0.0;
}

void describe(const ntr::GxRecord &rec, std::vector<IdInfo> &out, bool world, const float *vinv)
{
    const bool have_raw = g_cur_raw.size() == rec.mtx.size();
    out.resize(rec.mtx.size());
    for (size_t k = 0; k < out.size(); ++k) {
        IdInfo &d = out[k];
        d.key = 2166136261u;
        d.nverts = 0;
        d.sx = d.sy = 1e9f;
        d.px = d.py = 1e9f;
        d.cx = d.cy = d.cz = 0.0f;
        d.persp = rec.mtx[k].persp;
        d.used = false;
        d.contiguous = true;
        d.first = 0;
        d.raw = have_raw ? &g_cur_raw[k] : &g_no_raw;
        for (int r = 0; r < 3; ++r) d.len[r] = row_len(d.raw->pos + r * 4);
    }
    g_cur_xyz.resize(rec.v.size() * 3);
    // The recorder already kept each id's hash, sum, count and first vertex, using
    // the same math in the same order as the walk below, so only the raw x, y, z
    // need copying here (for next tick's mesh pairs).
    const bool stats = have_raw && !out.empty() && g_cur_raw[0].stats;
    if (stats) {
        for (size_t k = 0; k < out.size(); ++k) {
            IdInfo &d = out[k];
            d.key = d.raw->vhash;
            d.nverts = d.raw->nverts;
            d.first = d.raw->first;
            d.cx = d.raw->csum[0];
            d.cy = d.raw->csum[1];
            d.cz = d.raw->csum[2];
        }
        int16_t *o = g_cur_xyz.data();
        for (const ntr::GxRecVertex &v : rec.v) {
            o[0] = v.x;
            o[1] = v.y;
            o[2] = v.z;
            o += 3;
        }
    }
    for (size_t vi = 0; !stats && vi < rec.v.size(); ++vi) {
        const ntr::GxRecVertex &v = rec.v[vi];
        g_cur_xyz[vi * 3] = v.x;
        g_cur_xyz[vi * 3 + 1] = v.y;
        g_cur_xyz[vi * 3 + 2] = v.z;
        const unsigned id = v.id & 0x7FFFu;
        if (id >= out.size()) continue;
        IdInfo &d = out[id];
        if (!d.nverts) d.first = (uint32_t)vi;
        else if (vi != d.first + d.nverts) d.contiguous = false;
        uint32_t h = d.key;
        h = (h ^ (uint16_t)v.x) * 16777619u;
        h = (h ^ (uint16_t)v.y) * 16777619u;
        h = (h ^ (uint16_t)v.z) * 16777619u;
        d.key = h;
        d.cx += v.x;
        d.cy += v.y;
        d.cz += v.z;
        ++d.nverts;
    }
    for (size_t k = 0; k < out.size(); ++k) {
        IdInfo &d = out[k];
        d.key = (d.key ^ d.nverts) * 16777619u;
        d.key = (d.key ^ d.persp) * 16777619u;
        // the world pairing's keys: the display list and the matrix shape
        // (GAME3DS_INTERP_KEY=vert leaves them out, for a measurement)
        static const bool vert_keys = getenv("GAME3DS_INTERP_KEY") && !strcmp(getenv("GAME3DS_INTERP_KEY"), "vert");
        if (world && !vert_keys && have_raw) {
            d.key = (d.key ^ d.raw->src) * 16777619u;
            d.key = (d.key ^ d.raw->shape) * 16777619u;
        }
        // Use the centroid, not the model origin: a level model's pieces share one
        // origin that can be far off screen or near the eye, where a small camera turn
        // moves it thousands of pixels.
        if (d.nverts) {
            const float n = (float)d.nverts;
            d.cx /= n;
            d.cy /= n;
            d.cz /= n;
        }
        const float *m = rec.mtx[k].m;
        const float w = d.cx * m[3] + d.cy * m[7] + d.cz * m[11] + m[15];
        if (w > 1e-6f) {
            d.sx = (d.cx * m[0] + d.cy * m[4] + d.cz * m[8] + m[12]) / w;
            d.sy = (d.cx * m[1] + d.cy * m[5] + d.cz * m[9] + m[13]) / w;
        }
        // in the world: the centroid through pos (x/4096, as fold's) and V^-1,
        // and the origin, pos's row 3 through V^-1
        d.wx = d.wy = d.wz = d.ox = d.oy = d.oz = 0.0f;
        if (world && d.persp && have_raw) {
            const float *a = d.raw->pos;
            const float x = d.cx * kFx12, y = d.cy * kFx12, z = d.cz * kFx12;
            float vv[3], oo[3];
            for (int j = 0; j < 3; ++j) {
                vv[j] = x * a[j] + y * a[4 + j] + z * a[8 + j] + a[12 + j];
                oo[j] = a[12 + j];
            }
            float wv[3], wo[3];
            for (int j = 0; j < 3; ++j) {
                wv[j] = vv[0] * vinv[j] + vv[1] * vinv[4 + j] + vv[2] * vinv[8 + j] + vinv[12 + j];
                wo[j] = oo[0] * vinv[j] + oo[1] * vinv[4 + j] + oo[2] * vinv[8 + j] + vinv[12 + j];
            }
            d.wx = wv[0];
            d.wy = wv[1];
            d.wz = wv[2];
            d.ox = wo[0];
            d.oy = wo[1];
            d.oz = wo[2];
        }
    }
}

// r = a x b, 4x4, row-vector form (m[row * 4 + col])
void mat_mul(float *r, const float *a, const float *b)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] +
                           a[i * 4 + 3] * b[12 + j];
}

// The halfway position matrix: rows 0-2 lerped and scaled back to the lerp of
// their lengths (la, lb: row_len of a's and b's rows), row 3 lerped.
void blend_pos_len(float *o, const float *a, const float *b, const float *la, const float *lb)
{
    for (int i = 0; i < 16; ++i) o[i] = 0.5f * (a[i] + b[i]);
    for (int r = 0; r < 3; ++r) {
        float *x = o + r * 4;
        const float lx = row_len(x);
        if (lx > 1e-12f) {
            const float k = 0.5f * (la[r] + lb[r]) / lx;
            x[0] *= k;
            x[1] *= k;
            x[2] *= k;
        }
    }
}

void blend_pos(float *o, const float *a, const float *b)
{
    for (int i = 0; i < 16; ++i) o[i] = 0.5f * (a[i] + b[i]);
    for (int r = 0; r < 3; ++r) {
        float *x = o + r * 4;
        const float *p = a + r * 4, *q = b + r * 4;
        const float la = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        const float lb = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        const float lx = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (lx > 1e-12f) {
            const float k = 0.5f * (la + lb) / lx;
            x[0] *= k;
            x[1] *= k;
            x[2] *= k;
        }
    }
}

// The inverse of an affine row-vector matrix [A 0; t 1]: [A^-1 0; -t A^-1 1].
// False when it is not affine or A is singular.
bool affine_inverse(float *o, const float *m)
{
    if (fabsf(m[3]) > 1e-6f || fabsf(m[7]) > 1e-6f || fabsf(m[11]) > 1e-6f ||
        fabsf(m[15] - 1.0f) > 1e-6f)
        return false;
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
                i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (fabsf(det) < 1e-20f) return false;
    const float k = 1.0f / det;
    // the inverse 3x3 (adjugate / det), rows as m's
    const float inv[9] = {A * k, -(b * i - c * h) * k, (b * f - c * e) * k,
                          B * k, (a * i - c * g) * k,  -(a * f - c * d) * k,
                          C * k, -(a * h - b * g) * k, (a * e - b * d) * k};
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 3; ++col) o[r * 4 + col] = inv[r * 3 + col];
        o[r * 4 + 3] = 0.0f;
    }
    for (int col = 0; col < 3; ++col)
        o[12 + col] = -(m[12] * inv[col] + m[13] * inv[3 + col] + m[14] * inv[6 + col]);
    o[15] = 1.0f;
    return true;
}

// m = pos x pws, rows 0-2 times 1/4096 (gx_take_record's fold)
void fold(ntr::GxRecMatrix &out, const float *pos, const float *pws, uint8_t persp)
{
    mat_mul(out.m, pos, pws);
    for (int i = 0; i < 12; ++i) out.m[i] *= kFx12;
    out.persp = persp;
}

// Camera-compensated pairing. Same-key ids (coins, trees, posts, particles)
// are matched by position, but a turning camera slides all of them, and past
// half the gap between two the nearest match is the neighbour (the cause of
// the flicker while the camera moved). So first move tick N-1's centroids by
// the camera's own step, using the biggest perspective id with a unique key
// (the level mesh) as reference, then project with tick N's projection. A
// still object lands on itself; a moving one on its own motion.
bool camera_step(float *d, size_t &ref_cur)
{
    std::unordered_map<uint32_t, int> prev_count;
    prev_count.reserve(g_prev.size() * 2);
    for (const IdInfo &p : g_prev)
        if (p.nverts) ++prev_count[p.key];
    ref_cur = (size_t)-1;
    for (size_t k = 0; k < g_cur.size(); ++k) {
        const IdInfo &c = g_cur[k];
        if (!c.persp || !c.nverts) continue;
        auto it = prev_count.find(c.key);
        if (it == prev_count.end() || it->second != 1) continue;
        if (ref_cur != (size_t)-1 && c.nverts <= g_cur[ref_cur].nverts) continue;
        ref_cur = k;
    }
    if (ref_cur == (size_t)-1) return false;
    size_t ref_prev = (size_t)-1;
    for (size_t q = 0; q < g_prev.size(); ++q)
        if (g_prev[q].nverts && g_prev[q].key == g_cur[ref_cur].key) ref_prev = q;
    float inv[16];
    if (ref_prev == (size_t)-1 || !affine_inverse(inv, g_prev[ref_prev].raw->pos)) return false;
    mat_mul(d, inv, g_cur[ref_cur].raw->pos);
    return true;
}

// Where p's centroid would be on screen: through tick N-1's position matrix,
// the camera step d and tick N's projection. Left alone if it's behind the eye.
void predict(const IdInfo &p, const float *d, const float *pws, float &ox, float &oy)
{
    const float *a = p.raw->pos;
    const float x = p.cx * kFx12, y = p.cy * kFx12, z = p.cz * kFx12;
    float v[4], u[4], c[4];
    for (int j = 0; j < 4; ++j) v[j] = x * a[j] + y * a[4 + j] + z * a[8 + j] + a[12 + j];
    for (int j = 0; j < 4; ++j)
        u[j] = v[0] * d[j] + v[1] * d[4 + j] + v[2] * d[8 + j] + v[3] * d[12 + j];
    for (int j = 0; j < 4; ++j)
        c[j] = u[0] * pws[j] + u[1] * pws[4 + j] + u[2] * pws[8 + j] + u[3] * pws[12 + j];
    if (c[3] > 1e-6f) {
        ox = c[0] / c[3];
        oy = c[1] / c[3];
    }
}

void report()
{
    if (g_t.ticks < 120) return;
    static const bool on = getenv("GAME3DS_INTERP_STATS") != nullptr;
    if (on) {
        const double n = (double)g_t.ticks;
        float med = 0.0f, drift = -1.0f;
        if (!g_t.medians.empty()) {
            std::sort(g_t.medians.begin(), g_t.medians.end());
            med = g_t.medians[g_t.medians.size() / 2];
        }
        if (!g_t.drift.empty()) {
            std::sort(g_t.drift.begin(), g_t.drift.end());
            drift = g_t.drift[g_t.drift.size() / 2];
        }
        printf("[interp] %u ticks: in-between %u (none: first %u, few pairs %u, cut %u); "
               "ids/tick %.0f, paired by key %.0f, by position %.0f, as meshes %.1f, new %.1f; "
               "vertices paired %.1f%%; origin moved median %.2f px; camera ref %.0f%%; "
               "suspect pairs %.2f/tick, keys by draw order %.1f/tick, too big to sort %.2f/tick (%s); "
               "world ticks %lu, teleports %.2f/tick, still-ref drift median %.3f; take %.3f ms/tick "
               "(describe %.3f, pair %.3f, meshes %.3f, blend %.3f)\n",
               g_t.ticks, g_t.blended, g_t.none_first, g_t.none_few, g_t.none_cut, g_t.ids / n,
               g_t.by_key / n, g_t.by_pos / n, g_t.by_mesh / n, g_t.fresh / n,
               g_t.verts ? 100.0 * (double)g_t.verts_paired / (double)g_t.verts : 0.0, med,
               100.0 * (double)g_t.cam_ref / n, g_t.suspect / n, g_t.by_order / n, g_t.greedy / n,
               pair_mode() == PAIR_RAW ? "raw pairing" : pair_mode() == PAIR_5F ? "5f pairing" : "world pairing",
               g_t.world, g_t.teleport / n, drift,
               (double)g_t.cpu * 1000.0 / (double)SYSCLOCK_ARM11 / n,
               (double)g_t.ph[0] * 1000.0 / (double)SYSCLOCK_ARM11 / n,
               (double)g_t.ph[1] * 1000.0 / (double)SYSCLOCK_ARM11 / n,
               (double)g_t.ph[2] * 1000.0 / (double)SYSCLOCK_ARM11 / n,
               (double)g_t.ph[3] * 1000.0 / (double)SYSCLOCK_ARM11 / n);
    }
    g_t = Totals();
}

}  // namespace

namespace {
// GAME3DS_INTERP and GAME3DS_INTERP_STATS as first read, then the options
// menu's 60 fps switch (interp3ds_set_on)
int g_on = -1;
bool g_stats;

void latch_on()
{
    if (g_on >= 0) return;
    g_on = getenv("GAME3DS_INTERP") && atoi(getenv("GAME3DS_INTERP")) != 0;
    g_stats = getenv("GAME3DS_INTERP_STATS") != nullptr;
    // Old 3DS only pairs when it will show in-betweens. With just the stats on it
    // was recording and pairing ~300 ids a tick for nothing (6-10 ms a frame, and
    // the 50 KB allocation it ran out of heap on). GAME3DS_INTERP_STATS=2 pairs
    // anyway for the counters.
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    if (g_stats && !g_on && !n3ds && atoi(getenv("GAME3DS_INTERP_STATS")) != 2) {
        g_stats = false;
        printf("[interp] Old 3DS, 60 fps off: no pairing (GAME3DS_INTERP_STATS=2 pairs anyway)\n");
    }
}
}  // namespace

bool interp3ds_on()
{
    latch_on();
    return g_on != 0;
}

bool interp3ds_wanted()
{
    latch_on();
    return g_on || g_stats;
}

void interp3ds_set_on(bool on)
{
    latch_on();
    g_on = on;
    // off is off: no pairing at all, so its counters stop with it
    if (!on) g_stats = false;
    interp3ds_reset();
}

void interp3ds_reset()
{
    g_have_prev = false;
    g_view_prev_ok = false;
    view_forget();
}

bool interp3ds_capture_view(int32_t out[12])
{
    if (!data_0209b3ec) return false;
    memcpy(out, data_0209b3ec, 12 * sizeof(int32_t));
    return true;
}

bool interp3ds_take(ntr::GxRecord &rec, std::vector<ntr::GxRecMatrix> &mid,
                    std::vector<ntr::GxRecVertex> &mid_v, const int32_t *view)
{
    const u64 t_take = svcGetSystemTick();
    mid_v.clear();
    // the factors are this file's from here on (rec.raw gets tick N-2's
    // buffer back, which gx_take_record refills)
    const bool have_raw = rec.raw.size() == rec.mtx.size();
    g_cur_raw.swap(rec.raw);
    if (!have_raw) g_cur_raw.clear();
    // this tick's view, and whether it pairs in the world: the world mode, a
    // usable view in both ticks, and tick N-1 described for it
    const PairMode mode = pair_mode();
    g_view_cur_ok = read_view(g_view_cur, view);
    float vinv[16];
    const bool world_now = mode == PAIR_WORLD && g_view_cur_ok && affine_inverse(vinv, g_view_cur);
    describe(rec, g_cur, world_now, vinv);
    u64 t_ph = svcGetSystemTick();
    g_t.ph[0] += t_ph - t_take;
    const bool views = g_have_prev && g_view_prev_ok && g_view_cur_ok;
    const bool world = world_now && views && g_prev_world;
    // the camera's own step, V_{N-1}^-1 x V_N, and the PC port's cut test
    float vstep[16];
    bool have_vstep = false, view_cuts = false;
    if (views) {
        float pinv[16];
        if (affine_inverse(pinv, g_view_prev)) {
            mat_mul(vstep, pinv, g_view_cur);
            have_vstep = true;
        }
        view_cuts = view_cut(g_view_prev, g_view_cur);
    } else {
        view_forget();
    }
    ++g_t.ticks;
    bool made = false;
    if (!g_have_prev || !have_raw) {
        ++g_t.none_first;
    } else {
        // Pair tick N-1's ids by key, nearest centroid among equal keys. Both ticks'
        // ids as sorted (key, id) pairs, so each key is one run. No hash map: its
        // nodes and modulo divides (done in software on the ARM11) were a third of
        // the cost.
        static std::vector<std::pair<uint32_t, uint32_t>> pk, ck;
        pk.clear();
        for (size_t k = 0; k < g_prev.size(); ++k) {
            g_prev[k].used = false;
            if (g_prev[k].nverts) pk.emplace_back(g_prev[k].key, (uint32_t)k);
        }
        std::sort(pk.begin(), pk.end());
        g_pair.assign(g_cur.size(), -1);
        g_moves.clear();
        // tick N-1's centroids where tick N's camera would show them still
        float dstep[16];
        size_t ref_cur = (size_t)-1;
        bool comp;
        if (world) {
            memcpy(dstep, vstep, sizeof dstep);
            comp = have_vstep;
            ++g_t.world;
        } else {
            comp = camera_step(dstep, ref_cur);
        }
        // GAME3DS_INTERP_PAIR=raw: nearest on screen as it is (the old way). Default:
        // nearest after the camera's own step.
        const bool use_cam = comp && mode != PAIR_RAW;
        // (the world pairing reads a perspective id's px, py only in the mesh
        // pairs below, which predict their own candidates)
        for (IdInfo &p : g_prev) {
            if (!p.nverts) continue;
            p.px = p.sx;
            p.py = p.sy;
            if (comp && p.persp && !world)
                predict(p, dstep, g_cur[ref_cur].raw->pws, p.px, p.py);
        }
        // Per key: every (tick N id, tick N-1 id) candidate, nearest pairs first
        // across the whole group, so an early id can't steal a later id's partner.
        struct Cand {
            float dd;
            uint32_t k, q;
        };
        ck.clear();
        unsigned long verts = 0, verts_paired = 0, verts_jumped = 0;
        for (size_t k = 0; k < g_cur.size(); ++k) {
            if (!g_cur[k].nverts) continue;   // an id with no vertex of its own
            ++g_t.ids;
            verts += g_cur[k].nverts;
            ck.emplace_back(g_cur[k].key, (uint32_t)k);
        }
        std::sort(ck.begin(), ck.end());
        static std::vector<Cand> cands, order_cands;
        static std::vector<uint32_t> order_prev, grp;
        size_t pi = 0;
        for (size_t ci = 0; ci < ck.size();) {
            const uint32_t key = ck[ci].first;
            grp.clear();
            for (; ci < ck.size() && ck[ci].first == key; ++ci) grp.push_back(ck[ci].second);
            while (pi < pk.size() && pk[pi].first < key) ++pi;
            order_prev.clear();
            for (; pi < pk.size() && pk[pi].first == key; ++pi) order_prev.push_back(pk[pi].second);
            const size_t nprev = order_prev.size();
            if (!nprev) {
                g_t.fresh += grp.size();
                continue;
            }
            const bool several = nprev > 1 || grp.size() > 1;
            auto dist = [&](size_t i, size_t j) {
                const IdInfo &c = g_cur[grp[i]], &p = g_prev[order_prev[j]];
                if (world && c.persp) {
                    // in the world, and draw order only for an exact tie
                    const float dx = p.wx - c.wx, dy = p.wy - c.wy, dz = p.wz - c.wz;
                    const float di = (float)i - (float)j;
                    return dx * dx + dy * dy + dz * dz + 1e-6f * di * di;
                }
                const float dx = (use_cam ? p.px : p.sx) - c.sx, dy = (use_cam ? p.py : p.sy) - c.sy;
                return dx * dx + dy * dy;
            };
            cands.clear();
            if (grp.size() * nprev <= kSortCands) {
                for (size_t i = 0; i < grp.size(); ++i)
                    for (size_t j = 0; j < nprev; ++j) cands.push_back({dist(i, j), grp[i], order_prev[j]});
                std::sort(cands.begin(), cands.end(),
                          [](const Cand &a, const Cand &b) { return a.dd < b.dd; });
            } else {
                // A particle burst gives one key hundreds of ids, and sorting every
                // (tick N, tick N-1) candidate cost 20-40 ms a tick (the ? switch's
                // sparkles). A particle system draws its particles in the same order
                // each tick, so each id in draw order takes its nearest free partner
                // among the ones near the same place in tick N-1's order.
                ++g_t.greedy;
                static std::vector<uint8_t> taken;
                taken.assign(nprev, 0);
                constexpr size_t kWindow = 24;
                for (size_t i = 0; i < grp.size(); ++i) {
                    const size_t at = i * nprev / grp.size();
                    const size_t lo = at > kWindow ? at - kWindow : 0, hi = std::min(nprev, at + kWindow + 1);
                    size_t best = nprev;
                    float bd = 0.0f;
                    for (size_t j = lo; j < hi; ++j) {
                        if (taken[j]) continue;
                        const float dd = dist(i, j);
                        if (best == nprev || dd < bd) {
                            best = j;
                            bd = dd;
                        }
                    }
                    if (best == nprev) continue;
                    taken[best] = 1;
                    cands.push_back({bd, grp[i], order_prev[best]});
                }
            }
            // Draw order first. A model draws its parts in the same order every tick, and
            // a level its scenery, so if the key has as many ids in both ticks, pair the
            // k-th with the k-th as long as every pair is within kOrderPx of where the
            // camera step puts it. Nearness alone swaps similar parts that sit close
            // (trees flickering into each other's places). If the set changed (something
            // left or entered view) fall back to nearest pairs.
            if (mode == PAIR_5F && use_cam && several && nprev == grp.size()) {
                bool fits = true;
                order_cands.clear();
                for (size_t i = 0; i < order_prev.size() && fits; ++i) {
                    const uint32_t k = grp[i], q = order_prev[i];
                    const IdInfo &c = g_cur[k], &p = g_prev[q];
                    const float dx = p.px - c.sx, dy = p.py - c.sy, dd = dx * dx + dy * dy;
                    if (near_screen(c.sx, c.sy) && near_screen(p.px, p.py) && dd > kOrderPx * kOrderPx)
                        fits = false;
                    order_cands.push_back({dd, k, q});
                }
                if (fits) {
                    cands.swap(order_cands);
                    ++g_t.by_order;
                }
            }
            for (const Cand &cd : cands) {
                if (g_pair[cd.k] != -1 || g_prev[cd.q].used) continue;
                const IdInfo &c = g_cur[cd.k], &p = g_prev[cd.q];
                const float qx = use_cam ? p.px : p.sx, qy = use_cam ? p.py : p.sy;
                const bool on_screen = near_screen(c.sx, c.sy) && near_screen(qx, qy);
                // A far move with several candidates is a wrong pair. With only one candidate
                // it's the same object and the move is real (or a teleport, which the median
                // below catches when it's the whole scene).
                const bool in_world = world && c.persp;
                if (!in_world && several && on_screen && cd.dd > kJumpPx * kJumpPx) continue;
                // A TELEPORT: the pair's world origin jumped (a warp, a respawn,
                // or two objects that are not one): refused, as the PC port's
                if (in_world) {
                    const float dx = p.ox - c.ox, dy = p.oy - c.oy, dz = p.oz - c.oz;
                    if (dx * dx + dy * dy + dz * dz > kTeleport * kTeleport) {
                        g_prev[cd.q].used = true;
                        g_pair[cd.k] = -2;
                        ++g_t.teleport;
                        continue;
                    }
                }
                g_prev[cd.q].used = true;
                g_pair[cd.k] = (int)cd.q;
                ++(several ? g_t.by_pos : g_t.by_key);
                verts_paired += c.nverts;
                // Suspect pair: one of several same-key ids whose partner isn't where the
                // camera step says a still object would be (a tree paired with its neighbour).
                // Counted either way.
                float pex = p.px, pey = p.py;
                if (several && have_vstep && p.persp) predict(p, vstep, p.raw->pws, pex, pey);
                if (several && (comp || have_vstep) && p.persp && near_screen(pex, pey) && in_view(c.sx, c.sy)) {
                    const float ex = pex - c.sx, ey = pey - c.sy;
                    if (ex * ex + ey * ey > kSuspectPx * kSuspectPx) {
                        ++g_t.suspect;
                        // GAME3DS_INTERP_SUSPECT=1: each one, the first 200
                        static const bool say = getenv("GAME3DS_INTERP_SUSPECT") != nullptr;
                        static int said;
                        if (say && said++ < 200)
                            printf("[interp-suspect] tick %u: %u verts, key %08x, now (%.0f,%.0f), "
                                   "partner (%.0f,%.0f) predicted (%.0f,%.0f), miss %.0f px, %u of the key\n",
                                   g_t.ticks, c.nverts, (unsigned)c.key, c.sx, c.sy, p.sx, p.sy, pex, pey,
                                   sqrtf(ex * ex + ey * ey), (unsigned)grp.size());
                    }
                }
                // the cut test reads the RAW move (the camera's own included)
                if (in_view(c.sx, c.sy) && in_view(p.sx, p.sy) && near_screen(p.sx, p.sy)) {
                    const float dx = p.sx - c.sx, dy = p.sy - c.sy;
                    g_moves.push_back(sqrtf(dx * dx + dy * dy));
                }
            }
            for (uint32_t k : grp)
                if (g_pair[k] < 0) {
                    g_pair[k] = -1;   // a refused teleport is unpaired
                    ++g_t.fresh;
                    if (several) verts_jumped += g_cur[k].nverts;
                }
        }
        {
            const u64 t = svcGetSystemTick();
            g_t.ph[1] += t - t_ph;
            t_ph = t;
        }
        // Mesh pairs: an unpaired mesh (16+ vertices, one run) and an unpaired tick N-1
        // mesh with the same vertex count and projection, nearest on screen. That's
        // the same mesh with moving vertices (e.g. CPU-animated geometry in the
        // opening). Its matrix blends like a normal pair and its vertices are averaged
        // into the in-between's own copy.
        g_mesh.assign(g_cur.size(), 0);
        if (world && comp) {
            bool any = false;
            for (size_t k = 0; k < g_cur.size() && !any; ++k)
                any = g_pair[k] < 0 && g_cur[k].nverts >= 16 && g_cur[k].contiguous;
            for (size_t q = 0; any && q < g_prev.size(); ++q) {
                IdInfo &p = g_prev[q];
                if (!p.used && p.persp && p.nverts >= 16 && p.contiguous)
                    predict(p, dstep, p.raw->pws, p.px, p.py);
            }
        }
        for (size_t k = 0; k < g_cur.size(); ++k) {
            const IdInfo &c = g_cur[k];
            if (g_pair[k] >= 0 || c.nverts < 16 || !c.contiguous) continue;
            size_t best = (size_t)-1;
            float best_d = 0.0f;
            for (size_t q = 0; q < g_prev.size(); ++q) {
                const IdInfo &p = g_prev[q];
                if (p.used || p.nverts != c.nverts || p.persp != c.persp || !p.contiguous) continue;
                const float dx = p.px - c.sx, dy = p.py - c.sy, dd = dx * dx + dy * dy;
                if (best == (size_t)-1 || dd < best_d) {
                    best = q;
                    best_d = dd;
                }
            }
            if (best == (size_t)-1) continue;
            if (near_screen(c.sx, c.sy) && near_screen(g_prev[best].px, g_prev[best].py) &&
                best_d > kJumpPx * kJumpPx) continue;
            g_prev[best].used = true;
            g_pair[k] = (int)best;
            g_mesh[k] = 1;
            --g_t.fresh;
            ++g_t.by_mesh;
            verts_paired += c.nverts;
        }
        g_t.verts += verts;
        g_t.verts_paired += verts_paired;
        // GAME3DS_INTERP_DIAG=1: every 120 ticks, list the largest unpaired ids and
        // whether tick N-1 left an unpaired id of the same vertex count.
        static const bool diag = getenv("GAME3DS_INTERP_DIAG") != nullptr;
        static unsigned diag_few;
        const bool few = (float)verts_paired < kMinShare * (float)verts;
        if (diag && (g_t.ticks % 120 == 60 || (few && diag_few++ < 64))) {
            std::vector<std::pair<uint32_t, size_t>> un;
            for (size_t k = 0; k < g_cur.size(); ++k)
                if (g_pair[k] < 0 && g_cur[k].nverts) un.emplace_back(g_cur[k].nverts, k);
            std::sort(un.rbegin(), un.rend());
            printf("[interp-diag] tick %u: %lu of %lu vertices paired (%lu in pairs that moved over %.0f px); "
                   "largest unpaired:", g_t.ticks, verts_paired, verts, verts_jumped, kJumpPx);
            for (size_t i = 0; i < un.size() && i < 8; ++i) {
                const IdInfo &c = g_cur[un[i].second];
                bool twin = false;
                for (const IdInfo &p : g_prev)
                    if (!p.used && p.nverts == c.nverts && p.persp == c.persp) twin = true;
                printf(" [id %u: %u verts, %s, at %.0f,%.0f%s]", (unsigned)un[i].second, c.nverts,
                       c.persp ? "persp" : "ortho", c.sx < 1e8f ? c.sx : -1.0f, c.sy < 1e8f ? c.sy : -1.0f,
                       twin ? ", same-count id unpaired in N-1" : "");
            }
            printf("\n");
        }
        float median = 0.0f;
        if (!g_moves.empty()) {
            std::nth_element(g_moves.begin(), g_moves.begin() + g_moves.size() / 2, g_moves.end());
            median = g_moves[g_moves.size() / 2];
            g_t.medians.push_back(median);
        }
        if (!verts || (float)verts_paired < kMinShare * (float)verts) {
            ++g_t.none_few;
        } else if (world ? view_cuts : median > kCutPx) {
            ++g_t.none_cut;
        } else {
            {
                const u64 t = svcGetSystemTick();
                g_t.ph[2] += t - t_ph;
                t_ph = t;
            }
            // the blend, and the camera's half step from the largest paired
            // perspective id
            mid.resize(g_cur.size());
            static std::vector<float> pos_mid;
            pos_mid.resize(g_cur.size() * 16);
            size_t ref = (size_t)-1;
            for (size_t k = 0; k < g_cur.size(); ++k) {
                const IdInfo &c = g_cur[k];
                if (g_pair[k] < 0) continue;
                const IdInfo &p = g_prev[(size_t)g_pair[k]];
                // a pair whose factors did not change blends to them exactly
                // (x + x halved is x, and each row's length scale is 1)
                if (!memcmp(p.raw->pos, c.raw->pos, sizeof c.raw->pos) &&
                    !memcmp(p.raw->pws, c.raw->pws, sizeof c.raw->pws)) {
                    memcpy(&pos_mid[k * 16], c.raw->pos, sizeof c.raw->pos);
                    fold(mid[k], c.raw->pos, c.raw->pws, c.persp);
                } else {
                    blend_pos_len(&pos_mid[k * 16], p.raw->pos, c.raw->pos, p.len, c.len);
                    float pws[16];
                    for (int i = 0; i < 16; ++i) pws[i] = 0.5f * (p.raw->pws[i] + c.raw->pws[i]);
                    fold(mid[k], &pos_mid[k * 16], pws, c.persp);
                }
                if (c.persp && (ref == (size_t)-1 || c.nverts > g_cur[ref].nverts)) ref = k;
            }
            float cam[16];
            bool have_cam = false;
            if (world) {
                // the camera's half step from the views themselves:
                // V_N^-1 x V_mid
                float vmid[16], inv[16];
                blend_pos(vmid, g_view_prev, g_view_cur);
                if (affine_inverse(inv, g_view_cur)) {
                    mat_mul(cam, inv, vmid);
                    have_cam = true;
                    ++g_t.cam_ref;
                }
                // Reference drift check: the largest paired perspective id shouldn't move in
                // the world (confirms V is the matrix the draws used).
                if (ref != (size_t)-1) {
                    const IdInfo &c = g_cur[ref], &p = g_prev[(size_t)g_pair[ref]];
                    const float dx = p.ox - c.ox, dy = p.oy - c.oy, dz = p.oz - c.oz;
                    g_t.drift.push_back(sqrtf(dx * dx + dy * dy + dz * dz));
                }
            } else if (ref != (size_t)-1) {
                float inv[16];
                if (affine_inverse(inv, g_cur[ref].raw->pos)) {
                    mat_mul(cam, inv, &pos_mid[ref * 16]);
                    have_cam = true;
                    ++g_t.cam_ref;
                }
            }
            for (size_t k = 0; k < g_cur.size(); ++k) {
                if (g_pair[k] >= 0) continue;
                const IdInfo &c = g_cur[k];
                if (have_cam && c.persp) {
                    float p[16];
                    mat_mul(p, c.raw->pos, cam);
                    fold(mid[k], p, c.raw->pws, c.persp);
                } else {
                    mid[k] = rec.mtx[k];   // as tick N has it
                }
            }
            // the meshes' vertices, halfway (raw model units, rounded down)
            for (size_t k = 0; k < g_cur.size(); ++k) {
                if (!g_mesh[k]) continue;
                if (mid_v.empty()) mid_v = rec.v;
                const IdInfo &c = g_cur[k], &p = g_prev[(size_t)g_pair[k]];
                for (uint32_t i = 0; i < c.nverts; ++i) {
                    ntr::GxRecVertex &o = mid_v[c.first + i];
                    const int16_t *a = &g_prev_xyz[(size_t)(p.first + i) * 3];
                    o.x = (int16_t)(((int)a[0] + (int)o.x) >> 1);
                    o.y = (int16_t)(((int)a[1] + (int)o.y) >> 1);
                    o.z = (int16_t)(((int)a[2] + (int)o.z) >> 1);
                }
            }
            ++g_t.blended;
            made = true;
        }
    }
    g_prev.swap(g_cur);
    g_prev_raw.swap(g_cur_raw);   // the ids' pointers move with the buffers
    g_prev_xyz.swap(g_cur_xyz);
    g_have_prev = have_raw;
    g_prev_world = world_now;
    memcpy(g_view_prev, g_view_cur, sizeof g_view_prev);
    g_view_prev_ok = g_view_cur_ok;
    const u64 t_end = svcGetSystemTick();
    if (made) g_t.ph[3] += t_end - t_ph;
    g_t.cpu += t_end - t_take;
    // GAME3DS_INTERP_DIGEST=1: one hash per tick of every decision (pairs, meshes,
    // the in-between's matrices and vertices), so two builds that should behave
    // the same can be compared line by line.
    static const bool digest = getenv("GAME3DS_INTERP_DIGEST") != nullptr;
    if (digest) {
        uint32_t h = 2166136261u;
        auto mix = [&h](uint32_t w) { h = (h ^ w) * 16777619u; };
        mix(made);
        mix((uint32_t)g_pair.size());
        for (size_t k = 0; k < g_pair.size(); ++k) {
            mix((uint32_t)g_pair[k]);
            mix(k < g_mesh.size() ? g_mesh[k] : 0xFFu);
        }
        if (made) {
            for (const ntr::GxRecMatrix &m : mid) {
                uint32_t w[16];
                memcpy(w, m.m, sizeof w);
                for (uint32_t x : w) mix(x);
                mix(m.persp);
            }
            for (const ntr::GxRecVertex &v : mid_v) mix((uint16_t)v.x | ((uint32_t)(uint16_t)v.y << 16)), mix((uint16_t)v.z);
        }
        printf("[interp-digest] %u %08x\n", g_t.ticks, (unsigned)h);
    }
    report();
    return made;
}
