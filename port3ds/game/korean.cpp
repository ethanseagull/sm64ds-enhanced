// The Korean edition: the text of the Korean cartridge (ASMK) on the European
// game.
//
// The 3DS game is the European ROM's code, decompiled. The Korean cartridge is
// a 2007 rebuild of that code (no mini-games) whose text is Korean: a message
// bank of two-byte character codes, drawn from a 1bpp font of 16x16 cells that
// lives in its arm9, and graphics in the Spanish slot of the language-keyed
// files. The builder (sm64ds_builder.py, Builder.cs) makes a Korean .cia from
// both dumps: the European one for everything the code needs, and from the
// Korean one the message bank, the graphics that differ, and
// build/assets/korean.bin, the Korean arm9's font and text tables:
//
//   0x00  "SM64DS-KOREAN\0\0\0"
//   0x10  u32 version (1), u32 total size
//   0x20  the font: 0x362 codes x 32 bytes (code c is glyphs 2c and 2c+1, each
//         8x16 at 1bpp, a byte a row, bit 7 leftmost)
//         then the glyph widths (256), the big font's widths (256), the codes
//         the big font has (0x34 u16), the button icons' glyphs (8), the
//         digits' glyphs (10 u16) and the object message id table (0x6a pairs)
//
// With that file in the asset root this file switches the game to Korean:
//
//   * GetOwnerLanguage answers 5. The Korean code took the Spanish arm of
//     every language switch (it never asks the language), and the builder puts
//     the Korean graphics in the Spanish files, as the Korean cartridge has
//     them.
//   * The message functions that read text become the Korean code's: a
//     character is two bytes (0xFC a space, 0xFD/0xFE/0xFF as before), and is
//     drawn from korean.bin's font. Each __wrap_ below is the Korean arm9's
//     function of the same European name, read from the Korean cartridge
//     (addresses in the comments are the Korean arm9's); the European body
//     runs whenever the edition is not Korean. CMakeLists.txt wraps them.
//   * The title menu (ov007) is the European one in its Spanish slot, which
//     the builder fills with the Korean pictures; its Rec Room does nothing.
//
// The European code is kept where the Korean cartridge only changed what the
// 3DS doesn't run (its own DS firmware reading, the mini-games, the DS
// download play and the VS nicknames).
//
// GAME3DS_KOREAN=0 (env.txt) keeps a Korean .cia's game in its European text;
// GAME3DS_KOREAN_TRACE=1 logs the title menu's choices.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned int u32;
typedef int s32;

namespace {

constexpr int kCodes = 0x362;          // the font's character codes
constexpr int kFontBytes = kCodes * 32;
constexpr int kBigFontCodes = 0x34;     // Hangul the big font has (0x90.. in it)
constexpr int kMsgIdPairs = 0x6a;

struct KoreanData {
    u8 font[kFontBytes];       // 0x020891e8
    u8 width[256];             // 0x020900fc, by code (< 0x64)
    u8 big_width[256];         // 0x020901fc, by big-font index
    u16 big_codes[kBigFontCodes];   // 0x0208feec
    u8 icons[8];               // 0x0208fe64, 7 used
    u16 digits[10];            // 0x0208fe78
    u16 msg_ids[kMsgIdPairs * 2];   // 0x0208ff54
};

KoreanData *g_ko;

}  // namespace

