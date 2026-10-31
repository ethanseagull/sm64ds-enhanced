"""Pack the extracted NitroFS files into one file the 3DS opens once.

THE FILE SELECT -> CASTLE GROUNDS WAIT (hardware: ~9.6 s): the level's boot
opens ~180 of extracted/dsd/files' 2,073 files, and on the 3DS's SD card each
open (+ close) costs ~30 ms whatever the file's size -- 5.3 of the boot's 5.9
s on Azahar, with the reads themselves a few per cent. game/files_pack.cpp
serves every read-only fopen under extracted/dsd/files/ out of this pack
instead, from one handle opened once.

  py -B port3ds/tools/pack_files.py SRC_DIR OUT_FILE

Layout (little-endian): "S3PK", u32 version (1), u32 count, u32 data offset;
then per file u32 offset (from the pack's start), u32 size, u16 path length,
the path (relative to SRC_DIR, '/'-separated, UTF-8); then the files, each
4-byte aligned. OUT_FILE is rewritten only when its bytes would change, so a
copy tool that skips unchanged files (robocopy) skips it too.
"""
import hashlib
import os
from pathlib import Path
import struct
import sys


def build(src):
    # files.txt is dsd's own listing (ntr/fs.cpp's fs_mount_dir, a test's),
    # not a cartridge file: a player's dump extracted by the builder has none,
    # so neither does this pack
    files = sorted(p for p in src.rglob('*') if p.is_file() and p.relative_to(src).as_posix() != 'files.txt')
    rels = [p.relative_to(src).as_posix() for p in files]
    index_len = sum(10 + len(r.encode('utf-8')) for r in rels)
    data_off = (16 + index_len + 3) & ~3
    index, blobs, at = [], [], data_off
    for p, r in zip(files, rels):
        b = p.read_bytes()
        pad = (-len(b)) & 3
        index.append(struct.pack('<IIH', at, len(b), len(r.encode('utf-8'))) + r.encode('utf-8'))
        blobs.append(b + b'\0' * pad)
        at += len(b) + pad
    head = b'S3PK' + struct.pack('<III', 1, len(files), data_off)
    body = head + b''.join(index)
    body += b'\0' * (data_off - len(body))
    return body + b''.join(blobs), len(files)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    data, n = build(src)
    if out.exists() and out.stat().st_size == len(data) and \
            hashlib.sha256(out.read_bytes()).digest() == hashlib.sha256(data).digest():
        print(f'pack unchanged: {n} files, {len(data) // 1024} KB -> {out}')
        return
    tmp = out.with_suffix(out.suffix + '.tmp')
    tmp.write_bytes(data)
    os.replace(tmp, out)
    print(f'packed {n} files, {len(data) // 1024} KB -> {out}')


if __name__ == '__main__':
    main()
