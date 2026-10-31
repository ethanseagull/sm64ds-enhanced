#!/usr/bin/env python3
"""Make the shareable builder: build3ds-wide/SM64DS-Enhanced-Builder.zip.

  py -I port3ds/builder/package.py --label 1.0.0

The zip holds SM64DS-Enhanced-Builder.exe (build_exe.py) and beside it the
builder-files folder (also left unzipped in build3ds-wide/SM64DS-Enhanced-Builder):

  game3ds.elf          build3ds-wide's, ROM-CLEAN (tools/pcprobe.sh), debug info stripped
  romdata.recipe.tsv   where each of its tables lives in a dump (offsets, hashes)
  romdata.manifest     what the game checks romdata.bin against (hashes)
  game3ds.rsf          port3ds/cia's, with the code left uncompressed
  banner.wav           the HOME Menu sound (port3ds/builder/banner.wav)
  face.template, face.recipe   the 3D banner with the game's values zeroed, and
                       where they go (port3ds/builder/face, made by
                       port3ds/banner/face/face_recipe.py); the builder fills
                       them in from the dump (Face.cs)
  makerom.exe, bannertool.exe, THIRD-PARTY.txt, LABEL.txt
  music.mp3, done.mp3  the window's music and the sound a finished build plays
                       (the three sounds only where port3ds/builder has them)

It also makes build3ds-wide/SM64DS-Enhanced-Builder-Linux-Mac.zip: sm64ds_builder.py
(the builder in Python) beside the same builder-files less the Windows programs
and the window's sounds, plus makerom for Linux and macOS (MAKEROM_UNIX) and
selftest.txt, what sm64ds_builder.py --self-test checks a computer against.

The .exe carries none of it inside: one that unpacked programs out of itself
into %TEMP% and ran them was flagged by antivirus scanners (10 of 71 on
VirusTotal for 1.0.0's). The release's .exe is GitHub Actions' build of the
same source (release.py puts it in this zip's place), so it can be signed.

THE GATE: nothing of Nintendo's may ship, so before it zips anything this
checks every shipped file for the ROM's bytes -- each distinctive 64-byte
piece of romdata.bin (the tables the build zeroed) and pieces sampled from
every extracted game file -- and refuses on a hit, except the two pieces
upstream's own committed source carries (ALLOWED below). It also checks that
the elf, the recipe and the manifest are one build's (the same romdata.bin
hash), as the game itself does at boot.
"""
import argparse
import hashlib
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))   # py -I leaves this folder out
import build_exe  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
UP = ROOT / 'port-0.9.1'
TOOLS = ROOT / 'build-banner' / 'tools'
DKA = Path(r'C:\devkitPro\devkitARM\bin')

# makerom 0.19.0 for Linux and macOS (in build-banner/tools/makerom-0.19.0/<these>/makerom),
# by their sha256: built from Project_CTR's source by the makerom workflow
# (public/makerom.yml) on 2026-10-31, for glibc 2.17 and macOS 11. Project_CTR's
# own release builds need glibc 2.38 and macOS 15.
MAKEROM_UNIX = {
    'linux-x86_64': '13da617a182aee92d2aabdad7428167376c8beded052e8bfdf72e58e5ef7eab7',
    'macos-arm64': '9ecc3f2216513026394384ae20c4205560d5d09c54185fd38b1e1a358941fc52',
    'macos-x86_64': 'a5c21b2bd6c7bb8ad162e38ba63256c86951722d22b665303267cda97dc3f35e',
}

UNIX_README = """\
Super Mario 64 DS Enhanced builder for Linux and macOS

You need Python 3.8 or newer (python3 --version) and your own dump of the
European Super Mario 64 DS cartridge (game code ASMP), as an .nds file.

In a terminal, in this folder:

    python3 sm64ds_builder.py

and drag your dump into the terminal when it asks. The .cia goes next to
the dump. Copy it to your SD card and install it with FBI.

    python3 sm64ds_builder.py DUMP.nds --option console=old

builds for an Old 3DS; python3 sm64ds_builder.py --help lists the settings.
python3 sm64ds_builder.py --self-test checks this computer gives the same
results as the Windows builder. On a computer builder-files has no makerom
for (Linux on ARM, say), build makerom 0.19.0 yourself and add
--makerom PATH.

Keep the builder-files folder next to sm64ds_builder.py.
"""

