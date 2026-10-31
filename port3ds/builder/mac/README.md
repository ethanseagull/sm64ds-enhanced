# The Mac builder

`SM64DS Enhanced Builder.app` is the Windows builder for macOS. It works the
same way: pick your European dump (or drag it onto the window or the app's
Dock icon), choose your settings, click Build, and the `.cia` is saved next to
your dump. It runs on macOS 11 or later, on Apple silicon and Intel Macs, and a
build takes a few seconds.

## The source

- `Builder.swift`: the window and the build (`Builder.cs`). It extracts the
  game data from your dump, packs it, makes the HOME Menu icon and banner, and
  runs makerom.
- `Extract.swift`: the extraction, a port of the PC port's
  `extract_assets.ps1` (the Windows builder's `Extract.cs`).
- `Settings.swift`: the build settings (`Settings.cs`), written into the
  `.cia` as the game's `options.txt`.
- `Face.swift`: the 3D HOME Menu banner (`Face.cs`).
- `package.py`: makes the app and its zip.
- `THIRD-PARTY.txt` and `licenses/`: what's in the app and the licences that
  come with it. `package.py` adds them to the licences of the Windows
  release.

## Making the app

You need Xcode or its Command Line Tools (`xcode-select --install`), Python 3
and git. Then, from the top of the repository:

    python3 port3ds/builder/mac/package.py

That takes about a minute. It:

1. downloads the latest Windows release and takes from it what isn't Windows
   code: the 3DS game (`game3ds.elf`, `game3ds.rsf`), the extraction recipe and
   manifest, the banner template and recipe, `banner.wav`, the music and the
   licences. It checks that they are one build's, as upstream's `package.py`
   does.
2. builds makerom 0.19.0 and bannertool 1.2.3 from source at pinned commits
   for arm64 and x86_64, for macOS 11 and later. The official makerom builds for
   macOS need macOS 15, and bannertool has none.
3. compiles the Swift for both, and draws the app icon.
4. signs the app (ad hoc) and zips it.

The result goes in `port3ds/builder/mac/build/`: `SM64DS Enhanced Builder.app` and
`SM64DS-Enhanced-Builder-<version>-mac.zip`.

Options:

- `--from PATH` takes the Windows release you already have (its zip, its
  `builder-files` folder, or the first 1.0.0's single `.exe`) and skips the
  download. `--release TAG` downloads a particular release. For a game you
  built yourself ([docs/BUILDING.md](../../../docs/BUILDING.md)), give it the
  folder `../package.py` leaves in `build3ds-wide/SM64DS-Enhanced-Builder`.
  The music and the HOME Menu sound aren't in the repository, so an app made
  from that folder has none, as with the Windows builder.
- `--makerom PATH` and `--bannertool PATH` use Mac builds you already have.
- `--arch arm64` or `--arch x86_64` builds for one kind of Mac only.
- `--sign "Developer ID Application: ..."` signs with your certificate and
  hardened runtime, ready for `notarytool`. Without it the app is signed ad
  hoc, so macOS asks before it opens the first time.

When the Windows builder's C# changes, port the change to the Swift here.
`package.py` warns when it is given a release it hasn't been checked against,
and refuses one with files or banner recipe lines it doesn't know.

## How it was checked

With the European dump, against the Windows builder's own C# run under Mono:

- the extracted game data (2080 files, including the catalogs and
  `romdata.bin`), `files.pak`, the 3D banner (with and without the title
  logo), `options.txt` and the DS icon and logo pixels are byte-for-byte the
  same.
- the HOME Menu icon's PNG is drawn with Core Graphics, not GDI+, so its
  scaling differs by a little.

The makerom built here makes the same `.cia` content as the official 0.19.0
build. Only the ticket's title key and ID and the content ID differ, and they
differ between any two runs of makerom. The bannertool built here makes the
same icon and banner as its own CMake Release build. It has one change:
`lz11.cpp` pads with a fixed 4-byte array, because clang won't initialise a
variable-length one.

## Without the window

    "SM64DS Enhanced Builder.app/Contents/MacOS/SM64DSEnhancedBuilder" \
        --rom DUMP.nds --out OUT.cia [--option console=old] [--option fps_counter=1] [--keep]

This runs the same build without the window and writes its log to
`OUT.cia.log`. It exits with 0 on success, 1 if the build failed, and 2 for bad
arguments. It uses the default settings, not the window's saved ones.
`--option` takes `options.txt`'s keys (`60fps`, `anti_aliasing`,
`fps_counter`, `analog_controls`) and `console=new|old`. Put `console` first,
because it sets that console's defaults. `--keep` leaves the work folder in
place.