extern "C" {

int game3ds_korean;   // 1: the Korean edition is running
extern int game3ds_language_slot;       // language.cpp
extern int game3ds_language_override;

// ---- the European globals the message code keeps its state in -------------
extern u8 data_0209d65c;   // the line's starting pixel
extern u8 data_0209d660;
extern u8 data_0209d668;
extern u8 data_0209d674;
extern u8 data_0209d678;   // the pen's pixel within its tile
extern u8 data_0209d688;
extern u8 data_0209d68c;
extern u8 data_0209d694;
extern u8 data_0209d69c;
extern u8 data_0209d6a0;
extern u8 data_0209d6a4;   // characters on the line
extern u8 data_0209d6a8;
extern u8 data_0209d6b0;   // the line's width in pixels
extern u8 data_0209d6b4;
extern u8 data_0209d6b8;
extern u8 data_0209d6bc;   // the text state
extern u8 data_0209d6c0;
extern u8 data_0209d6c4;   // the colour
extern s16 data_0209d6d4;  // the message
extern u8 data_0209d6e4[];
extern u8 *data_0209d6f0;  // the message's info entry
extern u8 *data_0209d6f4;  // the text cursor
extern s32 data_0209d6fc;  // the bank
extern s32 data_0209d700;
extern s32 data_0209d704;  // words in the glyph buffer
extern u8 *data_0209d708;  // the bank's entries
extern s32 *data_0209d70c; // the bank's info section
extern u8 data_0209d710[]; // each character's width
extern s32 data_0209d74c[]; // the glyph buffer
extern u32 data_0208eecc[]; // the colour masks
extern u8 data_0209f20c;
extern u8 data_0209f220;
extern u8 data_0209f228;
extern u8 data_020a0f00;   // the owner's language
extern void (*MSG_GEN_TEXT_FUNCS[])(void);

void MultiStore_Int(int val, void *dst, int len);
void MultiStore16(u16 val, void *dst, int nbytes);
void MultiCopy_Int(void *src, void *dst, int len);
int func_02054d88(void);
void *_ZN2G212GetBG3ScrPtrEv(void);
unsigned _ZN2G213GetBG2CharPtrEv(void);
unsigned _ZN3G2S13GetBG0CharPtrEv(void);
void *_ZN3G2S12GetBG0ScrPtrEv(void);
void SetSubBg1Offset(int a, int b);
void SetBg3Offset(int x, int y);
void func_0201eaac(void);
void func_0201b388(int n);
void func_0201d418(int pos, int width);
void Message_DrawCenteredLine(int a, int b);
int IsStarCollected(u32 course, u8 star);
u8 NumStars(void);
int func_020138dc(void);
int _ZN8SaveData22NumGlowingRabbitsFoundEv(void);

// ---- the European bodies, for the European edition -------------------------
void __real__ZN7Message7AddCharEc(int c);
void __real_func_0201b6f8(int mode);
void __real_func_0201b7cc(void);
void __real_func_0201b100(int mode);
void __real_func_0201ab6c(void);
void __real_func_0201aca4(void);
void __real_func_0201adac(void);
void __real_func_0201cd08(int id);
void __real_func_0201cfb8(int id);
void __real_func_0201d850(u32 course);
void __real__ZN7Message11DisplayTextEt(u16 id);
void __real__ZN7Message30DisplayCourseNameForStarSelectEj(u32 course);
void __real_func_0201f138(void);
void __real_SetBg3Offset(int x, int y);
int __real_ObjectMessageIDToActualMessageID(int id);
void __real_func_ov002_020f2990(char *c);

}  // extern "C"

// ---- loading korean.bin ------------------------------------------------------

extern "C" void game3ds_korean_init(void)
{
    const char *off = getenv("GAME3DS_KOREAN");
    if (off && off[0] == '0') {
        printf("[korean] GAME3DS_KOREAN=0: the European text\n");
        return;
    }
    const char *root = getenv("SM64DS_ASSET_ROOT");
    char path[256];
    snprintf(path, sizeof path, "%s/build/assets/korean.bin", root ? root : "romfs:");
    FILE *f = fopen(path, "rb");
    if (!f)
        return;   // the European edition
    u8 head[0x20];
    const size_t want = 0x20 + sizeof(KoreanData);
    u32 size = 0;
    if (fread(head, 1, sizeof head, f) == sizeof head && memcmp(head, "SM64DS-KOREAN", 13) == 0) {
        memcpy(&size, head + 0x14, 4);
        u32 version;
        memcpy(&version, head + 0x10, 4);
        if (version != 1)
            size = 0;
    }
    if (size != want) {
        printf("[korean] %s is not a korean.bin this game reads (%u bytes); the European text\n",
               path, (unsigned)size);
        fclose(f);
        return;
    }
    KoreanData *d = (KoreanData *)malloc(sizeof(KoreanData));
    if (!d || fread(d, 1, sizeof(KoreanData), f) != sizeof(KoreanData)) {
        printf("[korean] could not read %s; the European text\n", path);
        free(d);
        fclose(f);
        return;
    }
    fclose(f);
    g_ko = d;
    game3ds_korean = 1;
    game3ds_language_slot = 5;   // (a Korean .cia from before langpack.bin has only korean.bin)
    data_020a0f00 = 5;
    printf("[korean] the Korean edition: %d characters, the Spanish slot's graphics\n", kCodes);
}

// ---- the owner's language -------------------------------------------------
// The Korean code never asks: each switch on the language became its Spanish
// arm (language.cpp answers 5), except the ones listed in the wrappers below.
namespace {
struct LangOverride {
    int saved;
    explicit LangOverride(int lang) : saved(game3ds_language_override) { game3ds_language_override = lang; }
    ~LangOverride() { game3ds_language_override = saved; }
};
}  // namespace