# romdata.bin pieces upstream's committed source carries as source, not as
# extracted bytes: port/hal/ptr_tables.cpp's kuppa_blob_020889b0 (a table
# copied by hand) and the UnknownVsEntry vtable (code addresses)
ALLOWED = {'data_020889b0', '_ZTV14UnknownVsEntry'}


def rom_pieces(blob, manifest):
    """(name, 64-byte piece) for each table of 64 bytes or more whose start is distinctive."""
    out = []
    for line in manifest.read_text().splitlines():
        if not line.startswith('sym\t'):
            continue
        _, tag, name, off, size, _sha = line.split('\t')
        off, size = int(off), int(size)
        if size < 64:
            continue
        piece = blob[off:off + 64]
        if len(set(piece)) >= 8:
            out.append((name, piece))
    return out


def file_pieces(files_dir, per_file=4, seed=64):
    """64-byte pieces sampled from every extracted game file (distinctive ones)."""
    rnd = random.Random(seed)
    out = []
    for p in sorted(files_dir.rglob('*')):
        if not p.is_file() or p.name == 'files.txt':
            continue
        b = p.read_bytes()
        if len(b) < 256:
            continue
        for _ in range(per_file):
            o = rnd.randrange(0, len(b) - 64)
            piece = b[o:o + 64]
            if len(set(piece)) >= 12:
                out.append((p.relative_to(files_dir).as_posix(), piece))
    return out


