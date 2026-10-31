// System services for code that includes the decomp's headers.
//
// libctru's <3ds.h> can't be in the same file as the decomp's types.h:
// libctru's u32 is uint32_t (unsigned long here) and the decomp's is unsigned
// int, and C++ rejects the conflicting typedefs. Game-side code calls these
// instead.
#pragma once

#include <stdint.h>

// One pass of the 3DS main loop: services the applet manager and reads
// buttons. Returns false when the app should quit (closed from HOME, or START).
bool host3ds_frame_continue();

// Milliseconds since boot.
uint64_t host3ds_ms();