// The HUD's sprite at ov002 0x020f1a30 (KO): the English layout of its tiles
// (data_ov002_0210b988[0]), which is where the Korean art fits.
extern "C" void __wrap_func_ov002_020f2990(char *c)
{
    if (!game3ds_korean) {
        __real_func_ov002_020f2990(c);
        return;
    }
    LangOverride o(1);
    __real_func_ov002_020f2990(c);
}

// ---- the character: Message::AddChar (KO 0x0201b620) ------------------------
// g is a glyph: 2c for the left half of code c, 2c+1 for its right (the
// Hangul, codes 0x64 up), and bit 15 for the space's narrower advance.

static void ko_addchar(unsigned g)
{
    const u8 *src = g_ko->font + (g & 0x7fff) * 16;
    const u32 mask = data_0208eecc[data_0209d6c4];
    const int sh = data_0209d678;
    for (int i = 0; i < 16; i++) {
        const u8 b = src[i];
        u32 w = 0;
        if (b & 0x80) w |= 0xf;
        if (b & 0x40) w |= 0xf0;
        if (b & 0x20) w |= 0xf00;
        if (b & 0x10) w |= 0xf000;
        if (b & 0x08) w |= 0xf0000;
        if (b & 0x04) w |= 0xf00000;
        if (b & 0x02) w |= 0xf000000;
        if (b & 0x01) w |= 0xf0000000;
        u32 m = w & mask;
        if (sh != 0)
            m = (m >> ((8 - sh) << 2)) | (m << (sh << 2));
        data_0209d74c[data_0209d704] = (s32)m;
        data_0209d704 = data_0209d704 + 1;
    }
    u8 width;
    if (g & 0x8000)
        width = 6;
    else if (g < 0xc8)
        width = g_ko->width[g >> 1];
    else
        width = (g & 1) ? 4 : 8;
    data_0209d710[data_0209d6a4] = width;
    data_0209d678 = (u8)((sh + width) & 7);
    data_0209d6b0 = (u8)(data_0209d6b0 + width);
    data_0209d6a4 = (u8)(data_0209d6a4 + 1);
}

extern "C" void __wrap__ZN7Message7AddCharEc(int c)
{
    if (!game3ds_korean) {
        __real__ZN7Message7AddCharEc(c);
        return;
    }
    ko_addchar((unsigned)c & 0xffff);
}

// the big font's index for a code: its own code below 0x64, else 0x90 + its
// place among the Hangul it has (0xc4 when it has not)
static int ko_big_index(unsigned code)
{
    if (code < 0x64)
        return code & 0xff;
    int j = 0;
    while (j < kBigFontCodes && g_ko->big_codes[j] != code)
        j++;
    return (j + 0x90) & 0xff;
}

// ---- the line's width: func_0201b6f8 (KO 0x0201b7d8) -------------------------

static void ko_measure(int mode)
{
    const u8 *p = data_0209d6f4;
    int i = 0;
    data_0209d6a4 = 0;
    data_0209d6b0 = 0;
    for (;;) {
        const u8 b = p[i++];
        switch (b) {
        case 0xfc:
            data_0209d6b0 = (u8)(data_0209d6b0 + (mode == 0 ? 6 : 8));
            data_0209d6a4 = (u8)(data_0209d6a4 + 1);
            break;
        case 0xfd:
            data_0209d6bc = 2;
            return;
        case 0xfe:
            break;
        case 0xff:
            data_0209d6bc = 8;
            return;
        default: {
            const unsigned code = (unsigned)b << 8 | p[i++];
            if (mode == 0)
                data_0209d6b0 = (u8)(data_0209d6b0 + (code < 0x64 ? g_ko->width[code] : 12));
            else
                data_0209d6b0 = (u8)(data_0209d6b0 + g_ko->big_width[ko_big_index(code)]);
            data_0209d6a4 = (u8)(data_0209d6a4 + 1);
            break;
        }
        }
    }
}

extern "C" void __wrap_func_0201b6f8(int mode)
{
    if (!game3ds_korean) {
        __real_func_0201b6f8(mode);
        return;
    }
    ko_measure(mode);
}

// ---- the line: func_0201b7cc (KO 0x0201b93c) --------------------------------

static void ko_end_of_message_state(void)
{
    const u8 m = data_0209d694;
    if (m == 1) { data_0209d6bc = 4; data_0209d68c = 0; }
    else if (m == 2) { data_0209d6bc = 6; data_0209d68c = 0; }
    else if (m == 3) { data_0209d6bc = 5; data_0209d68c = 0; }
    else data_0209d6bc = 8;
}

