// The game's file opens: counted, timed, and served out of one pack file.
//
// The file select to castle grounds load took ~10 s on hardware. The level
// boot opens ~180 files, each once, and on the 3DS SD card an open + close
// costs ~30 ms regardless of size: almost all of the boot was fopen and
// fclose.
//
// So every read-only fopen under <root>/extracted/dsd/files/ (wrapped with
// -Wl,--wrap=fopen) is served from <root>/extracted/files.pak
// (tools/pack_files.py; written to the SD by stage_assets.ps1 and into the
// .cia's RomFS by make_cia.py). Its index is read once and the pack stays
// open; each file is a fopencookie stream over its range (seek + read under
// one lock, since the boot worker and the game thread both load). Files up to
// 1 MB are read whole on open in one read (callers read whole files, and
// stdio's 1 KB refills each cost a seek); bigger ones (sound data) read on
// demand. Anything not in the pack, or no pack at all, uses the normal fopen.
// GAME3DS_NO_PACK=1 disables it; GAME3DS_FOPEN_TRACE=1 logs every open.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   // newlib's fopencookie (a strict -std= hides it)
#endif
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <string>
#include <unordered_map>

extern "C" FILE *__real_fopen(const char *path, const char *mode);
extern "C" int __real_fclose(FILE *f);

// for main_game.cpp's boot_lap
unsigned long g_fopen_n, g_fopen_packed;
u64 g_fopen_ticks;

