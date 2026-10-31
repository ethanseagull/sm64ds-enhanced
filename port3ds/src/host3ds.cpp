#include "host3ds.h"

#include <3ds.h>

bool host3ds_frame_continue()
{
    if (!aptMainLoop()) return false;
    hidScanInput();
    return !(hidKeysDown() & KEY_START);
}

uint64_t host3ds_ms()
{
    return osGetTime();
}
