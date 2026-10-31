// Gate 1 shims for the 3DS (GCC) build.
//
// The PC build uses port/hal/shims.cpp, which also defines a forwarder for
// _Z14ApproachLinearRiii that only exists because MSVC mangles
// ApproachLinear(int&, int, int) differently. GCC uses the same Itanium
// mangling as the ROM, so src/_Z14ApproachLinearRiii.cpp already defines it and
// the forwarder would clash. Everything else matches port/hal/shims.cpp.
#include <stdio.h>
#include <stdlib.h>
#include "Fader.h"
#include "FaderBrightness.h"

Fader::~Fader() {}
FaderBrightness::~FaderBrightness() {}

void Fader::AdvanceFade() {}
int Fader::SetBackwardTime(u32) { return 0; }
int Fader::SetForwardTime(u32) { return 0; }
int Fader::IsAtStart() { return 0; }
int Fader::IsAtEnd() { return 0; }

void FaderBrightness::AdvanceFade() { AdvanceInterp(); }

// Gate 1 links no heap; the harness never deletes a Fader. Loud if it does.
extern "C" void _ZN6Memory16operator_delete2EPv(void *ptr)
{
    fprintf(stderr,
            "smoke: Memory::operator_delete2(%p) reached, but gate 1 links no "
            "heap -- see port3ds/src/shims_gate1.cpp\n", ptr);
    abort();
}
