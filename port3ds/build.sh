#!/bin/bash
# Build the 3DS port. Run from devkitPro's MSYS2 shell, or from Windows with:
#   C:\devkitPro\msys2\usr\bin\bash.exe -l /c/sm64ds-3ds/port3ds/build.sh [cmake --build args]
#
# Uses Ninja and ccache (pacman -S ninja ccache). SM64DS_OPT overrides the
# optimization/debug flags (default "-O2 -g1"; line tables are enough for
# crash addresses).
set -e
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export CCACHE_DIR="${CCACHE_DIR:-$HOME/.ccache}"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-20G}"
here="$(cd "$(dirname "$0")" && pwd)"
build="$here/../build3ds"
# GAME3DS_WIDE=1: the 400x240 widescreen build (CMakeLists.txt), in a tree of its own
wide=OFF
if [ -n "$GAME3DS_WIDE" ]; then build="$here/../build3ds-wide"; wide=ON; fi
# GAME3DS_BUILD_DIR: a tree of its own (an A/B of SM64DS_OPT beside the usual one)
if [ -n "$GAME3DS_BUILD_DIR" ]; then build="$GAME3DS_BUILD_DIR"; fi
# GAME3DS_ALLOC_CENSUS=1: wrap the allocators (game/alloc_census.cpp)
census=OFF
if [ -n "$GAME3DS_ALLOC_CENSUS" ]; then census=ON; fi
# The build tools are Windows Python scripts.
python_win="${PYTHON_WIN:-$(cygpath -m "$LOCALAPPDATA")/Programs/Python/Python312/python.exe}"
if [ -f "$build/CMakeCache.txt" ] && ! grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja' "$build/CMakeCache.txt"; then
    echo "build.sh: $build was configured for another generator; remove it first" >&2
    exit 1
fi
cmake -S "$here" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DGAME3DS_WIDE=$wide \
    -DGAME3DS_ALLOC_CENSUS=$census \
    -DSM64DS_OPT:STRING="${SM64DS_OPT:--O2 -g1}" \
    -DPYTHON:STRING="$python_win" \
    -DDKA_BIN:STRING="$(cygpath -m "$DEVKITARM")/bin" > "$build.configure.log" 2>&1 \
    || { tail -30 "$build.configure.log"; exit 1; }
cmake --build "$build" "$@"