static void ko_line(void)
{
    int added = 0;
    data_0209d678 = data_0209d65c & 7;
    if (data_0209d6b8 == 0) {
        data_0209d704 = 0;
        data_0209d6a4 = 0;
        data_0209d6b0 = 0;
        MultiStore_Int(0, data_0209d74c, 0xf00);
    }
    for (;;) {
        u8 *cur = data_0209d6f4;
        const u8 c = *cur;
        if (c == 0xff) {
            if (added) {
                if (data_0209d69c == 0) {
                    data_0209d6bc = 2;
                } else {
                    data_0209d6bc = 3;
                    data_0209d6a0 = 0;
                }
                data_0209d6b4 = (u8)(data_0209d6b4 + 1);
            } else if (data_0209d69c != 0) {
                const u8 v = data_0209d6b4;
                if (v < data_0209d6f0[6]) {
                    data_0209d6b4 = (u8)(v + 1);
                    data_0209d6a0 = 0;
                    data_0209d6bc = 3;
                } else {
                    ko_end_of_message_state();
                }
            } else {
                ko_end_of_message_state();
            }
            return;
        }
        if (c == 0xfe) {
            const u8 idx = cur[2];
            data_0209d700 = idx;
            (MSG_GEN_TEXT_FUNCS + idx)[-1]();
            data_0209d6f4 = data_0209d6f4 + data_0209d6f4[1];
            continue;
        }
        if (c == 0xfd) {
            const u8 *p = data_0209d6f0;
            const u8 v = data_0209d6b4;
            if (v == p[6]) {
                data_0209d6bc = 7;
                return;
            }
            data_0209d6b4 = (u8)(v + 1);
            if (data_0209d69c != 0) {
                data_0209d6bc = 3;
                data_0209d6a0 = 0;
                if (data_0209d6b4 != p[6])
                    data_0209d6f4 = cur + 1;
            } else {
                if (data_0209d6b4 != p[6])
                    data_0209d6f4 = cur + 1;
                data_0209d6bc = 2;
            }
            return;
        }
        if (c == 0xfc) {
            ko_addchar(0x80a2);
            added = 1;
            data_0209d6f4 = data_0209d6f4 + 1;
            continue;
        }
        const unsigned code = (unsigned)c << 8 | cur[1];
        ko_addchar((code << 1) & 0xffff);
        if (code >= 0x64)
            ko_addchar(((code << 1) + 1) & 0xffff);
        added = 1;
        data_0209d6f4 = data_0209d6f4 + 2;
    }
}

extern "C" void __wrap_func_0201b7cc(void)
{
    if (!game3ds_korean) {
        __real_func_0201b7cc();
        return;
    }
    ko_line();
}

// ---- the big font's line: func_0201b100 (KO 0x0201b0c0) -----------------------

