#ifndef PORT3DS_MSVC_DECLSPEC_H
#define PORT3DS_MSVC_DECLSPEC_H
// __declspec, force-included into every C/C++ file of the game build, keeping
// the forms that matter at link time.
// selectany becomes a weak definition. allocate("name$suffix") becomes the
// section ".data.name$suffix": 3dsx.ld gathers *(.data.*) and the link sorts by
// name, which matches MSVC's order for grouped sections. The HAL relies on that
// to keep DS globals from different files next to each other. Those sections
// are retained, since some are unreferenced gaps that only keep neighbours
// apart and --gc-sections would drop them. align is passed through (and stops
// GCC from raising char arrays to word alignment, which would open gaps). The
// rest are empty: naked functions are rewritten by gccfix and there are no DLLs.
#define __declspec(x) PORT3DS_DECLSPEC_##x
#define PORT3DS_DECLSPEC_selectany __attribute__((weak))
#define PORT3DS_DECLSPEC_noinline __attribute__((noinline))
#define PORT3DS_DECLSPEC_noreturn __attribute__((noreturn))
#define PORT3DS_DECLSPEC_align(n) __attribute__((aligned(n)))
// The flags go in the section name ("awR", R = SHF_GNU_RETAIN, which this GCC's
// retain attribute doesn't emit); "@" comments out the flags GCC appends.
#define PORT3DS_DECLSPEC_allocate(s)     __attribute__((section(".data." s ",\"awR\",%progbits @"), used))
#define PORT3DS_DECLSPEC_allocator
#define PORT3DS_DECLSPEC_deprecated
#define PORT3DS_DECLSPEC_dllexport
#define PORT3DS_DECLSPEC_dllimport
#define PORT3DS_DECLSPEC_naked
#define PORT3DS_DECLSPEC_novtable
#define PORT3DS_DECLSPEC_thread
#endif