def gate(files, pieces):
    hits = []
    for f in files:
        data = f.read_bytes()
        for name, piece in pieces:
            if name not in ALLOWED and data.find(piece) >= 0:
                hits.append(f'{f.name}: {name}')
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--label', required=True, help='the build, shown in the builder (e.g. 1.0.0)')
    ap.add_argument('--build', type=Path, default=ROOT / 'build3ds-wide')
    ap.add_argument('--out', type=Path, default=None)
    a = ap.parse_args()
    out = a.out or a.build / 'SM64DS-Enhanced-Builder.zip'
    assets = UP / 'build' / 'assets'
    work = a.build / 'builder'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir()
    folder = a.build / 'SM64DS-Enhanced-Builder'   # the zip's contents
    if folder.exists():
        shutil.rmtree(folder)
    payload = folder / 'builder-files'
    payload.mkdir(parents=True)

    # one build's three: the elf's baked expectation, the manifest, the recipe
    man = assets / 'romdata.manifest'
    rec = assets / 'romdata.recipe.tsv'
    m = re.match(r'# romdata-manifest v1 ([0-9a-f]{64}) (\d+)', man.read_text().split('\n', 1)[0])
    r = re.match(r'# romdata-recipe v1 ([0-9a-f]{64}) (\d+)', rec.read_text().split('\n', 1)[0])
    assert m and r and m.groups() == r.groups(), 'the manifest and the recipe are not one build\'s'
    blob = (assets / 'romdata.bin').read_bytes()
    assert hashlib.sha256(blob).hexdigest() == m.group(1), 'romdata.bin is not the manifest\'s'
    elf = a.build / 'game3ds.elf'
    assert m.group(1).encode() in elf.read_bytes(), \
        'game3ds.elf expects another romdata.bin (rebuild after tools/pcprobe.sh)'

    subprocess.run([str(DKA / 'arm-none-eabi-strip.exe'), '--strip-debug', '-o',
                    str(payload / 'game3ds.elf'), str(elf)], check=True)
    shutil.copy2(man, payload)
    shutil.copy2(rec, payload)
    # the card build's RSF, but with the code left uncompressed: makerom's code
    # compression was 26 s of a 67 s build, for 3 MB less .cia
    rsf = (ROOT / 'port3ds' / 'cia' / 'game3ds.rsf').read_text()
    rsf, n = re.subn(r'(EnableCompress\s*:\s*)true', r'\g<1>false', rsf)
    assert n == 1, 'game3ds.rsf has no EnableCompress line'
    (payload / 'game3ds.rsf').write_text(rsf)
    for name in ('face.template', 'face.recipe'):   # the 3D banner, without the game's values
        shutil.copy2(HERE / 'face' / name, payload)
    shutil.copy2(TOOLS / 'makerom-0.19.0' / 'makerom.exe', payload)
    bt = TOOLS / 'bannertool-1.2.3' / 'bannertool-1.2.3-windows'
    shutil.copy2(bt / 'bannertool.exe', payload)
    third = (HERE / 'THIRD-PARTY.txt').read_text(encoding='utf-8')
    third += '\n\n==== bannertool: LICENSE.txt ====\n\n' + (bt / 'LICENSE.txt').read_text(encoding='utf-8')
    third += '\n\n==== sm64ds-decomp / port (Tango): LICENSE ====\n\n' + (UP / 'LICENSE').read_text(encoding='utf-8')
    for name in ('makerom', 'mbedtls', 'libyaml', 'gpl-3.0', 'libctru', 'citro3d', 'simde'):   # port3ds/builder/licenses
        third += f'\n\n==== {name} ====\n\n' + (HERE / 'licenses' / f'{name}.txt').read_text(encoding='utf-8')
    (payload / 'THIRD-PARTY.txt').write_text(third, encoding='utf-8')
    (payload / 'LABEL.txt').write_text(a.label + '\n')
    # the sounds: the HOME Menu's, the window's music and a finished build's.
    # They aren't in the public repo; the builder does without them (a silent
    # banner, no music).
    for name in ('banner.wav', 'music.mp3', 'done.mp3'):
        if (HERE / name).is_file():
            shutil.copy2(HERE / name, payload)

    pieces = rom_pieces(blob, man) + file_pieces(UP / 'extracted' / 'dsd' / 'files')
    shipped = sorted(payload.iterdir())
    hits = gate(shipped, pieces)
    print(f'gate: {len(pieces)} pieces of the ROM looked for in {len(shipped)} '
          f'shipped files: {len(hits)} found')
    if hits:
        for h in hits[:20]:
            print('  ', h)
        sys.exit('refused: the builder\'s files carry bytes from the ROM')

    exe = build_exe.build(a.label, folder / 'SM64DS-Enhanced-Builder.exe')

    # the exe and its folder at the zip's top, so Extract All makes one folder of them
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(folder.rglob('*')):
            if f.is_file():
                z.write(f, f.relative_to(folder).as_posix())

    # Linux and macOS: sm64ds_builder.py beside the same builder-files, less
    # the Windows programs and the window's sounds, plus makerom's builds for
    # them and selftest.txt (what the self-test checks those computers against)
    unix = a.build / 'SM64DS-Enhanced-Builder-Linux-Mac'
    if unix.exists():
        shutil.rmtree(unix)
    ufiles = unix / 'builder-files'
    ufiles.mkdir(parents=True)
    for f in payload.iterdir():
        if f.name not in ('makerom.exe', 'bannertool.exe', 'music.mp3', 'done.mp3'):
            shutil.copy2(f, ufiles)
    for plat, sha in MAKEROM_UNIX.items():
        src = TOOLS / 'makerom-0.19.0' / plat / 'makerom'
        assert hashlib.sha256(src.read_bytes()).hexdigest() == sha, f'{src} is not makerom 0.19.0\'s'
        (ufiles / 'makerom' / plat).mkdir(parents=True)
        shutil.copy2(src, ufiles / 'makerom' / plat / 'makerom')
    shutil.copy2(HERE / 'sm64ds_builder.py', unix)
    (unix / 'README.txt').write_text(UNIX_README, encoding='utf-8', newline='\n')
    subprocess.run([sys.executable, '-I', str(unix / 'sm64ds_builder.py'), '--self-test-write'], check=True,
                   stdout=subprocess.DEVNULL)
    uout = out.with_name('SM64DS-Enhanced-Builder-Linux-Mac.zip')
    with zipfile.ZipFile(uout, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(unix.rglob('*')):
            if f.is_file():
                # as a Unix zip, so unzip and the Mac's Archive Utility keep the
                # programs runnable
                info = zipfile.ZipInfo.from_file(f, f.relative_to(unix).as_posix())
                info.create_system = 3
                runnable = f.name in ('makerom', 'sm64ds_builder.py')
                info.external_attr = ((0o100755 if runnable else 0o100644) << 16)
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, f.read_bytes(), compresslevel=9)
    for f in (exe, out, uout):
        print(f'{f}: {f.stat().st_size:,} bytes, sha256 {hashlib.sha256(f.read_bytes()).hexdigest()[:16]}')


if __name__ == '__main__':
    main()