extern "C" void __wrap_func_0201b100(int mode)
{
    if (!game3ds_korean) {
        __real_func_0201b100(mode);
        return;
    }
    unsigned acc = data_0209d65c;
    const int base = func_02054d88() + 0x8000;
    const int size = 0x40;
    for (;;) {
        u8 *cursor = data_0209d6f4;
        const int op = *cursor;
        if (op == 0xfd || op == 0xff)
            return;
        if (op == 0xfe) {
            data_0209d6f4 = cursor + cursor[1];
            continue;
        }
        int idx, space;
        if (op == 0xfc) {
            idx = 0x51;
            space = 1;
        } else {
            space = 0;
            const unsigned code = (unsigned)op << 8 | cursor[1];
            data_0209d6f4 = cursor + 1;
            idx = ko_big_index(code);
        }
        int r4addr;
        if (mode != 0)
            r4addr = _ZN2G213GetBG2CharPtrEv() + (((data_0209d674 + 0x280) + (acc >> 3)) << 5);
        else
            r4addr = _ZN3G2S13GetBG0CharPtrEv() + (((data_0209d6a8 + 0x200) + (acc >> 3)) << 5);
        if ((acc & 0xf) == 0) {
            const int lo2 = idx & 0xf, hi2 = idx & 0xf0;
            MultiCopy_Int((void *)(base + (((lo2 << 1) + (hi2 << 2)) << 5)), (void *)r4addr, size);
            const int w = ((int)data_0209d6b0 + 7) / 8;
            MultiCopy_Int((void *)(base + ((lo2 + ((hi2 << 1) + 0x10)) << 6)), (void *)(r4addr + (w << 5)), size);
        } else {
            const int lo = idx & 0xf, hi = idx & 0xf0;
            int r8addr = base + (((lo << 1) + (hi << 2)) << 5);
            const int sh = acc & 7;
            const short val = space ? 8 : (short)g_ko->big_width[idx];
            const int w8 = (((int)data_0209d6b0 + 7) / 8) << 3;
            int r7addr = r8addr + 0x20;
            const int lshift = sh << 2;
            const int inv = 8 - sh;
            const int rshift = inv << 2;
            for (int row = 0; row < 0x10; row++) {
                const int i = (row >> 3) * w8 + (row & 7);
                ((int *)r4addr)[i] |= *(int *)r8addr << lshift;
                short rem = val;
                rem -= inv;
                if (rem > 0) {
                    (((int *)r4addr) + i)[8] = (int)((*(unsigned *)r8addr >> rshift) | (*(unsigned *)r7addr << lshift));
                    rem = rem - 8;
                }
                if (rem > 0)
                    *(int *)((char *)r4addr + (i << 2) + 0x40) = (int)(*(unsigned *)r7addr >> rshift);
                r8addr += 4;
                r7addr += 4;
                if (row == 7) {
                    r8addr += 0x3e0;
                    r7addr += 0x3e0;
                }
            }
        }
        acc = space ? (acc + 8) & 0xff : (acc + g_ko->big_width[idx]) & 0xff;
        data_0209d6f4 = data_0209d6f4 + 1;
    }
}

// ---- the text's embedded functions (MSG_GEN_TEXT_FUNCS) -----------------------

// the choice marks (KO 0x0201ab0c): two spaces where the European drew one
extern "C" void __wrap_func_0201ab6c(void)
{
    if (!game3ds_korean) {
        __real_func_0201ab6c();
        return;
    }
    const u8 *p = data_0209d6f4;
    const u16 v = (u16)(p[4] | p[3] << 8);
    u8 idx;
    switch (v) {
    case 0:
        data_0209d688 = data_0209d6b4;
        break;
    case 1:
    case 2:
    case 3:
        idx = data_0209d68c;
        data_0209d68c = (u8)(idx + 1);
        data_0209d6e4[idx] = v == 1 ? data_0209d6b0 : data_0209d6b4;
        ko_addchar(0xa2);
        ko_addchar(0xa2);
        data_0209d694 = (u8)v;
        break;
    }
}

// a number (KO 0x0201ac5c): the digits' glyphs
extern "C" void __wrap_func_0201aca4(void)
{
    if (!game3ds_korean) {
        __real_func_0201aca4();
        return;
    }
    static const u16 divisors[3] = {100, 10, 1};
    const u8 *p = data_0209d6f4;
    const u16 v = (u16)(p[4] | p[3] << 8);
    if (v != 0)
        return;
    const u8 mode = p[5], base = p[6];
    u8 num = 0;
    switch (mode) {
    case 0: num = (u8)(base - NumStars()); break;
    case 1: num = (u8)func_020138dc(); break;
    case 2: num = (u8)(base - _ZN8SaveData22NumGlowingRabbitsFoundEv()); break;
    }
    int leading = 0;
    for (int i = 0; i < 3; i++) {
        const int q = num / divisors[i];
        if (q != 0 || leading || i == 2) {
            ko_addchar(g_ko->digits[q]);
            leading = 1;
        }
        num = (u8)(num % divisors[i]);
    }
}

// a button's icon (KO 0x0201ad68): two glyphs, two apart
extern "C" void __wrap_func_0201adac(void)
{
    if (!game3ds_korean) {
        __real_func_0201adac();
        return;
    }
    const u8 *p = data_0209d6f4;
    const u16 idx = (u16)(p[4] | p[3] << 8);
    ko_addchar(g_ko->icons[idx]);
    ko_addchar((g_ko->icons[idx] + 2) & 0xffff);
}

// ---- the screens --------------------------------------------------------------

// how far a course name's number runs before its name (KO 0x0201dcb4)
static int ko_course_number_bytes(u32 course)
{
    if (course <= 0xe)
        return course < 9 ? 4 : 5;
    return 3;
}

