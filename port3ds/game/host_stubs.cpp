// What the PC host build defines in code the 3DS build does not compile, or
// compiles in its other arm, kept to the names the game link asks for.

// The title menu's sprite draw (src/func_ov007_020c421c.c) only defines these
// hooks in its MSVC-only branch, which GCC doesn't build, but
// hal/menu_recipe_title.cpp still sets them. So on 3DS the title menu draws
// every sprite object (the PC's selective one-screen draw isn't used).
int (*g_port_title_draw_skip)(char *obj);
void (*g_port_title_draw_end)(void);

// tests/fault_probe.h (included by walk_window.cpp) defines the actor
// class-name lookup the fault reports use; hal/actor_registry.cpp sets it.
typedef const char *(*port_classname_fn)(unsigned id);
extern "C" { port_classname_fn port_classname_resolver = 0; }

// tests/walk_window.cpp's scripted host key (SM64DS_HOST_KEY), which the
// menu seam asks for Backspace: no keyboard and no script on the 3DS.
extern "C" int port_host_key_script(int) { return 0; }

// The two ROM-clean install errors from hal/asset_root_refuse.cpp are Win32
// message boxes on PC. On 3DS the message goes to the log and bottom screen and
// the game stops. The data normally comes from the title's own RomFS, so a
// player only hits these running a .3dsx next to another build's
// sdmc:/sm64ds3ds.
#include <cstdio>
#include <cstdlib>
extern "C" void game3ds_early_mark(const char *what);   // game/early_io.cpp: written at once
extern "C" void port_asset_root_refuse(const char *wanted)
{
    game3ds_early_mark("FATAL: no game data folder (SM64DS_ASSET_ROOT)");
    std::printf("FATAL: no game data folder (SM64DS_ASSET_ROOT); it was about to open %s\n",
                wanted ? wanted : "?");
    std::fflush(stdout);
    std::exit(2);
}
extern "C" void port_asset_mismatch_refuse(const char *detail)
{
    game3ds_early_mark("FATAL: the game data belongs to a different build (romdata.bin)");
    std::printf("FATAL: the game data belongs to a different build of the game:\n%s\n"
                "Rebuild the .cia with this build's builder.\n",
                detail ? detail : "  (no detail)");
    std::fflush(stdout);
    std::exit(2);
}
