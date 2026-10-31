#!/usr/bin/env python3
"""Write a cartridge's NitroFS files out as extracted/dsd/files/<path>.

That is the layout port/hal/fs.cpp reads (the dsd toolkit's extraction
layout, which the PC port's kit also produces). Every file is copied byte for
byte; compressed files stay compressed, because the game's own loader expects
the "LZ77" tag in front of them.

  extract_files.py <rom.nds> <dest_root>

writes <dest_root>/extracted/dsd/files/...  Requires ndspy.
"""
import pathlib
import sys

import ndspy.rom


def walk(folder, prefix):
    """Yield (path, file_id) for every file under an ndspy Folder."""
    for i, name in enumerate(folder.files):
        yield prefix + name, folder.firstID + i
    for name, sub in folder.folders:
        yield from walk(sub, prefix + name + '/')


def main(rom_path, dest_root):
    rom = ndspy.rom.NintendoDSRom.fromFile(rom_path)
    out = pathlib.Path(dest_root) / 'extracted' / 'dsd' / 'files'
    n = total = 0
    path_of = {}
    for path, fid in walk(rom.filenames, ''):
        data = rom.files[fid]
        p = out / path
        p.parent.mkdir(parents=True, exist_ok=True)
        if not p.exists() or p.stat().st_size != len(data) or p.read_bytes() != data:
            p.write_bytes(data)
        path_of[fid] = path
        n += 1
        total += len(data)
    # files.txt: "<id> <size> <path>" for every FAT entry, the manifest
    # port/ntr/fs.cpp's directory backend mounts. Unnamed entries (the
    # overlays) carry no path.
    with open(out / 'files.txt', 'w', newline='\n') as f:
        for fid, data in enumerate(rom.files):
            f.write('%d %d %s\n' % (fid, len(data), path_of.get(fid, '')))
    print('extract_files: %d files, %.1f MB -> %s' % (n, total / 1048576.0, out))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
