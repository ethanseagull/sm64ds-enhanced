// The game's language: one of the European ROM's five, or an edition's.
//
// The 3DS game is the European ROM's code, which has English, French, German,
// Italian and Spanish, and picks one by the DS owner's language: its switches
// ask GetOwnerLanguage (1 English, 2 French, 3 German, 4 Italian, 5 Spanish),
// and the title menu asks OS_GetOwnerInfo (func_02059eb0). The 3DS has no DS
// owner, so the game is English, unless the .cia has
// build/assets/langpack.bin, which the builder (sm64ds_builder.py,
// Builder.cs) writes for any other language:
//
//   0x00  "SM64DS-LANGPACK\0", u32 version (1), u32 total size
//   0x18  the language's code, 8 bytes ("fr", "ja", "ko", "en-us", ...)
//   0x20  u8 the DS language the game runs as (1-5, above), u8 flags (1: the
//         font widths follow)
//   0x40  the font's widths (256), the big font's widths (256)
//
// The game then runs as that DS language. The Japanese, Korean and American
// English editions are a second cartridge's text and pictures, which the
// builder put in the files of the slot the edition runs as (the Japanese and
// American ones the English slot, the Korean one the Spanish); the Japanese
// and American fonts come with their own widths, which replace the European
// ones (data_0208f074, data_0208f174). korean.cpp is the Korean edition's
// text code.
//
// GAME3DS_LANGUAGE=1..5 (env.txt) runs the game as that DS language instead.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned int u32;

extern "C" {

int game3ds_language_slot = -1;       // the DS language the game runs as; -1: the DS owner's
int game3ds_language_override = -1;   // for one call (korean.cpp)
char game3ds_language_code[9];        // langpack.bin's, "" without one

extern u8 data_020a0f00;     // the owner's language, as the boot read it
extern u8 data_0208f074[];   // the font's widths, by character
extern u8 data_0208f174[];   // the big font's

int __real_GetOwnerLanguage(void);
void __real_func_02059eb0(unsigned char *info);
void game3ds_korean_init(void);

}  // extern "C"

static bool read_langpack(void)
{
    const char *root = getenv("SM64DS_ASSET_ROOT");
    char path[256];
    snprintf(path, sizeof path, "%s/build/assets/langpack.bin", root ? root : "romfs:");
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    u8 head[0x40];
    u32 version = 0, size = 0;
    if (fread(head, 1, sizeof head, f) == sizeof head && memcmp(head, "SM64DS-LANGPACK", 16) == 0) {
        memcpy(&version, head + 0x10, 4);
        memcpy(&size, head + 0x14, 4);
    }
    const bool widths = head[0x21] & 1;
    if (version != 1 || size != (widths ? 0x240u : 0x40u) || head[0x20] < 1 || head[0x20] > 5) {
        printf("[language] %s is not a langpack.bin this game reads; English\n", path);
        fclose(f);
        return false;
    }
    u8 w[512];
    if (widths && fread(w, 1, sizeof w, f) != sizeof w) {
        printf("[language] could not read %s; English\n", path);
        fclose(f);
        return false;
    }
    fclose(f);
    memcpy(game3ds_language_code, head + 0x18, 8);
    game3ds_language_slot = head[0x20];
    if (widths) {
        memcpy(data_0208f074, w, 256);
        memcpy(data_0208f174, w + 256, 256);
    }
    printf("[language] %s: the game runs as DS language %d%s\n", game3ds_language_code,
           game3ds_language_slot, widths ? ", with its fonts' widths" : "");
    return true;
}

extern "C" void game3ds_language_init(void)
{
    const bool pack = read_langpack();
    if (!pack || strcmp(game3ds_language_code, "ko") == 0)
        game3ds_korean_init();   // korean.bin (a Korean .cia from before langpack.bin has only it)
    if (const char *e = getenv("GAME3DS_LANGUAGE")) {
        const int lang = atoi(e);
        if (lang >= 1 && lang <= 5) {
            game3ds_language_slot = lang;
            printf("[language] GAME3DS_LANGUAGE: DS language %d\n", lang);
        }
    }
    if (game3ds_language_slot >= 0)
        data_020a0f00 = (u8)game3ds_language_slot;
}

extern "C" int __wrap_GetOwnerLanguage(void)
{
    if (game3ds_language_override >= 0)
        return game3ds_language_override;
    if (game3ds_language_slot >= 0)
        return game3ds_language_slot;
    return __real_GetOwnerLanguage();
}

// The title menu (ov007) reads the owner's language itself, and picks its
// pictures and prompts by it.
extern "C" void __wrap_func_02059eb0(unsigned char *info)
{
    __real_func_02059eb0(info);
    if (game3ds_language_slot >= 0)
        info[0] = (unsigned char)game3ds_language_slot;
}

// A romdata.bin the builder made from the American or Japanese cartridge alone
// (sm64ds_builder.py's standalone_extract) isn't the European one this build's
// table offsets were computed from, but it has its layout: the European tables
// in the same places, filled from that cartridge. Its manifest says so, after
// its own hash and length: "layout <the European blob's sha256>". When that is
// the blob this build expects (port_romdata_expect_sha, beside the offsets),
// hal/romdata_loader.cpp's guard against another build's data is told to let
// it through; the loader still checks the blob against its manifest. Called
// just before port_romdata_load (early_io.cpp).
extern "C" const char port_romdata_expect_sha[] __attribute__((weak));
extern "C" const unsigned long long port_romdata_expect_len __attribute__((weak));
extern "C" void game3ds_early_mark(const char *what);

extern "C" void game3ds_romdata_layout(void)
{
    const char *volatile expect = port_romdata_expect_sha;
    const unsigned long long *volatile expect_len = &port_romdata_expect_len;
    if (!expect || !expect_len)
        return;
    const char *root = getenv("SM64DS_ASSET_ROOT");
    char path[256];
    snprintf(path, sizeof path, "%s/build/assets/romdata.manifest", root ? root : "romfs:");
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256] = "";
    if (!fgets(line, sizeof line, f))
        line[0] = 0;
    fclose(f);
    char sha[80], layout[80];
    unsigned long long len = 0;
    if (sscanf(line, "# romdata-manifest v1 %79s %llu layout %79s", sha, &len, layout) == 3 &&
        strcmp(layout, expect) == 0 && len == *expect_len) {
        setenv("SM64DS_ROMDATA_NO_GUARD", "1", 1);
        game3ds_early_mark("romdata: another cartridge's, in this build's layout");
    }
}
