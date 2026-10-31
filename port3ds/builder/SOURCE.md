# The builder's source

The source of `SM64DS-Enhanced-Builder.exe` from the release:

- `Builder.cs`: the window and the build. It extracts the game data from your
  dump, packs it, makes the HOME Menu icon and banner, and runs makerom.
- `Extract.cs`: the extraction. It reads the game's files and data tables out
  of your dump and checks them against the hashes the build expects.
- `Settings.cs`: the build settings (the console and the game's options),
  written into the `.cia` as the game's `options.txt`.
- `Face.cs`: the 3D HOME Menu banner. `face/face.template` is the banner model
  with every value from the game zeroed out, and `face/face.recipe` says where
  each value goes; the builder fills them in from your dump.
- `build_exe.py`: compiles the `.exe` from those four files with the C#
  compiler that comes with Windows. The release's `.exe` is built this way by
  [GitHub Actions](../../.github/workflows/builder.yml).
- `package.py`: makes the zip, the `.exe` plus its `builder-files` folder: the
  3DS game (built from `port3ds/`, see [docs/BUILDING.md](../../docs/BUILDING.md)),
  makerom and bannertool. It refuses to package if any file it ships contains
  the game's bytes.
- `builder.ico`: the file's icon, drawn by `make_icon.cs`.

The window's music and sounds aren't included here. Licences are in
`THIRD-PARTY.txt` and `licenses/`.
