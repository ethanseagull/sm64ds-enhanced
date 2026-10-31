#!/bin/bash
# Regenerate the 3DS game build from the PC port's own CMake. Run after every
# upstream update of port-0.9.1 (from devkitPro's MSYS2 shell, or from Windows:
#   C:\devkitPro\msys2\usr\bin\bash.exe -l /c/sm64ds-3ds/port3ds/tools/pcprobe.sh).
#
#  1. configure port/CMakeLists.txt with the 3DS toolchain and a CMake File API
#     query into build-pcprobe/ (nothing is compiled)
#  2. run just the PC build's source generators for the game targets
#  3. export port3ds/game/manifest.cmake with tools/pcmanifest.py
#
# Needs extracted/dsd and extracted/arm9_dec.bin in the upstream checkout.
#
# ROM-clean is the default (PORT_ROM_CLEAN=OFF for the old baked tables): the
# generated table files declare every ROM table zeroed, and the data goes to
# port-0.9.1/build/assets/romdata.bin (plus its manifest and recipe), which the
# game loads at boot. So game3ds.elf has none of Nintendo's data, and the
# builder makes romdata.bin from the player's own dump.
set -e
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
pywin="$(cygpath -u "$LOCALAPPDATA")/Programs/Python/Python312"
export PATH="$pywin:$PATH"
here="$(cd "$(dirname "$0")" && pwd)"
top="$here/../.."
probe="$top/build-pcprobe"
port_root="$top/port-0.9.1"

mkdir -p "$probe/.cmake/api/v1/query"
: > "$probe/.cmake/api/v1/query/codemodel-v2"
cmake -S "$port_root/port" -B "$probe" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
    -DPORT_ROM_CLEAN="${PORT_ROM_CLEAN:-ON}" > "$probe/configure.log" 2>&1 \
    || { tail -30 "$probe/configure.log"; exit 1; }
echo "pcprobe: configured (log: build-pcprobe/configure.log)"

python.exe "$(cygpath -m "$here/pcgen_list.py")" "$(cygpath -m "$probe")" > "$probe/gen_targets.txt"
cd "$probe"
while read -r tgt files; do
    # -k: one generator failing must not hide the rest; the manifest step
    # below reports what is missing.
    make -k -j"$(nproc)" -f "CMakeFiles/$tgt.dir/build.make" $files > "gen_$tgt.log" 2>&1 || true
    echo "pcprobe: $tgt generators run (log: build-pcprobe/gen_$tgt.log)"
done < gen_targets.txt
# ROM-CLEAN: the blob's self-check (its bytes == the baked arrays') and the
# kit's recipe (romblob_ready_ww), which no source file depends on
if [ "${PORT_ROM_CLEAN:-ON}" != OFF ]; then
    mkdir -p assets   # the self-check's stamp
    make -f CMakeFiles/romblob_ready_ww.dir/build.make romblob_ready_ww > gen_romblob.log 2>&1 \
        || { tail -20 gen_romblob.log; exit 1; }
    echo "pcprobe: romdata.bin verified, recipe written (log: build-pcprobe/gen_romblob.log)"
fi
missing=0
for f in $(cut -d' ' -f2- gen_targets.txt); do [ -e "$f" ] || { echo "  missing: $f"; missing=$((missing+1)); }; done
echo "pcprobe: $missing generated source(s) missing"

python.exe "$(cygpath -m "$here/pcmanifest.py")" --probe "$(cygpath -m "$probe")" \
    --port-root "$(cygpath -m "$port_root")" \
    --out "$(cygpath -m "$here/../game/manifest.cmake")" \
    --exclude "$(cygpath -m "$here/../game/exclude.txt")"