// a question with yes and no (KO 0x0201ced0)
extern "C" void __wrap_func_0201cd08(int id)
{
    if (!game3ds_korean) {
        __real_func_0201cd08(id);
        return;
    }
    data_0209d660 = 0;
    func_0201eaac();
    data_0209d668 = 1;
    data_0209d6a8 = 0;
    MultiStore_Int(0, (void *)(_ZN3G2S13GetBG0CharPtrEv() + 0x4000), 0x2000);
    MultiStore16(0x2ff, _ZN3G2S12GetBG0ScrPtrEv(), 0x800);
    data_0209d6d4 = (s16)id;
    int pos = 0xc5;
    if (data_0209d6d4 == 0x29a)
        pos = 0x105;
    else if (data_0209d6d4 == 0x29b)
        pos = 0xe5;
    func_0201d418(pos, 0xb0);
    if (data_0209d6d4 == 0x2c7)   // the Korean save error: no yes and no
        return;
    data_0209d6d4 = 0x293;
    data_0209d6a8 = (u8)(data_0209d6a8 << 1);
    Message_DrawCenteredLine(0x285, 0xa);
    data_0209d6d4 = (s16)(data_0209d6d4 + 1);
    Message_DrawCenteredLine(0x291, 0xa);
}

// a message on the bottom screen (KO 0x0201d1b0): three rows lower
extern "C" void __wrap_func_0201cfb8(int id)
{
    if (!game3ds_korean) {
        __real_func_0201cfb8(id);
        return;
    }
    data_0209d6d4 = (s16)id;
    data_0209d660 = 0;
    func_0201eaac();
    data_0209d668 = 1;
    data_0209d6a8 = 0;
    func_0201d418(0x12f, 0x68);
}

// lay a drawn line's tiles into BG3's map, centred
static void ko_lay_line(u16 *map, int row, u16 *tile, int n)
{
    u16 *p = map + (row + ((0x20 - n) >> 1));
    for (int i = 0; i < n; i++) {
        p[i] = (u16)(*tile + i);
        *(u16 *)((char *)(p + i) + 0x40) = (u16)(n + *tile + i);
    }
}

static int ko_entry_text(int entry)
{
    return data_0209d6fc + 0x28 + data_0209d70c[1] + *(s32 *)(data_0209d708 + entry * 8);
}

