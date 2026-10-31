# Building

This is set up for Windows with devkitPro. Other systems haven't been tried.

## What you need

- devkitPro in `C:\devkitPro` with devkitARM, libctru, citro3d, the 3DS tools
  and its CMake toolchain. CMake, Make and Ninja in devkitPro's MSYS2 shell.
- Python 3.12 with `ndspy`, `capstone` and `pyelftools`.
- Visual Studio 2022 with the C++ x86 tools: the upstream port's generators
  use its `undname.exe` (or set `SM64DS_UNDNAME` to one).
- [ds-decomp](https://github.com/AetiasHax/ds-decomp)'s `dsd` 0.11.0.
- Your own European Super Mario 64 DS dump (`.nds`, game code ASMP).

## 1. The upstream port

The 3DS build compiles the PC port's platform layer from a pinned upstream
commit with this repo's patches on top. In devkitPro's MSYS2 shell:

```bash
git clone https://github.com/ethanseagull/sm64ds-enhanced.git
cd sm64ds-enhanced
git clone https://github.com/tangosdev/sm64ds-decomp.git sm64ds-decomp
git -C sm64ds-decomp worktree add -b 3ds/step5 ../port-0.9.1 bf42a2418d4396bdd8bb2d61a2cff6cf85c2b743
git -C port-0.9.1 am ../port3ds/upstream-patches/*.patch
```

## 2. Your game data

Put `dsd.exe` at `port-0.9.1/tools/bin/dsd.exe`, then in PowerShell:

```powershell
$rom = 'C:\path\to\your-dump.nds'
py -3.12 -m pip install ndspy capstone pyelftools
py -3.12 port-0.9.1/tools/unpack.py $rom
py -3.12 port-0.9.1/tools/asset_catalog.py generate $rom
& .\port-0.9.1\tools\bin\dsd.exe rom extract --rom $rom --output-path port-0.9.1/extracted/dsd
py -3.12 -I port3ds/tools/extract_files.py $rom port-0.9.1
```

All of this stays on your PC; none of it goes in the repo.

## 3. Build

In devkitPro's MSYS2 shell:

```bash
bash port3ds/tools/pcprobe.sh
GAME3DS_WIDE=1 bash port3ds/build.sh --target game3ds_3dsx
```

`pcprobe.sh` configures the upstream project and regenerates
`port3ds/game/manifest.cmake`; run it again after changing the upstream
patches. The game ends up in `build3ds-wide/game3ds.3dsx` and `game3ds.elf`.

## 4. A .cia

The simplest way is the builder. Put makerom 0.19.0 at
`build-banner/tools/makerom-0.19.0/makerom.exe` and bannertool 1.2.3's Windows
build in `build-banner/tools/bannertool-1.2.3/bannertool-1.2.3-windows/` (with
its `LICENSE.txt`), then:

```bash
py -I port3ds/builder/package.py --label dev
```

That writes `build3ds-wide/SM64DS-Enhanced-Builder.zip` and the same files
unzipped in `build3ds-wide/SM64DS-Enhanced-Builder/`. Run
`SM64DS-Enhanced-Builder.exe` from there, choose your dump, and it saves the
`.cia` next to the dump. (The release's music and sounds aren't in this
repository, so a build of your own has none.)

## Running the .3dsx instead

`port3ds/tools/stage_assets.ps1 -Sd E:\` copies the game data to an SD card's
`sm64ds3ds` folder (without `-Sd` it uses Azahar's virtual SD). Put
`game3ds.3dsx` in `E:\3ds\` and start it from the Homebrew Launcher, or open it
in Azahar. Saves go to `sm64ds3ds/save/sm64ds.sav`.
