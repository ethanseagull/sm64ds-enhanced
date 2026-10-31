// Can a 3DS app put memory at the DS's own addresses?
//
// port/ntr/io.cpp maps main RAM, I/O, palettes, VRAM and OAM at their real
// DS addresses and ntr::host_ptr() is an identity cast, so the port depends on
// this. The probe tries two ways per region and reports the kernel's results.
#pragma once

// Prints a table to stdout. Returns the number of regions that could be
// mapped and read back.
int memprobe_run();