// the pause screen's course and star (KO 0x0201da4c)
extern "C" void __wrap_func_0201d850(u32 arg0)
{
    if (!game3ds_korean) {
        __real_func_0201d850(arg0);
        return;
    }
    u16 tile = 0x200;
    u16 row = 0x80;
    data_0209d660 = 0;
    func_0201eaac();
    data_0209d6a8 = 0;
    data_0209d65c = 0;
    MultiStore_Int(0, (void *)(func_02054d88() + 0x4000), 0x2000);
    MultiStore16(0x2ff, _ZN2G212GetBG3ScrPtrEv(), 0x800);
    SetBg3Offset(0, 0);
    u16 *map = (u16 *)_ZN2G212GetBG3ScrPtrEv();

    if (arg0 < 0xf) {
        data_0209d6f0 = data_0209d708 + 0x1468;
        data_0209d6f4 = (u8 *)ko_entry_text(0x1468 / 8);
        ko_line();
        ko_addchar(0xa2);
        const int num = (arg0 + 1) & 0xff;
        if (num / 10 != 0)
            ko_addchar(g_ko->digits[num / 10]);
        ko_addchar(g_ko->digits[num % 10]);
        data_0209d6a8 = (u8)((data_0209d6b0 + 7) / 8);
        func_0201b388(data_0209d6a8);
        data_0209d6c0 = 1;
        const int n = data_0209d6a8;
        ko_lay_line(map, row, &tile, n);
        row += 0x40;
        tile = (u16)(tile + n * 2);
    }

    if ((data_0209f20c != 0 && arg0 < 0x15) || data_0209f20c == 0) {
        if (data_0209f20c != 0) {
            if (arg0 >= 0xf)
                row = 0x100;
        } else if (arg0 >= 0xf) {
            row = 0x140;
        }
        data_0209d65c = 0;
        data_0209d6a4 = 0;
        data_0209d6f0 = data_0209d708 + (arg0 + 0x196) * 8;
        data_0209d6f4 = (u8 *)ko_entry_text(arg0 + 0x196) + ko_course_number_bytes(arg0);
        ko_line();
        const int n = (u8)((data_0209d6b0 + 7) / 8);
        func_0201b388(n);
        ko_lay_line(map, row, &tile, n);
        data_0209d6a8 = (u8)(data_0209d6a8 + n);
        data_0209d6c0 = 1;
        tile = (u16)(tile + n * 2);
        row += 0x40;
    }

    if (arg0 < 0xf || (data_0209f20c != 0 && (arg0 < 0xf || arg0 >= 0x15))) {
        data_0209d65c = 0;
        data_0209d678 = 0;
        data_0209d704 = 0;
        data_0209d6a4 = 0;
        data_0209d6b0 = 0;
        MultiStore_Int(0, data_0209d74c, 0xf00);
        data_0209d6b8 = 1;
        if (IsStarCollected(arg0, data_0209f220) != 0 || data_0209f20c != 0)
            ko_addchar(0xa8);
        else
            ko_addchar(0xaa);
        data_0209d65c = (u8)(data_0209d65c + g_ko->width[0x54]);
        ko_addchar(0xa2);
        data_0209d65c = (u8)(data_0209d65c + g_ko->width[0x51]);
        if (arg0 < 0xf) {
            const int off = arg0 * 7 + 0x1b3;
            data_0209d6f0 = data_0209d708 + (off + (data_0209f20c != 0 ? data_0209f228 : data_0209f220)) * 8;
            row = 0x140;
        } else {
            data_0209d6f0 = data_0209d708 + 0x10e8;
            row = 0x100;
        }
        data_0209d6f4 = (u8 *)(data_0209d6fc + 0x28 + data_0209d70c[1] + *(s32 *)data_0209d6f0);
        ko_line();
        data_0209d65c = 0;
        const int n = (u8)((data_0209d6b0 + 7) / 8);
        func_0201b388(n);
        ko_lay_line(map, row, &tile, n);
        data_0209d6a8 = (u8)(data_0209d6a8 + n);
        data_0209d6c0 = 1;
        tile = (u16)(tile + n * 2);
        row += 0x40;
    }

    if (data_0209f20c != 0) {
        data_0209d6f0 = data_0209d708 + ((arg0 < 0xf || arg0 >= 0x15) ? 0x1460 : 0x1598);
        data_0209d6b8 = 0;
        data_0209d6f4 = (u8 *)(data_0209d6fc + 0x28 + data_0209d70c[1] + *(s32 *)data_0209d6f0);
        ko_line();
        const int n = (u8)((data_0209d6b0 + 7) / 8);
        func_0201b388(n);
        ko_lay_line(map, row, &tile, n);
        data_0209d6a8 = (u8)(data_0209d6a8 + n);
        data_0209d6c0 = 1;
        return;
    }
    if (arg0 >= 0xf)
        return;

    data_0209d6b8 = 0;
    data_0209d6f0 = data_0209d708 + 0x1470;
    data_0209d6f4 = (u8 *)ko_entry_text(0x1470 / 8);
    ko_line();
    const int n = (u8)((data_0209d6b0 + 7) / 8);
    func_0201b388(n);
    ko_lay_line(map, (u16)(row + 0x40), &tile, n);
}

// a message across the top screen (KO 0x0201e590): no course-name case
extern "C" void __wrap__ZN7Message11DisplayTextEt(u16 t)
{
    if (!game3ds_korean) {
        __real__ZN7Message11DisplayTextEt(t);
        return;
    }
    data_0209d6d4 = (s16)t;
    data_0209d660 = 0;
    func_0201eaac();
    const int idx = data_0209d6d4;
    data_0209d6c4 = 1;
    data_0209d6c0 = 0;
    data_0209d668 = 0;
    data_0209d6a8 = 0;
    data_0209d6f0 = data_0209d708 + idx * 8;
    data_0209d6f4 = (u8 *)ko_entry_text(idx);
    MultiStore_Int(0, (void *)(func_02054d88() + 0x4000), 0x2000);
    MultiStore16(0x2ff, _ZN2G212GetBG3ScrPtrEv(), 0x800);
    ko_measure(0);
    data_0209d6a8 = (u8)(((int)data_0209d6b0 + 7) / 8);
    data_0209d65c = 0;
    u16 *q = (u16 *)((char *)_ZN2G212GetBG3ScrPtrEv() + 0x107 * 2);
    const int cnt = data_0209d6a8;
    for (int i = 0; i < cnt; i++) {
        q[i] = (u16)(i + 0x200);
        q[i + 0x20] = (u16)(i + 0x200 + cnt);
    }
    ko_line();
    func_0201b388(data_0209d6a8);
}