namespace {

// Under either asset root (SM64DS_ASSET_ROOT): the .cia's RomFS or the SD
// folder. The pack is the one next to the requested files; a run only uses
// one root.
constexpr char kRel[] = "/extracted/dsd/files/";
const char *const kRoots[] = {"romfs:", "sdmc:/sm64ds3ds"};
std::string g_pack_path;

struct Entry {
    u32 off, size;
};
std::unordered_map<std::string, Entry> g_index;
FILE *g_pack;
LightLock g_lock;
bool g_loaded;

u32 rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

// how much of path is the asset root, when it names a file under one's
// extracted/dsd/files/ (0: it does not)
size_t root_len(const char *path)
{
    for (const char *r : kRoots) {
        const size_t n = strlen(r);
        if (strncmp(path, r, n) == 0 && strncmp(path + n, kRel, sizeof kRel - 1) == 0) return n;
    }
    return 0;
}

// the index of root's pack, once (under g_lock); false: no pack, or a pack
// this cannot read
bool load_index(const std::string &root)
{
    if (g_loaded) return g_pack != nullptr;
    g_loaded = true;
    if (getenv("GAME3DS_NO_PACK")) {
        printf("[pack] GAME3DS_NO_PACK: the game files are opened one by one\n");
        return false;
    }
    g_pack_path = root + "/extracted/files.pak";
    const char *kPack = g_pack_path.c_str();
    const u64 t0 = svcGetSystemTick();
    FILE *f = __real_fopen(kPack, "rb");
    if (!f) {
        printf("[pack] no %s: the game files are opened one by one (tools/stage_assets.ps1 "
               "writes it)\n", kPack);
        return false;
    }
    unsigned char head[16];
    if (fread(head, 1, 16, f) != 16 || memcmp(head, "S3PK", 4) != 0 || rd32(head + 4) != 1) {
        printf("[pack] %s is not a version 1 pack: not used\n", kPack);
        __real_fclose(f);
        return false;
    }
    const u32 count = rd32(head + 8), data_off = rd32(head + 12);
    std::string idx(data_off - 16, '\0');
    if (fread(&idx[0], 1, idx.size(), f) != idx.size()) {
        printf("[pack] %s: short index: not used\n", kPack);
        __real_fclose(f);
        return false;
    }
    g_index.reserve(count * 2);
    size_t at = 0;
    for (u32 i = 0; i < count && at + 10 <= idx.size(); ++i) {
        const unsigned char *p = (const unsigned char *)idx.data() + at;
        const u32 off = rd32(p), size = rd32(p + 4);
        const unsigned len = p[8] | p[9] << 8;
        g_index.emplace(std::string((const char *)p + 10, len), Entry{off, size});
        at += 10 + len;
    }
    setvbuf(f, nullptr, _IONBF, 0);   // a seek + read is one SD read of what is asked
    g_pack = f;
    printf("[pack] %s: %u files, index read in %.1f ms; the game files open from it\n", kPack,
           (unsigned)g_index.size(), (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11);
    return true;
}

#ifdef __LARGE64_FILES
typedef _off64_t ck_off;
#else
typedef off_t ck_off;
#endif

constexpr u32 kWhole = 1024 * 1024;   // read whole as it opens, up to this

struct Cookie {
    u32 off, size, pos;
    unsigned char *mem;   // the whole file (size <= kWhole), or null: from the pack
};

// n bytes of the pack at off, under g_lock
size_t pack_read(u32 off, void *buf, size_t n)
{
    LightLock_Lock(&g_lock);
    size_t got = 0;
    if (fseek(g_pack, (long)off, SEEK_SET) == 0) got = fread(buf, 1, n, g_pack);
    LightLock_Unlock(&g_lock);
    return got;
}

ssize_t ck_read(void *c, char *buf, size_t n)
{
    Cookie &k = *(Cookie *)c;
    if (k.pos >= k.size) return 0;
    if (n > k.size - k.pos) n = k.size - k.pos;
    const size_t got = k.mem ? (memcpy(buf, k.mem + k.pos, n), n) : pack_read(k.off + k.pos, buf, n);
    k.pos += (u32)got;
    return (ssize_t)got;
}

int ck_seek(void *c, ck_off *off, int whence)
{
    Cookie &k = *(Cookie *)c;
    long long to = whence == SEEK_SET ? (long long)*off
                 : whence == SEEK_CUR ? (long long)k.pos + *off
                                      : (long long)k.size + *off;
    if (to < 0) return -1;
    k.pos = to > k.size ? k.size : (u32)to;
    *off = k.pos;
    return 0;
}

int ck_close(void *c)
{
    Cookie *k = (Cookie *)c;
    free(k->mem);
    delete k;
    return 0;
}

// the pack's stream for path, or null (not under the prefix, not read-only,
// not in the pack, or no pack)
FILE *from_pack(const char *path, const char *mode)
{
    const size_t root = root_len(path);
    if (!root || mode[0] != 'r' || strchr(mode, '+'))
        return nullptr;
    LightLock_Lock(&g_lock);
    const bool up = load_index(std::string(path, root));
    const auto it = up ? g_index.find(path + root + sizeof kRel - 1) : g_index.end();
    const bool found = up && it != g_index.end();
    const Entry e = found ? it->second : Entry{0, 0};
    LightLock_Unlock(&g_lock);
    if (!found) return nullptr;
    Cookie *k = new Cookie{e.off, e.size, 0, nullptr};
    if (e.size <= kWhole) {
        k->mem = (unsigned char *)malloc(e.size ? e.size : 1);
        if (k->mem && pack_read(e.off, k->mem, e.size) != e.size) {
            free(k->mem);
            k->mem = nullptr;   // read as asked, then
        }
    }
    cookie_io_functions_t io = {ck_read, nullptr, ck_seek, ck_close};
    FILE *f = fopencookie(k, "rb", io);
    if (!f) {
        free(k->mem);
        delete k;
        return nullptr;
    }
    if (k->mem)
        setvbuf(f, nullptr, _IONBF, 0);   // already in memory: no second copy
    else
        setvbuf(f, nullptr, _IOFBF, 64 * 1024);
    return f;
}

struct LockInit {
    LockInit() { LightLock_Init(&g_lock); }
} g_lock_init;

}  // namespace

extern "C" FILE *__wrap_fopen(const char *path, const char *mode)
{
    const u64 t0 = svcGetSystemTick();
    FILE *f = path && mode ? from_pack(path, mode) : nullptr;
    const bool packed = f != nullptr;
    if (!f) f = __real_fopen(path, mode);
    const u64 dt = svcGetSystemTick() - t0;
    ++g_fopen_n;
    if (packed) ++g_fopen_packed;
    g_fopen_ticks += dt;
    // read each time: the first opens come before env.txt is in the environment
    if (getenv("GAME3DS_FOPEN_TRACE"))
        printf("[fopen] %.2f ms %s %s%s%s\n", (double)dt * 1000.0 / (double)SYSCLOCK_ARM11, mode,
               path, packed ? " (pack)" : "", f ? "" : " (failed)");
    return f;
}

extern "C" int __wrap_fclose(FILE *f)
{
    const u64 t0 = svcGetSystemTick();
    const int rc = __real_fclose(f);
    g_fopen_ticks += svcGetSystemTick() - t0;
    return rc;
}

// GAME3DS_LZ_SELFTEST=1: check the HAL's faster DecompressLZ16 and
// MultiStore_Int against the old byte/word loops, over every LZ77 file in the
// pack and a table of fills. Prints one line.
extern "C" void DecompressLZ16(void *src, void *dst);
extern "C" void MultiStore_Int(int val, int *dst, int len);

namespace {

void lz_old(const unsigned char *s, unsigned char *d)
{
    u32 size = (s[0] | s[1] << 8 | s[2] << 16 | (u32)s[3] << 24) >> 8;
    u32 out = 0;
    s += 4;
    while (out < size) {
        unsigned char flags = *s++;
        for (int bit = 0; bit < 8 && out < size; ++bit) {
            if (flags & (0x80 >> bit)) {
                u32 len = (s[0] >> 4) + 3;
                u32 disp = ((s[0] & 0xF) << 8 | s[1]) + 1;
                s += 2;
                while (len-- && out < size) {
                    d[out] = d[out - disp];
                    ++out;
                }
            } else {
                d[out++] = *s++;
            }
        }
    }
}

}  // namespace

void game3ds_lz_selftest()
{
    if (!getenv("GAME3DS_LZ_SELFTEST")) return;
    LightLock_Lock(&g_lock);
    const char *root = getenv("SM64DS_ASSET_ROOT");
    const bool up = load_index(root && *root ? root : kRoots[1]);
    LightLock_Unlock(&g_lock);
    if (!up) {
        printf("[lz-selftest] no pack: not run\n");
        return;
    }
    unsigned long files = 0, bytes = 0, bad = 0;
    u64 t_old = 0, t_new = 0;
    for (const auto &kv : g_index) {
        const Entry &e = kv.second;
        if (e.size < 8) continue;
        std::string raw(e.size, '\0');
        if (pack_read(e.off, &raw[0], e.size) != e.size) continue;
        const unsigned char *r = (const unsigned char *)raw.data();
        if (memcmp(r, "LZ77", 4) != 0 || r[4] != 0x10) continue;
        const u32 size = (r[4] | r[5] << 8 | r[6] << 16 | (u32)r[7] << 24) >> 8;
        const u32 pad = 4096;
        std::string a(size + 2 * pad, '\xAB'), b(size + 2 * pad, '\xAB');
        u64 t0 = svcGetSystemTick();
        lz_old(r + 4, (unsigned char *)&a[pad]);
        u64 t1 = svcGetSystemTick();
        DecompressLZ16((void *)(r + 4), &b[pad]);
        u64 t2 = svcGetSystemTick();
        t_old += t1 - t0;
        t_new += t2 - t1;
        ++files;
        bytes += size;
        if (a != b) {
            ++bad;
            printf("[lz-selftest] DIFFER: %s\n", kv.first.c_str());
        }
    }
    static const int vals[] = {0, -1, 0x01010101, 0x12345678, 0x00FF00FF, (int)0x80808080, 7};
    unsigned fbad = 0, cases = 0;
    for (int v : vals)
        for (int len = -4; len < 68; ++len) {
            int x[24], y[24];
            memset(x, 0x5A, sizeof x);
            memset(y, 0x5A, sizeof y);
            for (int i = 0; i < len / 4; ++i) x[2 + i] = v;   // the old loop
            MultiStore_Int(v, y + 2, len);
            ++cases;
            if (memcmp(x, y, sizeof x) != 0) ++fbad;
        }
    const double k = 1000.0 / (double)SYSCLOCK_ARM11;
    printf("[lz-selftest] %s: LZ77 %lu files, %lu KB decoded, %lu differ (old %.1f ms, new %.1f "
           "ms); fill %u of %u cases differ\n", bad || fbad ? "FAIL" : "PASS", files, bytes / 1024,
           bad, (double)t_old * k, (double)t_new * k, fbad, cases);
}