// the star select's course name (KO 0x0201e9d0)
extern "C" void __wrap__ZN7Message30DisplayCourseNameForStarSelectEj(u32 base)
{
    if (!game3ds_korean) {
        __real__ZN7Message30DisplayCourseNameForStarSelectEj(base);
        return;
    }
    data_0209d6d4 = (s16)(base + 0x196);
    data_0209d660 = 0;
    func_0201eaac();
    const int idx = data_0209d6d4;
    data_0209d6f0 = data_0209d708 + idx * 8;
    data_0209d6f4 = (u8 *)ko_entry_text(idx) + ko_course_number_bytes(base);
    ko_measure(0);
    data_0209d65c = (u8)((0x100 - data_0209d6b0) / 2);
    data_0209d6a8 = 0x20;
    MultiStore_Int(0, (void *)(func_02054d88() + 0x4000), 0x2000);
    MultiStore16(0x2ff, _ZN2G212GetBG3ScrPtrEv(), 0x800);
    u16 *q;
    if (data_0209d6d4 >= 0x1a5) {
        data_0209d6c4 = 0;
        q = (u16 *)((char *)_ZN2G212GetBG3ScrPtrEv() + 0x400);
    } else {
        data_0209d6c4 = 1;
        q = (u16 *)((char *)_ZN2G212GetBG3ScrPtrEv() + 0x4c0);
    }
    for (int i = 0; i < 0x40; i++)
        q[i] = (u16)(i + 0x200);
    SetSubBg1Offset(0, 0);
    ko_line();
    func_0201b388(data_0209d6a8);
    if (data_0209d6d4 >= 0x1a5)
        return;

    data_0209d6c0 = 1;
    data_0209d6f0 = data_0209d708 + 0x1470;
    data_0209d6f4 = (u8 *)(data_0209d6fc + 0x28 + data_0209d70c[1] + *(s32 *)data_0209d6f0);
    ko_measure(0);
    data_0209d65c = (u8)((0xbe - data_0209d6b0) / 2);
    data_0209d6a8 = 0x20;
    MultiStore_Int(0, (void *)(func_02054d88() + 0x4800), 0x800);
    q = (u16 *)((char *)_ZN2G212GetBG3ScrPtrEv() + 0x240);
    for (int i = 0; i < 0x40; i++)
        q[i] = (u16)(i + 0x240);
    ko_line();
    func_0201b388(data_0209d6a8);
}

// the save prompt's text box (KO 0x0201f310): BG3 at x -9, not -17
namespace {
int g_in_f138;
}
extern "C" void __wrap_func_0201f138(void)
{
    if (!game3ds_korean) {
        __real_func_0201f138();
        return;
    }
    g_in_f138 = 1;
    __real_func_0201f138();
    g_in_f138 = 0;
}

extern "C" void __wrap_SetBg3Offset(int x, int y)
{
    if (g_in_f138 && x == -0x11 && y == -0x3c)
        x = -9;
    __real_SetBg3Offset(x, y);
}

// an object's message id to the bank's (KO 0x0201fde8): the Korean table
extern "C" int __wrap_ObjectMessageIDToActualMessageID(int id)
{
    if (!game3ds_korean)
        return __real_ObjectMessageIDToActualMessageID(id);
    const u16 *t = g_ko->msg_ids;
    for (int i = kMsgIdPairs - 1; i > 0; i--) {
        if (t[i * 2] > id && t[(i - 1) * 2] <= id)
            return (u16)((id - t[(i - 1) * 2]) + t[(i - 1) * 2 + 1]);
    }
    return id;
}

// ---- the title menu (ov007) ---------------------------------------------------
// The Korean cartridge's menu takes the Spanish slot's resources, which the
// builder makes the Korean ones (the prompts over the title, texBank_S's file
// and title menus): language.cpp makes the owner Spanish for it.

// The Korean cartridge has no mini-games: its menu has no Rec Room, and its
// bank has no text for them. The builder leaves the Rec Room button out of the
// picture, and here pressing it (func_ov007_020b63e4, a menu button pressed;
// button 2 is the Rec Room) does nothing.
extern "C" void __real_func_ov007_020b63e4(char **button);
extern "C" void __wrap_func_ov007_020b63e4(char **button)
{
    if (game3ds_korean && *(unsigned short *)button[0] == 2)
        return;
    __real_func_ov007_020b63e4(button);
}

// the menu's choice (GAME3DS_KOREAN_TRACE=1: which button is which)
extern "C" void __real_func_ov007_020aec94(int item);
extern "C" void __wrap_func_ov007_020aec94(int item)
{
    static const bool trace = getenv("GAME3DS_KOREAN_TRACE") != nullptr;
    if (trace)
        printf("[menu] item %d\n", item);
    __real_func_ov007_020aec94(item);
}
