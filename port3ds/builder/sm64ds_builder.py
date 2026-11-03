#!/usr/bin/env python3
"""Super Mario 64 DS Enhanced builder, for Linux and macOS (and Windows).

Makes the installable .cia from your own Super Mario 64 DS cartridge dump,
the same .cia SM64DS-Enhanced-Builder.exe makes. Run it with Python 3.8 or
newer; it needs nothing else but the builder-files folder beside it.

  python3 sm64ds_builder.py                        asks for the dump
  python3 sm64ds_builder.py DUMP.nds [-o OUT.cia]  [--option KEY=VALUE ...]
  python3 sm64ds_builder.py DUMP.nds --language fr the game in French (en, fr, de,
                                                   it or es; English if not given)
  python3 sm64ds_builder.py DUMP.nds OTHER.nds     the Japanese, Korean or American
                                                   English edition, with a dump of
                                                   that cartridge too
  python3 sm64ds_builder.py USA.nds                the American English (or, from the
                                                   Japanese cartridge, Japanese)
                                                   edition from that dump alone

The .cia goes next to the dump unless -o says otherwise. --option sets what
the game starts with: console=new|old (first: it sets that console's 60 fps
and anti-aliasing), 60fps=, anti_aliasing=, fps_counter=, analog_controls=
(1 or 0).

It is Builder.cs, Extract.cs, Face.cs, Korean.cs, Languages.cs, Regions.cs
and Settings.cs in Python, made to give the same files to the bit: the game
data out of the dump (the European ROM, or the American or Japanese one by
builder-files' region kits; hash-checked), the RomFS, the HOME Menu icon from
the DS icon, the 3D banner from the dump's own files, and the icon, sound and
banner formats bannertool writes for the .exe. makerom (builder-files) builds
the .cia.
"""
import argparse
import hashlib
import math
import os
import platform
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time

TITLE = 'Super Mario 64 DS Enhanced'
TITLE_KOREAN = 'Super Mario 64 DS Enhanced (Korean)'
TITLE_KOREAN_HANGUL = '\uc288\ud37c \ub9c8\ub9ac\uc624 64 DS Enhanced'   # in the Korean slot
TITLE_JAPANESE = 'スーパーマリオ64DS Enhanced'   # in the Japanese slot
AUTHOR = 'ethanseagull'
HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.join(HERE, 'builder-files')


class BuildError(Exception):
    pass


def kit(name):
    """A file the build can't do without, from builder-files."""
    p = os.path.join(KIT, name)
    if not os.path.isfile(p):
        if os.path.isfile(os.path.join(KIT, 'LABEL.txt')):
            raise BuildError(f"The builder's {name} is missing from its builder-files folder. "
                             "Download the builder again and extract the whole zip.")
        raise BuildError("The builder's files aren't next to it. Extract the whole zip first, then run "
                         "sm64ds_builder.py from the folder that makes, with its builder-files folder beside it.")
    return p


def label():
    try:
        with open(os.path.join(KIT, 'LABEL.txt'), encoding='utf-8') as f:
            return 'build ' + f.read().strip()
    except OSError:
        return 'build unknown'


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def s32(b, o):
    return struct.unpack_from('<i', b, o)[0]


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def s16(b, o):
    return struct.unpack_from('<h', b, o)[0]


def sha256(b):
    return hashlib.sha256(b).hexdigest()


# --- the settings (Settings.cs BuildOptions) --------------------------------

class Options:
    def __init__(self):
        self.old = False
        self.fps60 = self.aa = self.analog = True
        self.counter = False

    def set(self, line):
        """One KEY=VALUE line; False if it isn't one of options.txt's."""
        if '=' not in line:
            return False
        key, value = line.split('=', 1)
        key, value = key.strip(), value.strip().lower()
        on = value in ('1', 'on', 'true')
        if key == 'console':
            if value not in ('old', 'new'):
                return False
            self.old = value == 'old'
            self.fps60 = self.aa = not self.old
        elif key == '60fps':
            self.fps60 = on
        elif key == 'anti_aliasing':
            self.aa = on
        elif key == 'fps_counter':
            self.counter = on
        elif key == 'analog_controls':
            self.analog = on
        else:
            return False
        return True

    def file_text(self):
        bit = lambda on: '1' if on else '0'
        return ("# The builder's settings, read at boot until the options menu (SELECT)\n"
                "# saves sdmc:/sm64ds3ds/options.txt.\n"
                f"console={'old' if self.old else 'new'}\n"
                f"60fps={bit(self.fps60)}\n"
                f"anti_aliasing={bit(self.aa)}\n"
                f"fps_counter={bit(self.counter)}\n"
                f"analog_controls={bit(self.analog)}\n")

    def summary(self):
        o = lambda on: 'on' if on else 'off'
        return (f"{'Old 3DS' if self.old else 'New 3DS'}; 60 fps {o(self.fps60)}, anti-aliasing {o(self.aa)}, "
                f"FPS counter {o(self.counter)}, analog controls {o(self.analog)}")


# --- the game data out of the dump (Extract.cs) -----------------------------

HANDLE_TABLE_ADDRESS = 0x020BD4B8
HANDLE_COUNT = 0x80A
KINDS = {'.bca': 'animation', '.bmd': 'model', '.btp': 'texture-sequence', '.kcl': 'collision',
         '.narc': 'archive', '.sdat': 'sound-archive', '.bin': 'data'}


def kind(path):
    return KINDS.get(os.path.splitext(path)[1].lower(), 'file')


def expand_blz(data):
    """The overlays' and the program's back-to-front LZ: both streams run
    backwards from the end; anything before the compressed run is plain."""
    try:
        return _expand_blz(data)
    except IndexError:
        raise ValueError('it runs past its own end')


def _expand_blz(data):
    if len(data) < 8:
        return data
    footer, extra = struct.unpack_from('<II', data, len(data) - 8)
    if extra == 0:
        return data
    header_len, compressed_len = footer >> 24, footer & 0xFFFFFF
    if header_len < 8 or header_len > len(data) or compressed_len > len(data):
        raise ValueError('an unreadable compression header')
    passthrough = len(data) - compressed_len
    comp_len = compressed_len - header_len
    out_len = len(data) + extra - passthrough
    out = bytearray(out_len)
    read = done = flags = 0
    mask = 1
    base = passthrough + comp_len - 1
    def at(i):   # a negative index would wrap round to the end in Python; C# stops
        if i < 0:
            raise IndexError
        return data[i]
    while done < out_len:
        if mask == 1:
            if read >= compressed_len:
                raise ValueError('it ended mid-stream')
            flags = at(base - read)
            read += 1
            mask = 0x80
        else:
            mask >>= 1
        if flags & mask:
            b1, b2 = at(base - read), at(base - read - 1)
            read += 2
            length = (b1 >> 4) + 3
            disp = (((b1 & 0x0F) << 8) | b2) + 3
            if disp > done:
                if done < 2:
                    raise ValueError('it refers back to data not there yet')
                disp = 2
            src = done - disp
            for _ in range(length):
                out[out_len - 1 - done] = out[out_len - 1 - src]
                src += 1
                done += 1
        else:
            out[out_len - 1 - done] = at(base - read)
            read += 1
            done += 1
    return bytes(data[:passthrough]) + bytes(out)


def extract(rom_bytes, recipe_path, manifest_path, log, progress):
    """(files {path: bytes}, assets {name: bytes}) out of the dump, as
    Extract.Run writes them; raises BuildError with the player's reason."""
    rb = rom_bytes
    if len(rb) < 0x4000:
        raise BuildError(f'That file is only {len(rb)} bytes, too small to be a DS cartridge dump.')
    title = rb[0:12].decode('ascii', 'replace').rstrip('\0')
    code = rb[0x0C:0x10].decode('ascii', 'replace')
    if title != 'S.MARIO64DS' or not code.startswith('ASM'):
        show = lambda s: re.sub('[^ -~]', '.', s)
        raise BuildError(f"That is not a Super Mario 64 DS dump (it says title '{show(title)}', game code "
                         f"'{show(code)}'). Use a dump of a Super Mario 64 DS cartridge.")
    if code != 'ASMP':
        raise BuildError(f'This is not the European ROM (game code {code}). This port is built from the '
                         'European ROM (ASMP), so its data will not come out of another region\'s cartridge.')
    fnt_off, fnt_size, fat_off, fat_size, ovt_off, ovt_size, ovt7_off, ovt7_size = struct.unpack_from('<8I', rb, 0x40)
    used_size = u32(rb, 0x80)
    for what, off, size in (('file name table', fnt_off, fnt_size), ('file allocation table', fat_off, fat_size),
                            ('overlay table', ovt_off, ovt_size)):
        if off == 0 or off + size > len(rb):
            raise BuildError(f'This dump is truncated: its {what} runs past the end of the file. Re-dump the cartridge.')
    if not (ovt7_off == 0 and ovt7_size == 0) and (ovt7_off == 0 or ovt7_size == 0 or ovt7_off + ovt7_size > len(rb)):
        raise BuildError('This dump is truncated: its ARM7 overlay table runs past the end of the file. '
                         'Re-dump the cartridge.')
    if used_size > len(rb):
        raise BuildError(f'This dump is incomplete: the header says the cartridge holds {used_size} bytes but the '
                         f'file is only {len(rb)}. Re-dump the cartridge.')
    log(f'Super Mario 64 DS ({code}, Europe), {round(len(rb) / 1048576)} MB')
    progress(2, 'Reading the file tables')

    fat_count = fat_size // 8
    fat = [struct.unpack_from('<II', rb, fat_off + i * 8) for i in range(fat_count)]

    damaged = "This dump's file name table is damaged. Re-dump the cartridge."
    paths = {}
    queue = [(0, '')]
    visited = {0}
    while queue:
        dir_id, base = queue.pop(0)
        entry = fnt_off + dir_id * 8
        if entry + 8 > len(rb):
            raise BuildError(damaged)
        o = fnt_off + u32(rb, entry)
        file_id = u16(rb, entry + 4)
        while True:
            if o >= len(rb):
                raise BuildError(damaged)
            typ = rb[o]
            o += 1
            if typ == 0:
                break
            n = typ & 0x7F
            if o + n + (2 if typ & 0x80 else 0) > len(rb):
                raise BuildError(damaged)
            name = rb[o:o + n].decode('ascii', 'replace')
            o += n
            full = f'{base}/{name}' if base else name
            if typ & 0x80:
                sub = u16(rb, o) & 0x0FFF
                o += 2
                if sub not in visited:
                    visited.add(sub)
                    queue.append((sub, full))
            else:
                paths[file_id] = full
                file_id += 1
    if not paths:
        raise BuildError('This dump has no named files in it. Re-dump the cartridge.')
    ids = sorted(paths)
    for i in ids:
        if i < 0 or i >= fat_count:
            raise BuildError(f'This dump is damaged: its file name table names a file ({paths[i]}) that its file '
                             'allocation table does not have. Re-dump the cartridge.')
        start, end = fat[i]
        if end < start or end > len(rb):
            raise BuildError(f"This dump is damaged: its file table points at data past the end of the file "
                             f"(file {i}, '{paths[i]}'). Re-dump the cartridge.")
    files = {}
    for n, i in enumerate(ids, 1):
        start, end = fat[i]
        files[paths[i]] = rb[start:end]
        if n % 500 == 0:
            progress(2 + 68 * n // len(ids), f'Unpacking file {n} of {len(ids)}')
    log(f'    {len(ids)} files, {round(sum(len(b) for b in files.values()) / 1048576, 1)} MB')
    progress(70, f'Unpacked {len(ids)} files')

    # overlay 0's asset-handle table
    if ovt_size < 32:
        raise BuildError('This dump has no ARM9 overlays. Re-dump the cartridge.')
    ov0_ram, ov0_file, ov0_word = u32(rb, ovt_off + 4), u32(rb, ovt_off + 24), u32(rb, ovt_off + 28)
    if ov0_file >= fat_count:
        raise BuildError('This dump is damaged: its overlay table points at a file its file allocation table '
                         'does not have. Re-dump the cartridge.')
    ov0 = rb[fat[ov0_file][0]:fat[ov0_file][1]]
    progress(71, 'Decompressing overlay 0')
    if (ov0_word >> 24) & 1:
        try:
            ov0 = expand_blz(ov0)
        except ValueError as e:
            raise BuildError(f'Could not read overlay 0 of this dump ({e}). Re-dump the cartridge.')
    table = HANDLE_TABLE_ADDRESS - ov0_ram
    if table < 0 or table + HANDLE_COUNT * 4 > len(ov0):
        raise BuildError('This dump is Super Mario 64 DS but not a revision this build knows: its overlay 0 has '
                         'no asset-handle table where one is expected.')
    id_by_path = {}   # the script's hashtable: case-insensitive, the last one kept
    for i in ids:
        id_by_path[paths[i].lower()] = i
    progress(72, 'Reading the asset-handle table')
    handle_rows = ['handle\thex_handle\tfile_id\thex_file_id\tpath\tkind\tsize\n']
    for h in range(HANDLE_COUNT):
        so = u32(ov0, table + h * 4) - ov0_ram
        if so < 0 or so >= len(ov0):
            raise BuildError('This dump is Super Mario 64 DS but not a revision this build knows: asset handle '
                             f'{h} points outside overlay 0.')
        end = ov0.find(b'\0', so)
        if end < 0:
            end = len(ov0)
        asset = ov0[so:end].decode('ascii', 'replace')
        if asset.lower() not in id_by_path:
            raise BuildError('This dump is Super Mario 64 DS but not a revision this build knows: asset handle '
                             f"{h} names '{asset}', which its filesystem does not have.")
        i = id_by_path[asset.lower()]
        handle_rows.append(f'{h}\t0x{h:04x}\t{i}\t0x{i:04x}\t{asset}\t{kind(asset)}\t{fat[i][1] - fat[i][0]}\n')
    file_rows = ['file_id\thex_id\tpath\tkind\tsize\n']
    for i in ids:
        file_rows.append(f'{i}\t0x{i:04x}\t{paths[i]}\t{kind(paths[i])}\t{fat[i][1] - fat[i][0]}\n')
    assets = {'files.tsv': ''.join(file_rows).encode(), 'handles.tsv': ''.join(handle_rows).encode()}
    log(f'    {len(ids)} files and {HANDLE_COUNT} handles catalogued')

    # the NitroFS tables, the card's own bytes
    if fnt_size == 0 or fat_size == 0:
        raise BuildError('This dump has an empty file table, so the game could not look up any of its files. '
                         'Re-dump the cartridge.')
    assets['nitrofs_fnt.bin'] = rb[fnt_off:fnt_off + fnt_size]
    assets['nitrofs_fat.bin'] = rb[fat_off:fat_off + fat_size]
    if ovt_size:
        assets['nitrofs_ovt9.bin'] = rb[ovt_off:ovt_off + ovt_size]
    assets['nitrofs.tsv'] = (f'key\tvalue\nfnt_offset\t{fnt_off}\nfnt_size\t{fnt_size}\nfat_offset\t{fat_off}\n'
                             f'fat_size\t{fat_size}\novt9_offset\t{ovt_off}\novt9_size\t{ovt_size}\n'
                             f'ovt7_offset\t{ovt7_off}\novt7_size\t{ovt7_size}\n').encode()
    progress(80, 'Rebuilding romdata.bin')

    # romdata.bin: the code-side tables, from the recipe, hash-checked
    damaged_recipe = ("This builder's romdata.recipe.tsv is damaged, so the game data cannot be rebuilt. "
                      'Download the builder again.')
    with open(recipe_path, encoding='utf-8') as f:
        recipe = f.read().splitlines()
    head = re.match(r'^# romdata-recipe v1 ([0-9a-f]{64}) (\d+)$', recipe[0]) if len(recipe) >= 2 else None
    if not head:
        raise BuildError(damaged_recipe)
    want_sha, total = head.group(1), int(head.group(2))
    rows, needed = [], []
    for line in recipe:
        if line.startswith('#') or not line.strip():
            continue
        f = line.split('\t')
        if len(f) < 4:
            raise BuildError(damaged_recipe)
        rows.append(f)
        if f[2] not in needed:
            needed.append(f[2])
    ov_by_name = {}
    for i in range(ovt_size // 32):
        e = ovt_off + i * 32
        ov_by_name[f'ov{u32(rb, e):03d}'] = (u32(rb, e + 24), u32(rb, e + 28) >> 24)
    images = {}
    for n, src in enumerate(needed):
        progress(80 + 17 * n // len(needed), f'Decompressing {src} ({n + 1} of {len(needed)})')
        if src == 'arm9':
            a9 = u32(rb, 0x20)
            raw, packed = rb[a9:a9 + u32(rb, 0x2C)], True
        else:
            if src.lower() not in ov_by_name:
                raise BuildError(f"This dump is Super Mario 64 DS but not a revision this build knows: it has no "
                                 f"overlay '{src}'.")
            fid, fl = ov_by_name[src.lower()]
            if fid >= fat_count:
                raise BuildError(f"This dump is damaged: its overlay table points at a file its file allocation "
                                 f"table does not have (overlay '{src}'). Re-dump the cartridge.")
            start, end = fat[fid]
            if end < start or end > len(rb):
                raise BuildError(f"This dump is damaged: overlay '{src}' runs past the end of the file. "
                                 "Re-dump the cartridge.")
            raw, packed = rb[start:end], bool(fl & 1)
        if packed:
            try:
                raw = expand_blz(raw)
            except ValueError as e:
                raise BuildError(f"Could not read this dump's program data ({e}). Re-dump the cartridge."
                                 if src == 'arm9' else
                                 f"Could not read overlay '{src}' of this dump ({e}). Re-dump the cartridge.")
        images[src.lower()] = raw
    progress(98, 'Assembling and checking romdata.bin')
    blob = bytearray(total)
    for f in rows:
        try:
            off, size, src_off = int(f[0]), int(f[1]), int(f[3])
        except ValueError:
            raise BuildError(damaged_recipe)
        img = images.get(f[2].lower())
        if img is None or off < 0 or size < 0 or src_off < 0 or off + size > total:
            raise BuildError(damaged_recipe)
        have = min(size, max(0, len(img) - src_off))
        if have > 0:
            blob[off:off + have] = img[src_off:src_off + have]
    if sha256(blob) != want_sha:
        raise BuildError(f'This is a European dump (game code {code}), but its contents do not match the cartridge '
                         'this build was made from. That usually means the file has been modified (a patch, a '
                         'translation or a trimmed dump) or the dump is faulty. Use an untouched dump of your own '
                         'retail cartridge, then try again.')
    assets['romdata.bin'] = bytes(blob)
    log(f'    romdata.bin verified ({total} bytes, checksum OK)')
    with open(manifest_path, 'rb') as f:
        assets['romdata.manifest'] = f.read()
    progress(100, 'Done')
    return files, assets


# --- the language editions (Korean.cs, Languages.cs) -------------------------
# The game is the European ROM's code, which has five languages (English,
# French, German, Italian and Spanish) and picks one by the DS owner's
# language. The 3DS has none, so the game is English unless the .cia says
# otherwise: --language picks the one it runs in. The Japanese, Korean and
# American English editions also take a second dump, of that cartridge (ASMJ,
# ASMK, ASME), for its text and the pictures with words in them, which go in
# the European files of the language slot the game then runs as.
# build/assets/langpack.bin tells the game (game/language.cpp reads it):
#
#   0x00  "SM64DS-LANGPACK\0", u32 version (1), u32 size
#   0x18  the language's code, 8 bytes ("ja", "en-us", ...)
#   0x20  u8 the DS language the game runs as (1 English, 2 French, 3 German,
#         4 Italian, 5 Spanish), u8 flags (1: the font widths follow)
#   0x40  the font's widths (256), the big font's widths (256)

LANGUAGES = {   # code: (name, the DS language the game runs as, the second dump it takes)
    'en': ('English', 1, None), 'fr': ('French', 2, None), 'de': ('German', 3, None),
    'it': ('Italian', 4, None), 'es': ('Spanish', 5, None),
    'ja': ('Japanese', 1, 'ASMJ'), 'ko': ('Korean', 5, 'ASMK'), 'en-us': ('American English', 1, 'ASME'),
}
DUMP_LANGUAGE = {v[2]: k for k, v in LANGUAGES.items() if v[2]}   # a second dump's game code: its language
REGION = {'ASMJ': 'Japanese', 'ASMK': 'Korean', 'ASME': 'American'}


def edition_title(language):
    """The .cia's title (and file name) for a language."""
    if language in (None, 'en'):
        return TITLE
    if language == 'ko':
        return TITLE_KOREAN
    return f'{TITLE} ({LANGUAGES[language][0]})'


def edition_slots(language):
    """The HOME Menu title in the language's own slot, where it has its own."""
    if language == 'ko':
        return {7: (TITLE_KOREAN_HANGUL, TITLE_KOREAN_HANGUL)}
    if language == 'ja':
        return {0: (TITLE_JAPANESE, TITLE_JAPANESE)}
    return None


def langpack(language, widths=b''):
    """build/assets/langpack.bin (above)."""
    return (b'SM64DS-LANGPACK\0' + struct.pack('<II', 1, 0x40 + len(widths)) +
            language.encode('ascii').ljust(8, b'\0') +
            struct.pack('<BB', LANGUAGES[language][1], 1 if widths else 0) + bytes(0x1E) + widths)


def nitro_files(rb, want, who):
    """{path: bytes} of the NitroFS files named in want (case-blind paths)."""
    fnt_off, fnt_size, fat_off, fat_size = struct.unpack_from('<4I', rb, 0x40)
    if fnt_off == 0 or fnt_off + fnt_size > len(rb) or fat_off == 0 or fat_off + fat_size > len(rb):
        raise BuildError(f'The {who} dump is truncated: its file tables run past the end of the file. '
                         'Re-dump the cartridge.')
    damaged = f"The {who} dump's file name table is damaged. Re-dump the cartridge."
    wanted = {p.lower(): p for p in want}
    out = {}
    queue, visited = [(0, '')], {0}
    while queue:
        dir_id, base = queue.pop(0)
        entry = fnt_off + dir_id * 8
        if entry + 8 > len(rb):
            raise BuildError(damaged)
        o = fnt_off + u32(rb, entry)
        file_id = u16(rb, entry + 4)
        while True:
            if o >= len(rb):
                raise BuildError(damaged)
            typ = rb[o]
            o += 1
            if typ == 0:
                break
            n = typ & 0x7F
            if o + n + (2 if typ & 0x80 else 0) > len(rb):
                raise BuildError(damaged)
            full = (base + '/' if base else '') + rb[o:o + n].decode('ascii', 'replace')
            o += n
            if typ & 0x80:
                sub = u16(rb, o) & 0x0FFF
                o += 2
                if sub not in visited:
                    visited.add(sub)
                    queue.append((sub, full))
            else:
                if full.lower() in wanted and file_id < fat_size // 8:
                    start, end = struct.unpack_from('<II', rb, fat_off + file_id * 8)
                    if start <= end <= len(rb):
                        out[wanted[full.lower()]] = rb[start:end]
                file_id += 1
    return out


def files_sha(files, paths):
    """One hash over the files a language takes, in paths' order."""
    h = hashlib.sha256()
    for p in paths:
        h.update(p.encode('ascii') + b'\0' + hashlib.sha256(files[p]).digest())
    return h.hexdigest()


def second_dump(rb, language):
    """Checks the second dump is the cartridge the language takes; its arm9."""
    code, who = LANGUAGES[language][2], REGION[LANGUAGES[language][2]]
    if len(rb) < 0x4000:
        raise BuildError(f'The {who} dump is only {len(rb)} bytes, too small to be a DS cartridge dump.')
    have = rb[0x0C:0x10].decode('ascii', 'replace')
    if rb[0:12].rstrip(b'\0') != b'S.MARIO64DS' or have != code:
        raise BuildError(f"The second dump is not the {who} Super Mario 64 DS (game code "
                         f"{re.sub('[^ -~]', '.', have)}, not {code}).")
    a9 = u32(rb, 0x20)
    try:
        return expand_blz(rb[a9:a9 + u32(rb, 0x2C)])
    except ValueError as e:
        raise BuildError(f"Could not read the {who} dump's program data ({e}). Re-dump the cartridge.")


def not_that_cartridge(language):
    code, who = LANGUAGES[language][2], REGION[LANGUAGES[language][2]]
    return BuildError(f'This is a {who} dump (game code {code}), but its contents do not match the cartridge this '
                      'build was made from. That usually means the file has been modified (a patch or a trimmed '
                      'dump), the dump is faulty, or it is another revision of the cartridge. Use an untouched '
                      'dump of your own retail cartridge, then try again.')


def replace_files(files, assets, new, who):
    """new {path: bytes} over the European files, and the file tables to match.
    A file that grew moves past the end of the cartridge's data (the game's card
    serves whatever the allocation table points at); one that didn't stays.
    Returns how many entries of the file table changed."""
    rows = assets['files.tsv'].decode().splitlines(True)
    id_by_path = {}
    for line in rows[1:]:
        f = line.rstrip('\n').split('\t')
        id_by_path[f[2].lower()] = int(f[0])
    fat = bytearray(assets['nitrofs_fat.bin'])
    top = max(u32(fat, i * 8 + 4) for i in range(len(fat) // 8))
    at = (top + 0x1FF) & ~0x1FF
    size_by_id = {}
    for p in sorted(new, key=lambda p: id_by_path[p.lower()]):
        i = id_by_path[p.lower()]
        start, end = struct.unpack_from('<II', fat, i * 8)
        size = len(new[p])
        if size > end - start:
            start, at = at, (at + size + 0x1FF) & ~0x1FF
        struct.pack_into('<II', fat, i * 8, start, start + size)
        size_by_id[i] = size
        files[p] = new[p]
    if at > 0x8000000:   # the game's card reads any 32-bit address; mods' range starts at 0x10000000
        raise BuildError(f"The {who} files don't fit the cartridge's address space.")
    assets['nitrofs_fat.bin'] = bytes(fat)

    def resize(name, id_col):
        lines = assets[name].decode().splitlines(True)
        out = [lines[0]]
        for line in lines[1:]:
            f = line.rstrip('\n').split('\t')
            i = int(f[id_col])
            if i in size_by_id:
                f[-1] = str(size_by_id[i])
            out.append('\t'.join(f) + '\n')
        assets[name] = ''.join(out).encode()
    resize('files.tsv', 0)
    resize('handles.tsv', 2)
    return len(size_by_id)


def lz77_wrap(raw):
    """The game's LZ77 file around raw bytes, as literal runs."""
    out = bytearray(b'LZ77' + bytes((0x10, len(raw) & 0xFF, len(raw) >> 8 & 0xFF, len(raw) >> 16 & 0xFF)))
    for i in range(0, len(raw), 8):
        out.append(0)
        out += raw[i:i + 8]
    return bytes(out)


def bmd_find(bmd, table, count, stride, name, size):
    """The data offset of the BMD texture (table 0x18) or palette (0x20) called
    name, which must be size bytes (None: any; then (offset, size))."""
    for i in range(u32(bmd, count)):
        e = u32(bmd, table) + i * stride
        n = u32(bmd, e)
        if bmd[n:bmd.index(b'\0', n)] == name.encode('ascii'):
            if size is None:
                return u32(bmd, e + 4), u32(bmd, e + 8)
            if u32(bmd, e + 8) != size:
                break
            return u32(bmd, e + 4)
    raise BuildError("The title screen's textures aren't the ones this build knows. Re-dump the cartridge.")


def narc_members(narc):
    """({member path: index}, {section magic: offset}) of a NARC archive."""
    bad = BuildError("One of the game's archives isn't one this build can read. Re-dump the cartridge.")
    if narc[:4] != b'NARC' or len(narc) < 16:
        raise bad
    sections, at = {}, u16(narc, 0x0C)
    for _ in range(u16(narc, 0x0E)):
        if at + 8 > len(narc):
            raise bad
        sections[narc[at:at + 4]] = at
        at += u32(narc, at + 4)
    if b'BTAF' not in sections or b'BTNF' not in sections or b'GMIF' not in sections:
        raise bad
    fnt = sections[b'BTNF'] + 8
    names, queue = {}, [(0, '')]
    while queue:
        d, base = queue.pop(0)
        o, fid = fnt + u32(narc, fnt + d * 8), u16(narc, fnt + d * 8 + 4)
        while True:
            typ = narc[o]
            o += 1
            if typ == 0:
                break
            full = (base + '/' if base else '') + narc[o:o + (typ & 0x7F)].decode('ascii', 'replace')
            o += typ & 0x7F
            if typ & 0x80:
                queue.append((u16(narc, o) & 0x0FFF, full))
                o += 2
            else:
                names[full] = fid
                fid += 1
    return names, sections


def narc_files(narc):
    """[bytes] of each member of a NARC, by index."""
    _, s = narc_members(narc)
    fat, img = s[b'BTAF'], s[b'GMIF'] + 8
    return [narc[img + u32(narc, fat + 12 + i * 8):img + u32(narc, fat + 16 + i * 8)]
            for i in range(u16(narc, fat + 8))]


def narc_get(narc, name):
    names, _ = narc_members(narc)
    if name not in names:
        raise BuildError(f"An archive has no {name}. Re-dump the cartridge.")
    return narc_files(narc)[names[name]]


def narc_replace(narc, new):
    """The NARC with the members in new {path: bytes} replaced: the names kept,
    the data laid out again (each member 4-byte aligned)."""
    names, s = narc_members(narc)
    data = narc_files(narc)
    for name, b in new.items():
        data[names[name]] = b
    btaf, gmif = bytearray(b'BTAF' + struct.pack('<IHH', 12 + 8 * len(data), len(data), 0)), bytearray()
    for b in data:
        btaf += struct.pack('<II', len(gmif), len(gmif) + len(b))
        gmif += b + b'\xff' * (-len(b) & 3)
    btnf = narc[s[b'BTNF']:s[b'BTNF'] + u32(narc, s[b'BTNF'] + 4)]
    body = bytes(btaf) + btnf + b'GMIF' + struct.pack('<I', 8 + len(gmif)) + bytes(gmif)
    return narc[:8] + struct.pack('<I', 16 + len(body)) + narc[12:16] + body


# --- the Korean edition (Korean.cs) -----------------------------------------
# From the Korean cartridge (ASMK): its message bank, the graphics it changed
# (the Korean art is in the Spanish slot, which the game then uses), and
# korean.bin, its arm9's font and text tables (game/korean.cpp reads it).

KOREAN_FILES = (
    'ARCHIVE/c2d.narc', 'ARCHIVE/ces.narc', 'ARCHIVE/en1.narc',
    'data/2D_cad/EUR/SPA/d_2d_cmn_icon_S_ncg.bin', 'data/2D_cad/EUR/SPA/d_2d_staff_roll_cake_8bit_S_ncg.bin',
    'data/2D_cad/EUR/d_2d_scenario_player_select_S_ncg.bin',
    'data/2D_cad/EUR/d_2d_scenario_player_select_course_number_BG_S_ncg.bin',
    'data/2D_cad/EUR/d_2d_staff_roll_end_EUR_ncg.bin', 'data/2D_cad/d_2d_game_over_ncg.bin',
    'data/2D_cad/d_2d_mario_3Dfont_ncl.bin', 'data/2D_cad/d_2d_scenario_player_select_course_number_BG_nsc.bin',
    'data/2D_cad/d_2d_upr_touch_to_select_nsc.bin',
    'data/DSMT/EUR/file_menu_S_ncg.bin', 'data/DSMT/EUR/title_demo_S_ncg.bin',
    'data/DSMT/EUR/texBank_E.bmd',
    'data/message/msg_data_hn.bin',
)
# every language's bank becomes the Korean one
KOREAN_BANK = 'data/message/msg_data_hn.bin'
BANK_SLOTS = ('data/message/msg_data_eng.bin', 'data/message/msg_data_frn.bin',
              'data/message/msg_data_gmn.bin', 'data/message/msg_data_itl.bin',
              'data/message/msg_data_spn.bin')
# korean.bin's parts out of the Korean arm9: (address, size, padded size, sha256)
KOREAN_ARM9 = (
    (0x020891E8, 0x6C40, 0x6C40, 'd00ef99b3d932d96a9028a918ec1743228f9d8c58ab723989477a2615b72308d'),   # font
    (0x020900FC, 0x100, 0x100, '432081527a8d6515bcb8c241a291adc3a0b5361450d7041b027921d8e55eb291'),     # widths
    (0x020901FC, 0x100, 0x100, '6dc062862651f497461da6abd52887a1baa2cf20f2fb2400425879ae47b211a3'),     # big font's
    (0x0208FEEC, 0x68, 0x68, '41260aa68d9300ee3b58696a3df4b85b93912d1e48be209cf05bca475a063569'),       # its codes
    (0x0208FE64, 7, 8, 'd86891d09c437337e5582db342a88d38e598ad47ee4ff43a0527868fd0175130'),             # icons
    (0x0208FE78, 0x14, 0x14, '63027dc0325e47496c74e01cfea8e4bed36aabe0acd59074dd5d3e8722fc09aa'),       # digits
    (0x0208FF54, 0x1A8, 0x1A8, '92cf5aafe424f7ec58490a1d92246f61c4832cc1d769a4a48fa2731372ce95c5'),     # message ids
)
KOREAN_FILES_SHA = '040ed1bab9155b35457bc54335e1816943623f41635b9101b630811e1eac1599'   # files_sha of the retail cartridge


def korean_extract(kb, log):
    """{'files': {path: bytes}, 'korean.bin': bytes} out of the Korean dump."""
    arm9 = second_dump(kb, 'ko')
    log(f'Super Mario 64 DS (ASMK, Korea), {round(len(kb) / 1048576)} MB')
    files = nitro_files(kb, KOREAN_FILES, 'Korean')
    missing = [p for p in KOREAN_FILES if p not in files]
    if missing:
        raise BuildError(f"The Korean dump has no {missing[0]}. Re-dump the cartridge.")
    body = bytearray()
    for addr, size, padded, want in KOREAN_ARM9:
        o = addr - 0x02004000
        part = arm9[o:o + size]
        if len(part) != size or sha256(part) != want or files_sha(files, KOREAN_FILES) != KOREAN_FILES_SHA:
            raise not_that_cartridge('ko')
        body += part + bytes(padded - size)
    head = b'SM64DS-KOREAN' + bytes(3) + struct.pack('<II', 1, 0x20 + len(body)) + bytes(8)
    log(f'    the Korean text: {len(files)} files and the font, checksums OK')
    return {'files': files, 'korean.bin': head + bytes(body)}


KOREAN_TEXBANK = 'data/DSMT/EUR/texBank_E.bmd'      # the Korean cartridge's (all its menu art)
KOREAN_TEXBANK_S = 'data/DSMT/EUR/texBank_S.bmd'


def nearest(c, choices, pal_):
    return min(choices, key=lambda j: sum((a - b) ** 2 for a, b in zip(c, pal_[j])))


def bmd_colours(b, at, n):
    return [(c & 31, c >> 5 & 31, c >> 10 & 31) for c in (u16(b, at + i * 2) for i in range(n))]


def korean_texbanks(eu_e, eu_s, ko):
    """The title menu's two texture banks (texBank_E, texBank_S) for the Korean
    edition: the European banks, which the game's menu is made for, with the
    Korean cartridge's pictures in them. The menu runs in the Spanish slot as the
    Korean one does, so texBank_S's file and title menus are the ones shown; the
    Korean title menu has no Rec Room and its wrench and arrows elsewhere, so it
    is the European picture's VS, wrench and arrows in the Korean palette's
    nearest colours, the Korean ADVENTURE (128 wide, squeezed to the European
    button's 120), and nothing where the Rec Room was."""
    e, s, k = bytearray(lz77(eu_e)), bytearray(lz77(eu_s)), lz77(ko)
    tex = lambda b, n, size: bmd_find(b, 0x18, 0x14, 20, n, size)
    pal = lambda b, n, size: bmd_find(b, 0x20, 0x1C, 16, n, size)

    # the file menu: the same layout in both
    fm, fmp = tex(k, 'file_menu_K', 0x10000), pal(k, 'file_menu_F_pl', 0x200)
    for b, t, p in ((e, 'file_menu_U', 'file_menu_U_pl'), (s, 'file_menu_S', 'file_menu_S_pl')):
        at, pat = tex(b, t, 0x10000), pal(b, p, 0x200)
        b[at:at + 0x10000] = k[fm:fm + 0x10000]
        b[pat:pat + 0x200] = k[fmp:fmp + 0x200]

    # the title menu
    et, ep = tex(e, 'title_menu_E', 0x4000), pal(e, 'title_menu_E_pl', 0x200)
    kt, kp = tex(k, 'title_menu_K', 0x4000), pal(k, 'title_menu_K_pl', 0x200)
    epal, kpal = bmd_colours(e, ep, 256), bmd_colours(k, kp, 256)
    near = [0] + [nearest(epal[i], range(1, 256), kpal) for i in range(1, 256)]
    menu = bytearray(near[v] for v in e[et:et + 0x4000])
    for y in range(80, 120):            # the Rec Room
        menu[y * 128:y * 128 + 64] = bytes(64)
    for y in range(40, 80):             # ADVENTURE
        for x in range(120):
            menu[y * 128 + 4 + x] = k[kt + y * 128 + x * 128 // 120]
    for b, t, p in ((e, 'title_menu_E', 'title_menu_E_pl'), (s, 'title_menu_S', 'title_menu_S_pl')):
        at, pat = tex(b, t, 0x4000), pal(b, p, 0x200)
        b[at:at + 0x4000] = menu
        b[pat:pat + 0x200] = k[kp:kp + 0x200]

    # the options screen: the same layout, but the European part loads 200
    # colours, so the Korean colours past 200 become their nearest below
    ot, op = tex(k, 'title_option', 0x4000), pal(k, 'title_option', 0x200)
    opal = bmd_colours(k, op, 256)
    below = list(range(200)) + [nearest(opal[i], range(1, 200), opal) for i in range(200, 256)]
    at, pat = tex(e, 'title_option_EU', 0x4000), pal(e, 'title_option_pl', 0x190)
    e[at:at + 0x4000] = bytes(below[v] for v in k[ot:ot + 0x4000])
    e[pat:pat + 0x190] = k[op:op + 0x190]
    # and its OK button, which the Spanish slot takes from title_opt_ok (two
    # buttons, 80 and 96 wide): the Korean OK (64x32 at 192, 32) in each
    at, pat = tex(e, 'title_opt_ok', 0x2000), pal(e, 'title_opt_ok', 0x200)
    ok = bytearray(0x2000)
    for y in range(32):
        row = k[ot + (32 + y) * 256 + 192:ot + (32 + y) * 256 + 256]
        ok[y * 256 + 8:y * 256 + 72] = row
        ok[y * 256 + 96:y * 256 + 160] = row
    e[at:at + 0x2000] = ok
    e[pat:pat + 0x200] = k[op:op + 0x200]
    return lz77_wrap(bytes(e)), lz77_wrap(bytes(s))


def korean_apply(files, assets, ko, log):
    """The Korean files over the European ones."""
    new = {p: ko['files'][p] for p in KOREAN_FILES if p != KOREAN_BANK}
    for p in BANK_SLOTS:
        new[p] = ko['files'][KOREAN_BANK]
    del new[KOREAN_TEXBANK]
    new[KOREAN_TEXBANK], new[KOREAN_TEXBANK_S] = korean_texbanks(files[KOREAN_TEXBANK], files[KOREAN_TEXBANK_S],
                                                                 ko['files'][KOREAN_TEXBANK])
    changed = replace_files(files, assets, new, 'Korean')
    assets['korean.bin'] = ko['korean.bin']
    assets['langpack.bin'] = langpack('ko')
    log(f'    {len(new)} files Korean, {changed} entries in the file table updated')


# --- the Japanese and American editions (Languages.cs) ----------------------
# The American cartridge (ASME) and the Japanese one (ASMJ) are one game: both
# have the English and the Japanese text, and each its own pictures. Their
# code is the European English code (the European game added the other four
# languages beside it), so either edition runs in the English slot: its
# message bank in every slot's, its pictures in the English files, its fonts
# and their widths for the European ones.
#
# The Japanese font numbers its characters differently: the European code
# writes some itself (the space, the stars, the coin, the times sign, the dash
# and the digits) as the European font numbers them. The Japanese edition
# moves those characters to the European numbers, and the characters the
# European numbers held to where they were, in both fonts, their widths and
# the text: JAPANESE_MOVES, (Japanese number, European number).

JAPANESE_MOVES = ((0xF1, 0x4D), (0xCC, 0xF0), (0xCD, 0xF1), (0xCE, 0xEE), (0xCF, 0xEF), (0xCA, 0x47),
                  (0x48, 0x00), (0x49, 0x01), (0x4A, 0x02), (0x4B, 0x03), (0x4C, 0x04), (0x4D, 0x05),
                  (0x4E, 0x06), (0x4F, 0x07), (0x58, 0x08), (0x59, 0x09))
MG_PICTURES = ('MG/d_2d_mg_bg_option_nsc.bin', 'MG/d_2d_mg_card_ncg.bin', 'MG/d_2d_mg_common_ncg.bin',
               'MG/d_2d_mg_jump_ncg.bin', 'MG/d_2d_mg_menu_touch_ncg.bin', 'MG/d_2d_mg_option_ncg.bin',
               'MG/d_2d_mg_pazzle_panel_ncg.bin', 'MG/d_2d_mg_smart_ball_lay00_ncg.bin',
               'MG/d_2d_mg_smart_ball_lay01_ncg.bin', 'MG/d_2d_mg_trampoline_ncg.bin')
EU_FONT = 'data/2D_cad/EUR/d_2d_mario_3Dfont_PAL_ncg.bin'
EU_BIG_FONT = 'data/2D_cad/EUR/d_2d_mario_3Dfont_16_16_PAL_ncg.bin'
EU_TEXBANK = 'data/DSMT/EUR/texBank_E.bmd'
ENG = 'data/2D_cad/EUR/ENG/'


def region_spec(language):
    """What the Japanese or American edition takes from its cartridge."""
    j = language == 'ja'
    d, u, us = ('JPN/', '', '') if j else ('USA/', '_U', '_US')
    files = {   # the cartridge's file: the European file it becomes
        f'data/2D_cad/{d}d_2d_battle_framework_course_select{u}_nsc.bin':
            'data/2D_cad/EUR/d_2d_battle_framework_course_select_P_nsc.bin',
        f'data/2D_cad/{d}d_2d_battle_framework_gamestart{u}_nsc.bin':
            'data/2D_cad/EUR/d_2d_battle_framework_gamestart_P_nsc.bin',
        f'data/2D_cad/{d}d_2d_cmn_icon{u}_ncg.bin': ENG + 'd_2d_cmn_icon_E_ncg.bin',
        f'data/DSMT/{d}face_demo_big_yoshistar{u}.bca': 'data/DSMT/EUR/face_demo_big_yoshistar_U.bca',
        f'data/normal_obj/opening/{d}peach_letter{us}.bmd': 'data/normal_obj/opening/EUR/peach_letter_US.bmd',
    }
    if j:
        files.update({p: p for p in MG_PICTURES})
    members = {   # its ARCHIVE/c2d.narc's: the member of the European ARCHIVE/cee.narc it becomes
        f'data/2D_cad/{d}d_2d_battle_btm_course_player_select{u}_ncg.bin':
            ENG + 'd_2d_battle_btm_course_player_select_EU_ncg.bin',
        f'data/2D_cad/{d}d_2d_battle_cmn_icon{u}_ncg.bin': ENG + 'd_2d_battle_cmn_icon_E_ncg.bin',
        f'data/2D_cad/{d}d_2d_battle_upr_course_player_select{u}_ncg.bin':
            ENG + 'd_2d_battle_upr_course_player_select_EU_ncg.bin',
        f'data/2D_cad/{d}d_2d_battle_upr_icon{u}_ncg.bin': ENG + 'd_2d_battle_upr_icon_E_ncg.bin',
        f'data/2D_cad/{d}d_2d_game_over_BG{u}_ncg.bin': ENG + 'd_2d_game_over_BG_E_ncg.bin',
        f'data/2D_cad/{d}d_2d_touch_select_4bit{u}_ncg.bin': ENG + 'd_2d_touch_select_4bit_E_ncg.bin',
        f'data/2D_cad/{d}d_2d_upr_icon{u}_ncg.bin': ENG + 'd_2d_upr_icon_E_ncg.bin',
        f'data/2D_cad/{d}d_2d_upr_play_mode_select{u}_ncg.bin': ENG + 'd_2d_upr_play_mode_select_E_ncg.bin',
    }
    p = '' if j else '_US_PAL'
    return {
        'files': files, 'members': members,
        'fonts': (f'data/2D_cad/{d}d_2d_mario_3Dfont{p}_ncg.bin', f'data/2D_cad/{d}d_2d_mario_3Dfont_16_16{p}_ncg.bin'),
        'bank': 'data/message/msg_data_jpn.bin' if j else 'data/message/msg_data_nes.bin',
        'texbank': f'data/DSMT/{d}texBank{u}.bmd',
        # its arm9's font widths (data_0208f074 and data_0208f174 in the European one)
        'widths': (0x0208DA2C, 0x0208DB2C) if j else (0x0208D90C, 0x0208DA0C),
        'moves': JAPANESE_MOVES if j else (),
        'sha': JAPANESE_SHA if j else AMERICAN_SHA,
    }


JAPANESE_SHA = '612355421d33bb4b455288e26f951b6a8f7f200d78cf274e87cf113fc37107f2'   # region_sha of the retail
AMERICAN_SHA = 'dcdcf53eb1b2a5fbd749068375c9fcb4a93c8b5176c4424ae3bd4bb3a791e709'   # cartridges (Rev 1)


def code_map(moves):
    """Each character's new number for moves ((from, to), ...): those moved,
    and each one whose number they took to a number they left."""
    m = list(range(256))
    for a, b in moves:
        m[a] = b
    taken = sorted({b for _, b in moves} - {a for a, _ in moves})
    freed = sorted({a for a, _ in moves} - {b for _, b in moves})
    for a, b in zip(taken, freed):
        m[a] = b
    return m


def move_glyphs(font, m, tiles):
    """font (4bpp tiles) with each character c's tiles at character m[c]'s."""
    out = bytearray(font)
    for c in range(256):
        for a, b in zip(tiles(c), tiles(m[c])):
            out[b * 32:b * 32 + 32] = font[a * 32:a * 32 + 32]
    return bytes(out)


def small_tiles(c):   # 8x16: Message::AddChar
    t = (c & 0x1F) + ((c & 0xE0) << 1)
    return (t, t + 32)


def big_tiles(c):     # 16x16: func_0201b100
    t = (c & 0xF) * 2 + (c & 0xF0) * 4
    return (t, t + 1, t + 32, t + 33)


def lz10_wrap(raw):
    """A type-0x10 LZ stream (no "LZ77" in front) of raw bytes, as literal runs."""
    out = bytearray(bytes((0x10, len(raw) & 0xFF, len(raw) >> 8 & 0xFF, len(raw) >> 16 & 0xFF)))
    for i in range(0, len(raw), 8):
        out.append(0)
        out += raw[i:i + 8]
    return bytes(out)


def recode_bank(bank, m):
    """A message bank (LZ77) with each character c of its text written m[c];
    0xFD (a new line), 0xFE (a command, its length next) and 0xFF (the end)
    stay."""
    b = bytearray(lz77(bank))
    if b[:8] != b'GSEM1gmb' or b[0x20:0x24] != b'1FNI':
        raise BuildError("A message bank isn't one this build can read. Re-dump the cartridge.")
    info = 0x20
    count = u16(b, info + 8)
    data = info + u32(b, info + 4) + 8
    done = bytearray(len(b))
    for i in range(count):
        p = data + u32(b, info + 0x10 + i * 8)
        while p < len(b) and not done[p]:
            c = b[p]
            if c == 0xFF:
                break
            if c == 0xFE:
                n = b[p + 1]
                if n < 2:
                    raise BuildError("A message bank isn't one this build can read. Re-dump the cartridge.")
                done[p:p + n] = b'\1' * n
                p += n
                continue
            done[p] = 1
            if c != 0xFD:
                b[p] = m[c]
            p += 1
    return lz77_wrap(bytes(b))


def region_sha(files, arm9, spec):
    h = hashlib.sha256()
    for p in sorted(files):
        h.update(p.encode('ascii') + b'\0' + hashlib.sha256(files[p]).digest())
    for a in spec['widths']:
        h.update(arm9[a - 0x02004000:a - 0x02004000 + 0x100])
    return h.hexdigest()


def region_extract(rb, language, log):
    """What the Japanese or American edition takes out of its dump: {'files':
    {path: bytes}, 'widths': the fonts' widths}."""
    arm9 = second_dump(rb, language)
    code, who = LANGUAGES[language][2], REGION[LANGUAGES[language][2]]
    log(f'Super Mario 64 DS ({code}, {"Japan" if language == "ja" else "USA"}), {round(len(rb) / 1048576)} MB')
    spec = region_spec(language)
    want = list(spec['files']) + ['ARCHIVE/c2d.narc', spec['bank'], spec['texbank']]
    files = nitro_files(rb, want, who)
    missing = [p for p in want if p not in files]
    if missing:
        raise BuildError(f"The {who} dump has no {missing[0]}. Re-dump the cartridge.")
    if region_sha(files, arm9, spec) != spec['sha']:
        raise not_that_cartridge(language)
    widths = b''.join(arm9[a - 0x02004000:a - 0x02004000 + 0x100] for a in spec['widths'])
    log(f'    the {LANGUAGES[language][0]} text: {len(files)} files and the fonts, checksums OK')
    return {'files': files, 'widths': widths}


def region_texbank(eu_e, src, language):
    """texBank_E (the title, file and options menus) with the cartridge's
    pictures. The European title menu's picture has its pieces elsewhere (the
    American and Japanese have the mini-game button where the European has the
    wrench, and the wrench and arrows below), so they go where the European
    has them."""
    e, k = bytearray(lz77(eu_e)), lz77(src)
    tex = lambda b, n: bmd_find(b, 0x18, 0x14, 20, n, None)
    pal = lambda b, n: bmd_find(b, 0x20, 0x1C, 16, n, None)
    j = language == 'ja'
    u = '' if j else '_U'
    for kt, kp, et, ep in ((f'file_menu{u}', f'file_menu{u}_pl', 'file_menu_U', 'file_menu_U_pl'),
                           (f'title_menu{u}', f'title_menu{u}_pl', 'title_menu_E', 'title_menu_E_pl'),
                           (f'wallpaper_logo{u}', f'wallpaper_logo{u}', 'wallpaper_logo_U', 'wallpaper_logo_U')):
        (ka, ks), (kpa, kps), (ea, es), (epa, eps) = tex(k, kt), pal(k, kp), tex(e, et), pal(e, ep)
        if ks != es or kps != eps:
            raise BuildError("The title screen's textures aren't the ones this build knows. Re-dump the cartridge.")
        pic = k[ka:ka + ks]
        if et == 'title_menu_E':
            # (x, y, w, h) in the Japanese picture: (x, y) in the European one
            menu = bytearray(ks)
            for sx, sy, w, h, dx, dy in ((0, 0, 64, 40, 0, 0),         # VS
                                         (64, 0, 64, 40, 0, 80),        # the mini-games: the Rec Room
                                         (0, 40, 128, 40, 0, 40),       # ADVENTURE
                                         (0, 80, 32, 32, 96, 0),        # the wrench
                                         (32, 80, 48, 16, 80, 112)):    # the arrows
                for y in range(h):
                    menu[(dy + y) * 128 + dx:(dy + y) * 128 + dx + w] = pic[(sy + y) * 128 + sx:(sy + y) * 128 + sx + w]
            pic = bytes(menu)
        e[ea:ea + es] = pic
        e[epa:epa + eps] = k[kpa:kpa + kps]
    # the options screen: the same layout, but the European picture has 200
    # colours, so the cartridge's colours past 200 become their nearest below
    (ka, ks), (kpa, kps) = tex(k, f'title_option{u}'), pal(k, f'title_option{u}')
    (ea, es), (epa, eps) = tex(e, 'title_option_EU'), pal(e, 'title_option_pl')
    if ks != es or kps != 0x200 or eps != 0x190:
        raise BuildError("The title screen's textures aren't the ones this build knows. Re-dump the cartridge.")
    opal = bmd_colours(k, kpa, 256)
    below = list(range(200)) + [nearest(opal[i], range(1, 200), opal) for i in range(200, 256)]
    e[ea:ea + es] = bytes(below[v] for v in k[ka:ka + ks])
    e[epa:epa + eps] = k[kpa:kpa + eps]
    return lz77_wrap(bytes(e))


def region_apply(files, assets, got, language, log):
    """The Japanese or American files over the European English ones."""
    spec, src = region_spec(language), got['files']
    m = code_map(spec['moves'])
    new = {eu: src[p] for p, eu in spec['files'].items()}
    bank = recode_bank(src[spec['bank']], m) if spec['moves'] else src[spec['bank']]
    for p in BANK_SLOTS:
        new[p] = bank
    c2d = src['ARCHIVE/c2d.narc']
    new['ARCHIVE/cee.narc'] = narc_replace(files['ARCHIVE/cee.narc'],
                                           {eu: narc_get(c2d, p) for p, eu in spec['members'].items()})
    font, big = narc_get(c2d, spec['fonts'][0]), lz77(narc_get(c2d, spec['fonts'][1]))
    if spec['moves']:
        font, big = move_glyphs(font, m, small_tiles), move_glyphs(big, m, big_tiles)
    new['ARCHIVE/c2d.narc'] = narc_replace(files['ARCHIVE/c2d.narc'], {EU_FONT: font, EU_BIG_FONT: lz10_wrap(big)})
    new[EU_TEXBANK] = region_texbank(files[EU_TEXBANK], src[spec['texbank']], language)
    widths = bytearray(512)
    for c in range(256):
        widths[m[c]], widths[256 + m[c]] = got['widths'][c], got['widths'][256 + c]
    changed = replace_files(files, assets, new, LANGUAGES[language][0])
    assets['langpack.bin'] = langpack(language, bytes(widths))
    log(f'    {len(new)} files {LANGUAGES[language][0]}, {changed} entries in the file table updated')


# --- the American or Japanese cartridge alone (Regions.cs) ------------------
# Without a European dump, what extract() takes out of one is made out of the
# American (ASME) or Japanese (ASMJ) cartridge: builder-files/region-ASME.tsv
# (region-ASMJ.tsv), which region_kit.py makes from all three cartridges, says
# where each piece of romdata.bin (the European layout) is in that cartridge's
# program, the numbers the European layout has in place of the cartridge's
# own (its pointers and file numbers), the European file system's names, and
# which of the cartridge's files each European file is. What the cartridge has
# nothing for (the four other languages' tables and pictures) is left empty,
# as the game runs in the English slot; then region_apply makes the edition as
# it does with the two dumps. Its lines, tab-separated:
#
#   code ASME | layout <sha> <len> | blob <sha> | leaves <sha> | ovt <sha>
#   img <module> <len> <sha>       a module of the cartridge (arm9, ov000...), decompressed
#   r <off> <len> <module> <from>  romdata.bin bytes, from the module
#   w <off> <hex word> ...         romdata.bin words, the European numbers
#   b <off> <hex>                  romdata.bin bytes, the European names
#   D <first> <parent> <entry> ... a directory of the file name table, in order:
#                                  f:<name> a file, d:<name>:<id> a directory
#   F <path> <how>                 a file of it; <how> is = <path> (the cartridge's file),
#                                  m <narc> <member> (a member of one of its archives),
#                                  c <path> (the European file's copy), z <lz> <size> (empty),
#                                  t <path> (the texture bank, then T and P lines)
#   A <narc> | N <narc> <first> <parent> <entry> ... | M <narc> <member> <how>
#                                  an archive, its names (as D) and its members (as F)
#   T <path> <name> <size> <w> <h> <params> <from>   the texture bank's textures in
#   P <path> <name> <size> <from>                    order, and palettes ('-': none)

REGION_DUMPS = {'ASME': 'en-us', 'ASMJ': 'ja'}


def region_modules(rb):
    """{'arm9': bytes, 'ov000': bytes, ...}: the cartridge's program, decompressed."""
    a9 = u32(rb, 0x20)
    try:
        out = {'arm9': expand_blz(rb[a9:a9 + u32(rb, 0x2C)])}
    except ValueError as e:
        raise BuildError(f"Could not read this dump's program data ({e}). Re-dump the cartridge.")
    fat, table, size = u32(rb, 0x48), u32(rb, 0x50), u32(rb, 0x54)
    for i in range(size // 32):
        e = table + i * 32
        ident, f, flags = u32(rb, e), u32(rb, e + 24), u32(rb, e + 28)
        start, end = u32(rb, fat + f * 8), u32(rb, fat + f * 8 + 4)
        if f >= u32(rb, 0x4C) // 8 or end < start or end > len(rb):
            raise BuildError('This dump is damaged: an overlay runs past the end of the file. Re-dump the cartridge.')
        try:
            out[f'ov{ident:03d}'] = expand_blz(rb[start:end]) if flags & 0x1000000 else rb[start:end]
        except ValueError as e:
            raise BuildError(f"Could not read overlay {ident} of this dump ({e}). Re-dump the cartridge.")
    return out


def fnt_build(dirs):
    """A file name table from its directories [(first id, parent, [(kind, name, id)])]."""
    subs = []
    for _, _, ents in dirs:
        b = bytearray()
        for k, name, sub in ents:
            nb = name.encode('ascii')
            b += bytes([len(nb) | (0x80 if k == 'd' else 0)]) + nb + (struct.pack('<H', sub) if k == 'd' else b'')
        subs.append(bytes(b) + b'\0')
    out, off = bytearray(), len(dirs) * 8
    for (first, parent, _), s in zip(dirs, subs):
        out += struct.pack('<IHH', off, first, parent)
        off += len(s)
    return bytes(out) + b''.join(subs)


def fnt_walk(dirs):
    """[(file id, path)] of a file name table's files."""
    out, queue = [], [(0, '')]
    while queue:
        d, base = queue.pop(0)
        fid = dirs[d][0]
        for k, name, sub in dirs[d][2]:
            if k == 'd':
                queue.append((sub & 0xFFF, base + name + '/'))
            else:
                out.append((fid, base + name))
                fid += 1
    return out


def narc_build(dirs, data):
    """A NARC archive of the members data (by index) named by dirs (as fnt_build)."""
    btaf, gmif = bytearray(b'BTAF' + struct.pack('<IHH', 12 + 8 * len(data), len(data), 0)), bytearray()
    for b in data:
        btaf += struct.pack('<II', len(gmif), len(gmif) + len(b))
        gmif += b + b'\xff' * (-len(b) & 3)
    names = fnt_build(dirs)
    names += b'\xff' * (-len(names) & 3)
    body = bytes(btaf) + b'BTNF' + struct.pack('<I', 8 + len(names)) + names + \
        b'GMIF' + struct.pack('<I', 8 + len(gmif)) + bytes(gmif)
    return b'NARC\xfe\xff\x00\x01' + struct.pack('<IHH', 16 + len(body), 16, 3) + body


def texbank_build(src, textures, palettes):
    """A texture bank (a BMD of one material) with the textures [(name, w, h,
    params, data)] and palettes [(name, data)] in place of src's, laid out as
    the cartridge lays its out: the tables and their names, the material, then
    the textures' data and the palettes'."""
    b = lz77(src)
    mat_count, mat_at = u32(b, 0x24), u32(b, 0x28)
    head = bytearray(b[:0xE0])   # the header, the bone and the display list
    out = bytearray(head)
    tex_at = len(out)
    out += bytes(20 * len(textures))
    names = []
    for t in textures:
        names.append(len(out))
        out += t[0].encode('ascii') + b'\0'
    out += bytes(-len(out) & 3)
    pal_at = len(out)
    out += bytes(16 * len(palettes))
    pnames = []
    for p in palettes:
        pnames.append(len(out))
        out += p[0].encode('ascii') + b'\0'
    out += bytes(-len(out) & 3)
    mats = len(out)
    mat = bytearray(b[mat_at:mat_at + 48 * mat_count])
    out += mat
    for i in range(mat_count):
        n = u32(b, mat_at + i * 48)
        struct.pack_into('<I', out, mats + i * 48, len(out))
        out += b[n:b.index(b'\0', n) + 1]
    out += bytes(-len(out) & 3)
    data_at = len(out)
    for i, (name, w, h, params, data) in enumerate(textures):
        struct.pack_into('<IIIHHI', out, tex_at + i * 20, names[i], len(out), len(data), w, h, params)
        out += data
    for i, (name, data) in enumerate(palettes):
        struct.pack_into('<IIIi', out, pal_at + i * 16, pnames[i], len(out), len(data), -1)
        out += data
    struct.pack_into('<IIIIII', out, 0x14, len(textures), tex_at, len(palettes), pal_at, mat_count, mats)
    struct.pack_into('<I', out, 0x38, data_at)
    return lz77_wrap(bytes(out))


def texbank_parts(bmd):
    """({name: (w, h, params, data)}, {name: data}) of a texture bank."""
    b = lz77(bmd)
    tex, pal = {}, {}
    for i in range(u32(b, 0x14)):
        e = u32(b, 0x18) + i * 20
        n, at, size, w, h, params = struct.unpack_from('<IIIHHI', b, e)
        tex[b[n:b.index(b'\0', n)].decode('ascii')] = (w, h, params, b[at:at + size])
    for i in range(u32(b, 0x1C)):
        e = u32(b, 0x20) + i * 16
        n, at, size = struct.unpack_from('<III', b, e)
        pal[b[n:b.index(b'\0', n)].decode('ascii')] = b[at:at + size]
    return tex, pal


def region_kit(code):
    """The kit's lines for a cartridge, split into their fields."""
    with open(kit(f'region-{code}.tsv'), encoding='utf-8') as f:
        return [line.rstrip('\n').split('\t') for line in f if line.strip() and not line.startswith('#')]


def dirs_of(fields):
    ents = []
    for e in fields:
        k, rest = e.split(':', 1)
        if k == 'd':
            name, sub = rest.rsplit(':', 1)
            ents.append(('d', name, int(sub)))
        else:
            ents.append(('f', rest, None))
    return ents


def standalone_extract(rb, recipe_path, manifest_path, log, progress):
    """(files, assets) as extract() gives them, out of the American or Japanese
    cartridge alone (above)."""
    code = rb[0x0C:0x10].decode('ascii', 'replace')
    who = REGION[code]
    log(f'Super Mario 64 DS ({code}, {"Japan" if code == "ASMJ" else "USA"}), {round(len(rb) / 1048576)} MB')
    lines = region_kit(code)
    head = {f[0]: f[1:] for f in lines if f[0] in ('code', 'layout', 'blob', 'leaves', 'ovt')}
    damaged = BuildError(f"This builder's region-{code}.tsv is damaged. Download the builder again.")
    if head.get('code') != [code] or 'layout' not in head or 'blob' not in head:
        raise damaged
    progress(5, 'Decompressing the program')
    mods = region_modules(rb)
    for f in lines:
        if f[0] == 'img' and (f[1] not in mods or len(mods[f[1]]) != int(f[2]) or sha256(mods[f[1]]) != f[3]):
            raise not_that_cartridge(REGION_DUMPS[code])

    # romdata.bin
    progress(20, 'Rebuilding romdata.bin')
    total = int(head['layout'][1])
    blob = bytearray(total)
    try:
        for f in lines:
            if f[0] == 'r':
                off, n, frm = int(f[1]), int(f[2]), int(f[4])
                blob[off:off + n] = mods[f[3]][frm:frm + n]
            elif f[0] == 'w':
                off = int(f[1])
                for i, w in enumerate(f[2].split()):
                    struct.pack_into('<I', blob, off + i * 4, int(w, 16))
            elif f[0] == 'b':
                raw = bytes.fromhex(f[2])
                blob[int(f[1]):int(f[1]) + len(raw)] = raw
    except (ValueError, KeyError, IndexError, struct.error):
        raise damaged
    if len(blob) != total or sha256(blob) != head['blob'][0]:
        raise not_that_cartridge(REGION_DUMPS[code])
    log(f'    romdata.bin made ({total} bytes, checksum OK)')
    with open(manifest_path, encoding='utf-8') as f:
        man = f.read().splitlines()
    m = re.match(r'^# romdata-manifest v1 ([0-9a-f]{64}) (\d+)$', man[0]) if man else None
    if not m or m.group(1) != head['layout'][0] or int(m.group(2)) != total:
        raise BuildError(f"This builder's region-{code}.tsv belongs to another build of the game. "
                         'Download the builder again.')
    # the manifest of this blob, its layout the European one's (game/language.cpp)
    out = [f'# romdata-manifest v1 {sha256(blob)} {total} layout {m.group(1)}']
    for line in man[1:]:
        f = line.split('\t')
        if f[0] == 'part' and len(f) == 5:
            f[4] = sha256(blob[int(f[2]):int(f[2]) + int(f[3])])
        elif f[0] == 'sym' and len(f) == 6:
            f[5] = sha256(blob[int(f[3]):int(f[3]) + int(f[4])])
        out.append('\t'.join(f))
    assets = {'romdata.bin': bytes(blob), 'romdata.manifest': ('\n'.join(out) + '\n').encode()}

    # the cartridge's files
    progress(40, 'Reading the file tables')
    fnt_off, fnt_size, fat_off, fat_size, ovt_off, ovt_size = struct.unpack_from('<6I', rb, 0x40)
    have = {p: b for i, p in fnt_walk(fnt_dirs(rb)) for b in [rb_file(rb, i)]}
    narcs = {}

    def member(narc, name):
        if narc not in narcs:
            names, _ = narc_members(have[narc])
            narcs[narc] = (names, narc_files(have[narc]))
        names, data = narcs[narc]
        return data[names[name]] if name in names else None
    used = hashlib.sha256()
    for f in lines:
        if f[0] in ('F', 'M'):
            how = f[2:] if f[0] == 'F' else f[3:]
            if how[0] == '=' or how[0] == 't':
                b = have.get(how[1])
            elif how[0] == 'm':
                b = member(how[1], how[2]) if how[1] in have else None
            else:
                continue
            if b is None:
                raise BuildError(f"The {who} dump has no {how[-1]}. Re-dump the cartridge.")
            used.update(hashlib.sha256(b).digest())
    if used.hexdigest() != head['leaves'][0] or sha256(rb[ovt_off:ovt_off + ovt_size]) != head['ovt'][0]:
        raise not_that_cartridge(REGION_DUMPS[code])

    # the European file system
    progress(60, 'Making the European file system')
    dirs = [(int(f[1]), int(f[2]), dirs_of(f[3:])) for f in lines if f[0] == 'D']
    adirs, how_f, how_m, tex, pal = {}, {}, {}, {}, {}
    for f in lines:
        if f[0] == 'N':
            adirs.setdefault(f[1], []).append((int(f[2]), int(f[3]), dirs_of(f[4:])))
        elif f[0] == 'F':
            how_f[f[1]] = f[2:]
        elif f[0] == 'M':
            how_m[(f[1], f[2])] = f[3:]
        elif f[0] == 'T':
            tex.setdefault(f[1], []).append(f[2:])
        elif f[0] == 'P':
            pal.setdefault(f[1], []).append(f[2:])
    done = {}

    def make(key, how):
        if key in done:
            return done[key]
        k = how[0]
        if k == '=':
            b = have[how[1]]
        elif k == 'm':
            b = member(how[1], how[2])
        elif k == 'c':
            b = make(how[1], how_f[how[1]]) if ':' not in how[1] else \
                make(tuple(how[1].split(':', 1)), how_m[tuple(how[1].split(':', 1))])
        elif k == 'z':
            raw = bytes(int(how[2]))
            b = {'lz77': lz77_wrap, 'lz10': lz10_wrap, 'raw': bytes}[how[1]](raw)
        elif k == 't':
            st, sp = texbank_parts(have[how[1]])
            t = [(n, int(w), int(h), int(p, 16), st[s][3] if s != '-' else bytes(int(size)))
                 for n, size, w, h, p, s in tex[key]]
            p = [(n, pal_of(sp, s, int(size))) for n, size, s in pal[key]]
            b = texbank_build(have[how[1]], t, p)
        elif k == 'A':
            fl = fnt_walk(adirs[key])
            data = [None] * len(fl)
            for i, (fid, name) in enumerate(fl):
                data[fid - adirs[key][0][0]] = make((key, name), how_m[(key, name)])
            b = narc_build(adirs[key], data)
        else:
            raise damaged
        done[key] = b
        return b
    files = {}
    eu = fnt_walk(dirs)
    for fid, p in eu:
        if p not in how_f:
            raise damaged
        files[p] = make(p, how_f[p])
    log(f'    {len(files)} files, {round(sum(len(b) for b in files.values()) / 1048576, 1)} MB')

    # the tables: the overlays' entries as the cartridge's, then the files
    progress(80, 'Writing the file tables')
    fnt = fnt_build(dirs)
    first = dirs[0][0]
    count = first + len(eu)
    fat = bytearray(count * 8)
    fat[:first * 8] = rb[fat_off:fat_off + first * 8]
    new_fat = (fnt_off + len(fnt) + 0x1FF) & ~0x1FF
    at = (new_fat + count * 8 + 0x1FF) & ~0x1FF
    for fid, p in sorted(eu):
        struct.pack_into('<II', fat, fid * 8, at, at + len(files[p]))
        at = (at + len(files[p]) + 0x1FF) & ~0x1FF
    if at > 0x8000000:
        raise BuildError(f"The {who} files don't fit the cartridge's address space.")
    assets['nitrofs_fnt.bin'], assets['nitrofs_fat.bin'] = fnt, bytes(fat)
    assets['nitrofs_ovt9.bin'] = rb[ovt_off:ovt_off + ovt_size]
    assets['nitrofs.tsv'] = (f'key\tvalue\nfnt_offset\t{fnt_off}\nfnt_size\t{len(fnt)}\nfat_offset\t{new_fat}\n'
                             f'fat_size\t{len(fat)}\novt9_offset\t{ovt_off}\novt9_size\t{ovt_size}\n'
                             f'ovt7_offset\t0\novt7_size\t0\n').encode()
    file_rows = ['file_id\thex_id\tpath\tkind\tsize\n']
    for fid, p in sorted(eu):
        file_rows.append(f'{fid}\t0x{fid:04x}\t{p}\t{kind(p)}\t{len(files[p])}\n')
    # the handles: overlay 0's table (romdata.bin's first part) and its names
    ov0 = eu_module(blob, recipe_path, 'ov000')
    id_by_path = {p.lower(): fid for fid, p in eu}
    path_by_id = dict(eu)
    handle_rows = ['handle\thex_handle\tfile_id\thex_file_id\tpath\tkind\tsize\n']
    for h in range(HANDLE_COUNT):
        so = u32(ov0, HANDLE_TABLE_ADDRESS - OV0_ADDRESS + h * 4) - OV0_ADDRESS
        asset = ov0[so:ov0.index(b'\0', so)].decode('ascii', 'replace')
        if asset.lower() not in id_by_path:
            raise damaged
        i = id_by_path[asset.lower()]
        handle_rows.append(f'{h}\t0x{h:04x}\t{i}\t0x{i:04x}\t{asset}\t{kind(asset)}\t{len(files[path_by_id[i]])}\n')
    assets['files.tsv'], assets['handles.tsv'] = ''.join(file_rows).encode(), ''.join(handle_rows).encode()
    log(f'    {len(eu)} files and {HANDLE_COUNT} handles catalogued')
    progress(100, 'Done')
    return files, assets


OV0_ADDRESS = 0x020AA420   # overlay 0 in the European program


def pal_of(pals, name, size):
    return pals[name][:size] if name != '-' else bytes(size)


def eu_module(blob, recipe_path, module):
    """The European module's bytes romdata.bin has (the rest zero), as one image."""
    with open(recipe_path, encoding='utf-8') as f:
        rows = [line.split('\t') for line in f.read().splitlines() if line and not line.startswith('#')]
    rows = [(int(r[0]), int(r[1]), int(r[3])) for r in rows if r[2].lower() == module]
    img = bytearray(max(s + n for _, n, s in rows))
    for off, n, s in rows:
        img[s:s + n] = blob[off:off + n]
    return bytes(img)


def fnt_dirs(rb):
    """The cartridge's file name table, as fnt_build's directories."""
    fnt_off, fnt_size = u32(rb, 0x40), u32(rb, 0x44)
    fnt = rb[fnt_off:fnt_off + fnt_size]
    dirs = []
    for d in range(u16(fnt, 6)):
        off, first, parent = struct.unpack_from('<IHH', fnt, d * 8)
        ents, o = [], off
        while fnt[o]:
            t = fnt[o]
            name = fnt[o + 1:o + 1 + (t & 0x7F)].decode('ascii', 'replace')
            o += 1 + (t & 0x7F)
            if t & 0x80:
                ents.append(('d', name, u16(fnt, o)))
                o += 2
            else:
                ents.append(('f', name, None))
        dirs.append((first, parent, ents))
    return dirs


def rb_file(rb, fid):
    fat = u32(rb, 0x48)
    start, end = u32(rb, fat + fid * 8), u32(rb, fat + fid * 8 + 4)
    if end < start or end > len(rb):
        raise BuildError('This dump is damaged: its file table points past the end of the file. Re-dump the cartridge.')
    return rb[start:end]


def pack(files):
    """files.pak (Builder.cs Pack): S3PK, the files in path order with their
    folders compared case-blind, each 4-byte aligned. files.txt is left out."""
    names = sorted((p for p in files if p != 'files.txt'),
                   key=lambda p: [part.lower() for part in p.split('/')])
    encoded = [n.encode('utf-8') for n in names]
    data_off = (16 + sum(10 + len(n) for n in encoded) + 3) & ~3
    out = bytearray(b'S3PK' + struct.pack('<III', 1, len(names), data_off))
    at = data_off
    for name, enc in zip(names, encoded):
        size = len(files[name])
        out += struct.pack('<IIH', at, size, len(enc)) + enc
        at += size + (-size & 3)
    out += bytes(data_off - len(out))
    for name in names:
        b = files[name]
        out += b + bytes(-len(b) & 3)
    return bytes(out)


# --- HOME Menu art from the dump (Builder.cs Art) ---------------------------

def ds_icon(rb):
    """The DS game's 32x32 16-colour icon, RGBA rows top first (colour 0 clear)."""
    banner = u32(rb, 0x68)
    if banner == 0 or banner + 0x240 > len(rb):
        raise BuildError('The dump has no DS icon to make the HOME Menu icon from.')
    tiles, pal = rb[banner + 0x20:banner + 0x220], rb[banner + 0x220:banner + 0x240]
    colours = []
    for i in range(16):
        c = u16(pal, i * 2)
        r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
        colours.append((0, 0, 0, 0) if i == 0 else (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255))
    px = [None] * 1024
    for ty in range(4):
        for tx in range(4):
            for y in range(8):
                for x in range(8):
                    b = tiles[(ty * 4 + tx) * 32 + y * 4 + x // 2]
                    px[(ty * 8 + y) * 32 + tx * 8 + x] = colours[b & 15 if x & 1 == 0 else b >> 4]
    return px


def icon48(ds):
    """Art.SaveIcon: the icon at 44x44 in the middle of a white 48x48, each pixel
    the area-weighted average of the DS pixels under it, in whole numbers."""
    out = []
    for y in range(48):
        for x in range(48):
            if not (2 <= x < 46 and 2 <= y < 46):
                out.append((255, 255, 255, 255))
                continue
            sr = sg = sb = 0
            x0, y0 = (x - 2) * 32, (y - 2) * 32
            for sy in range(y0 // 44, (y0 + 31) // 44 + 1):
                for sx in range(x0 // 44, (x0 + 31) // 44 + 1):
                    w = ((min(x0 + 32, sx * 44 + 44) - max(x0, sx * 44)) *
                         (min(y0 + 32, sy * 44 + 44) - max(y0, sy * 44)))
                    r, g, b, a = ds[sy * 32 + sx]
                    if a == 0:
                        r = g = b = 255
                    sr += w * r
                    sg += w * g
                    sb += w * b
            out.append(((sr + 512) // 1024, (sg + 512) // 1024, (sb + 512) // 1024, 255))
    return out


def overlay(rb, ident):
    """An ARM9 overlay out of the dump, decompressed; None if it isn't there."""
    fat, table, size = u32(rb, 0x48), u32(rb, 0x50), u32(rb, 0x54)
    for i in range(size // 32):
        e = table + i * 32
        if u32(rb, e) != ident:
            continue
        f, flags = u32(rb, e + 24), u32(rb, e + 28)
        start, end = u32(rb, fat + f * 8), u32(rb, fat + f * 8 + 4)
        if end <= start or end > len(rb):
            return None
        raw = rb[start:end]
        if flags & 0x1000000:
            try:
                return expand_blz(raw)
            except ValueError:
                return None
        return raw
    return None


LOGO_PALETTE = '8d3a76050a99c1b2007249164cb4047450ea607d5a70a01f90017a2cedf4cc2a'
LOGO_SCREEN = 'eca1045a4f516efcfa532f0ed85bf97a2d9bef295db19f51d97825b35780b4fd'
LOGOS = {   # the cartridge: overlay 7's palette, screen and characters (offset, size, sha256)
    'ASMP': ((0x2BF08, 556, LOGO_PALETTE), (0x30068, 6420, LOGO_SCREEN),
             (0x3D08C, 33392, '16eac000c3f8de4135ccd1b78d465aa52bb350fba1323162f4ddf20f2f1bbc83')),
    'ASME': ((0x2BA48, 556, LOGO_PALETTE), (0x2FB80, 6420, LOGO_SCREEN),
             (0x3CBA4, 33392, 'f168e397924567e3445561dad3f178b96608ec27ef7fae893da1bb6cf3335ad5')),
    'ASMJ': ((0x2BA08, 556, 'f09e6584545ea7ed02a6a5df60ce7a14a201a9beda1f59fa89367b0a316870c1'), (0x2F764, 6420, LOGO_SCREEN),
             (0x3C788, 33388, 'd2eda4d702a93acfb549c5db6514c76271347b470a60468700ed9ab3d72593c0')),
}


def title_logo(rb):
    """The title screen's logo from overlay 7 (NCCL/NCSC/NCCG blocks, each checked
    against the retail cartridge's hash, LOGOS): (width, height, RGBA rows), or None."""
    want = LOGOS.get(rb[0x0C:0x10].decode('ascii', 'replace'))
    ov = overlay(rb, 7)
    if ov is None or want is None:
        return None

    def block(at, size, sha):
        b = ov[at:at + size]
        return b if len(b) == size and sha256(b) == sha else None
    pal, scr, chr_ = (block(*w) for w in want)
    if pal is None or scr is None or chr_ is None:
        return None
    colours = [(0, 0, 0, 0)]
    for i in range(1, 256):
        c = u16(pal, 0x20 + i * 2)
        r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
        colours.append((r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255))
    px = [(0, 0, 0, 0)] * (256 * 256)
    min_x = min_y = 256
    max_x = max_y = -1
    for k in range(32 * 32):
        e = u16(scr, 0x28 + k * 2)
        tile, hf, vf = e & 0x3ff, e & 0x400, e & 0x800
        if 0x24 + tile * 64 + 64 > len(chr_):
            continue
        for y in range(8):
            for x in range(8):
                idx = chr_[0x24 + tile * 64 + (7 - y if vf else y) * 8 + (7 - x if hf else x)]
                if idx == 0:
                    continue
                X, Y = (k % 32) * 8 + x, (k // 32) * 8 + y
                px[Y * 256 + X] = colours[idx]
                min_x, max_x, min_y, max_y = min(min_x, X), max(max_x, X), min(min_y, Y), max(max_y, Y)
    if max_x < 0:
        return None
    w, h = max_x - min_x + 1, max_y - min_y + 1
    return w, h, [px[(min_y + y) * 256 + min_x + x] for y in range(h) for x in range(w)]


# --- the 3D banner (Face.cs) ------------------------------------------------

TURN = math.pi * 2 / 4096


def lz77(raw):
    """The game's LZ77 files: "LZ77" and a type-0x10 stream, or plain."""
    i = 4 if raw[:4] == b'LZ77' else 0
    if raw[i] != 0x10:
        return raw
    n = raw[i + 1] | raw[i + 2] << 8 | raw[i + 3] << 16
    o = bytearray(n)
    at = 0
    i += 4
    while at < n:
        flags = raw[i]
        i += 1
        for b in range(8):
            if at >= n:
                break
            if flags & (0x80 >> b):
                v = raw[i] << 8 | raw[i + 1]
                i += 2
                length, disp = (v >> 12) + 3, (v & 0xfff) + 1
                for _ in range(length):
                    if at >= n:
                        break
                    o[at] = o[at - disp]
                    at += 1
            else:
                o[at] = raw[i]
                i += 1
                at += 1
    return bytes(o)


def sx(v, bits):
    x = v & ((1 << bits) - 1)
    return x - (1 << bits) if x & (1 << (bits - 1)) else x


def rgb555(c):
    r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
    return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)


def cname(d, o):
    if o == 0:
        return ''
    e = d.find(b'\0', o)
    return d[o:e if e >= 0 else len(d)].decode('ascii', 'replace')


def sin4096(a):
    """sin of a DS angle (1/4096 turns), the same to the bit on every computer
    and as Face.cs's Sin4096: math.sin differs between systems (and from .NET's
    in the last bit at 269 of the 4096 angles), so this folds the angle into the
    first eighth of a turn and sums the series in plain doubles."""
    q = a % 4096
    neg = q >= 2048
    if neg:
        q -= 2048
    if q > 1024:
        q = 2048 - q
    v = _sin_series(q * (math.pi / 2048)) if q <= 512 else _cos_series((1024 - q) * (math.pi / 2048))
    return -v if neg else v


def _sin_series(x):
    x2 = x * x
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 *
               (1 - x2 / 156 * (1 - x2 / 210 * (1 - x2 / 272))))))))


def _cos_series(x):
    x2 = x * x
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 *
           (1 - x2 / 132 * (1 - x2 / 182 * (1 - x2 / 240)))))))


def euler(rx, ry, rz):
    a0, a1 = sin4096(rx), sin4096(rx + 1024)
    b0, b1 = sin4096(ry), sin4096(ry + 1024)
    c0, c1 = sin4096(rz), sin4096(rz + 1024)
    return [b1 * c1, b1 * c0, -b0,
            b0 * a0 * c1 - a1 * c0, a1 * c1 + b0 * a0 * c0, b1 * a0,
            a0 * c0 + b0 * a1 * c1, b0 * a1 * c0 - a0 * c1, b1 * a1]


def local(scale, rot, trans):
    r = euler(*rot)
    m = [0.0] * 16
    for i in range(3):
        for j in range(3):
            m[i * 4 + j] = scale[i] * r[i * 3 + j]
    m[12], m[13], m[14], m[15] = trans[0], trans[1], trans[2], 1.0
    return m


def mul(a, b):
    r = [0.0] * 16
    for i in range(4):
        for j in range(4):
            s = 0.0
            for k in range(4):
                s += a[i * 4 + k] * b[k * 4 + j]
            r[i * 4 + j] = s
    return r


def apply(v, m, n):
    r = []
    for j in range(3):
        s = 0.0
        for k in range(n):
            s += v[k] * m[k * 4 + j]
        r.append(s)
    return r


def invert(m):
    a, r = list(m), [0.0] * 16
    for i in range(4):
        r[i * 5] = 1.0
    for c in range(4):
        p = c
        for i in range(c + 1, 4):
            if abs(a[i * 4 + c]) > abs(a[p * 4 + c]):
                p = i
        for j in range(4):
            a[c * 4 + j], a[p * 4 + j] = a[p * 4 + j], a[c * 4 + j]
            r[c * 4 + j], r[p * 4 + j] = r[p * 4 + j], r[c * 4 + j]
        d = a[c * 4 + c]
        for j in range(4):
            a[c * 4 + j] /= d
            r[c * 4 + j] /= d
        for i in range(4):
            if i == c:
                continue
            k = a[i * 4 + c]
            for j in range(4):
                a[i * 4 + j] -= k * a[c * 4 + j]
                r[i * 4 + j] -= k * r[c * 4 + j]
    return r


def f32(x):
    """x as C#'s (float) does it: the nearest single."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return struct.unpack('<q', struct.pack('<d', x))[0]


class Bone:
    pass


class Bmd:
    """A BMD0 model (include/BMD_File.h)."""

    def __init__(self, raw):
        d = self.d = lz77(raw)
        self.scale_shift = u32(d, 0)
        self.bones = []
        for i in range(u32(d, 4)):
            o = u32(d, 8) + i * 0x40
            n = u32(d, o + 0x30)
            b = Bone()
            b.parent = s16(d, o) + s16(d, o + 8) if s16(d, o + 8) != 0 else -1
            b.scale = [s32(d, o + 0x10 + k * 4) / 4096.0 for k in range(3)]
            b.rot = [s16(d, o + 0x1c + k * 2) for k in range(3)]
            b.trans = [s32(d, o + 0x24 + k * 4) / 4096.0 for k in range(3)]
            b.mats = d[u32(d, o + 0x34):u32(d, o + 0x34) + n]
            b.dls = d[u32(d, o + 0x38):u32(d, o + 0x38) + n]
            self.bones.append(b)
        self.dls = []
        for g in range(u32(d, 0x0c)):
            rec = u32(d, 0x10) + g * 8
            for k in range(u32(d, rec)):
                s = u32(d, rec + 4) + k * 0x10
                self.dls.append((d[u32(d, s + 4):u32(d, s + 4) + u32(d, s)],
                                 d[u32(d, s + 0xc):u32(d, s + 0xc) + u32(d, s + 8)]))
        self.textures = []
        for i in range(u32(d, 0x14)):
            o = u32(d, 0x18) + i * 0x14
            p = u32(d, o + 0x10)
            at, size = u32(d, o + 4), u32(d, o + 8)
            self.textures.append({'name': cname(d, u32(d, o)), 'data': d[at:at + size], 'extra': d[at + size:],
                                  'param': p, 'fmt': (p >> 26) & 7, 'w': 8 << ((p >> 20) & 7),
                                  'h': 8 << ((p >> 23) & 7)})
        self.palettes = []
        for i in range(u32(d, 0x1c)):
            o = u32(d, 0x20) + i * 0x10
            at, size = u32(d, o + 4), u32(d, o + 8)
            self.palettes.append([rgb555(u16(d, at + k * 2)) for k in range(size // 2)])
        self.materials = []
        for i in range(u32(d, 0x24)):
            o = u32(d, 0x28) + i * 0x30
            self.materials.append({'name': cname(d, u32(d, o)), 'tex': s32(d, o + 4), 'pal': s32(d, o + 8),
                                   'words': [u32(d, o + 0x0c + k * 4) for k in range(9)]})
        self.tmap = [u16(d, u32(d, 0x2c) + i * 2) for i in range(32)]

    def rgba(self, ti, pi):
        """A texture as RGBA bytes, top row first (port/ntr/texture.cpp)."""
        t = self.textures[ti]
        pal = self.palettes[pi] if pi >= 0 else []
        transparent0 = (t['param'] >> 29) & 1
        w, h, d = t['w'], t['h'], t['data']
        o = bytearray(w * h * 4)
        c = lambda i: pal[i] if i < len(pal) else (0, 0, 0)

        def put(px, rgb, a):
            o[px * 4:px * 4 + 4] = bytes((rgb[0], rgb[1], rgb[2], a))
        if t['fmt'] == 5:
            bw = w // 4
            for blk in range(bw * (h // 4)):
                texels = u32(d, blk * 4)
                info = u16(t['extra'], blk * 2)
                b, mode = (info & 0x3fff) * 2, info >> 14
                c0, c1 = c(b), c(b + 1)
                if mode == 0:
                    four = [c0, c1, c(b + 2), None]
                elif mode == 1:
                    four = [c0, c1, blend(c0, c1, 1, 1), None]
                elif mode == 2:
                    four = [c0, c1, c(b + 2), c(b + 3)]
                else:
                    four = [c0, c1, blend(c0, c1, 5, 3), blend(c0, c1, 3, 5)]
                for k in range(16):
                    p = four[(texels >> (k * 2)) & 3]
                    if p is not None:
                        put(((blk // bw) * 4 + k // 4) * w + (blk % bw) * 4 + k % 4, p, 255)
            return bytes(o)
        for i in range(w * h):
            f = t['fmt']
            if f == 1:
                put(i, c(d[i] & 31), (d[i] >> 5) * 255 // 7)
            elif f == 6:
                put(i, c(d[i] & 7), (d[i] >> 3) * 255 // 31)
            elif 2 <= f <= 4:
                nb = 2 if f == 2 else 4 if f == 3 else 8
                v = (d[i * nb // 8] >> ((i * nb) % 8)) & ((1 << nb) - 1)
                put(i, c(v), 0 if v == 0 and transparent0 else 255)
            elif f == 7:
                v = u16(d, i * 2)
                put(i, rgb555(v), 255 if v & 0x8000 else 0)
        return bytes(o)


def blend(a, b, wa, wb):
    return tuple((a[k] * wa + b[k] * wb) // (wa + wb) for k in range(3))


class Bca:
    """A BCA joint animation, sampled as func_0204547c does."""

    def __init__(self, raw):
        d = self.d = lz77(raw)
        self.frames = u16(d, 2)
        self.p_scale, self.p_rot, self.p_trans, self.tracks = u32(d, 8), u32(d, 0xc), u32(d, 0x10), u32(d, 0x14)

    def fix(self, pool, shift, flag, start, f):   # func_020457f0
        t = lambda k: s32(self.d, pool + (start + k) * 4)
        if flag == 0:
            return t(0)
        if shift == 0:
            return t(f)
        b = ((self.frames - 1) >> shift) << shift
        i = f >> shift
        if f >= b:
            return t(i + (f - b))
        rem = f - (i << shift)
        return t(i) if rem == 0 else (t(i) * ((1 << shift) - rem) + t(i + 1) * rem) >> shift

    def angle(self, shift, flag, start, f):   # func_020456a0, 12-bit angles
        t = lambda k: s16(self.d, self.p_rot + (start + k) * 2)
        if flag == 0:
            return t(0)
        if shift == 0:
            return t(f)
        i = f >> shift
        b = ((self.frames - 1) >> shift) << shift
        if f >= b:
            return t(i + (f - b))
        frac = f - (i << shift)
        lo, hi = t(i), t(i + 1)
        if frac == 0:
            return lo
        rng = (1 << shift) - frac
        if abs(lo - hi) >= 0x800:
            if lo > hi:
                hi += 0x1000
            else:
                lo += 0x1000
            return ((hi * frac + lo * rng) >> shift) & 0xfff
        return (lo * rng + hi * frac) >> shift

    def sample(self, bone, f):
        d, o = self.d, self.tracks + bone * 0x24
        sh = lambda k: d[o + k * 4]
        fl = lambda k: d[o + k * 4 + 1]
        st = lambda k: u16(d, o + k * 4 + 2)
        anyscale = any(st(k) != 0 or fl(k) != 0 for k in range(3))
        scale = [self.fix(self.p_scale, sh(k), fl(k), st(k), f) / 4096.0 if anyscale else 1.0 for k in range(3)]
        rot = [self.angle(sh(3 + k), fl(3 + k), st(3 + k), f) for k in range(3)]
        trans = [self.fix(self.p_trans, sh(6 + k), fl(6 + k), st(6 + k), f) / 4096.0 for k in range(3)]
        return scale, rot, trans


class Btp:
    """A BTP texture sequence: per material, its (first frame, texture, palette) keys."""

    def __init__(self, raw):
        d = lz77(raw)
        self.frames = u16(d, 0)
        texs = [cname(d, u32(d, u32(d, 4) + i * 8 + 4)) for i in range(u16(d, 2))]
        pals = [cname(d, u32(d, u32(d, 0xc) + i * 8 + 4)) for i in range(u16(d, 8))]
        p10, p14, p18 = u32(d, 0x10), u32(d, 0x14), u32(d, 0x18)
        self.order, self.seqs = [], {}
        for i in range(u16(d, 0x1c)):
            e = u32(d, 0x20) + i * 0xc
            cnt, off = u16(d, e + 8), u16(d, e + 0xa)
            keys = [(u16(d, p10 + (off + j) * 2), texs[u16(d, p14 + (off + j) * 2)],
                     pals[u16(d, p18 + (off + j) * 2)] if pals else None) for j in range(cnt)]
            nm = cname(d, u32(d, e + 4))
            self.order.append(nm)
            self.seqs[nm] = keys


PARAMS = {0x00: 0, 0x10: 1, 0x11: 0, 0x12: 1, 0x13: 1, 0x14: 1, 0x15: 0, 0x16: 16, 0x17: 12, 0x18: 16,
          0x19: 12, 0x1a: 9, 0x1b: 3, 0x1c: 3, 0x20: 1, 0x21: 1, 0x22: 1, 0x23: 2, 0x24: 1, 0x25: 1,
          0x26: 1, 0x27: 1, 0x28: 1, 0x29: 1, 0x2a: 1, 0x2b: 1, 0x30: 1, 0x31: 1, 0x32: 1, 0x33: 1,
          0x34: 32, 0x40: 1, 0x41: 0, 0x50: 1, 0x60: 1, 0x70: 3, 0x71: 2, 0x72: 1}


def display_list(data):
    """A display list's triangles: each vertex (pos, slot, normal, u, v)."""
    words = struct.unpack_from(f'<{len(data) // 4}I', data)
    pos, slot, prim, normal, u, v = [0.0, 0.0, 0.0], 0, -1, None, 0.0, 0.0
    tris, strip = [], []

    def emit():
        s = strip
        if prim == 0:
            for i in range(0, len(s) - 2, 3):
                tris.append((s[i], s[i + 1], s[i + 2]))
        elif prim == 1:
            for i in range(0, len(s) - 3, 4):
                tris.append((s[i], s[i + 1], s[i + 2]))
                tris.append((s[i], s[i + 2], s[i + 3]))
        elif prim == 2:
            for i in range(len(s) - 2):
                tris.append((s[i], s[i + 1], s[i + 2]) if i % 2 == 0 else (s[i + 1], s[i], s[i + 2]))
        elif prim == 3:
            for i in range(0, len(s) - 3, 2):
                tris.append((s[i], s[i + 1], s[i + 3]))
                tris.append((s[i], s[i + 3], s[i + 2]))
    w = 0
    while w < len(words):
        packed = words[w]
        w += 1
        for k in range(4):
            cmd = (packed >> (k * 8)) & 0xff
            if cmd not in PARAMS:
                raise BuildError('The face model has an unknown command.')
            n = PARAMS[cmd]
            p0 = words[w] if n > 0 and w < len(words) else 0
            p1 = words[w + 1] if n > 1 and w + 1 < len(words) else 0
            w += n
            vertex = False
            if cmd == 0x14:
                slot = p0 & 31
            elif cmd == 0x21:
                normal = (sx(p0, 10) / 512.0, sx(p0 >> 10, 10) / 512.0, sx(p0 >> 20, 10) / 512.0)
            elif cmd == 0x22:
                u, v = sx(p0, 16) / 16.0, sx(p0 >> 16, 16) / 16.0
            elif cmd == 0x23:
                pos = [sx(p0, 16) / 4096.0, sx(p0 >> 16, 16) / 4096.0, sx(p1, 16) / 4096.0]
                vertex = True
            elif cmd == 0x24:
                pos = [sx(p0, 10) / 64.0, sx(p0 >> 10, 10) / 64.0, sx(p0 >> 20, 10) / 64.0]
                vertex = True
            elif cmd == 0x25:
                pos[0], pos[1] = sx(p0, 16) / 4096.0, sx(p0 >> 16, 16) / 4096.0
                vertex = True
            elif cmd == 0x26:
                pos[0], pos[2] = sx(p0, 16) / 4096.0, sx(p0 >> 16, 16) / 4096.0
                vertex = True
            elif cmd == 0x27:
                pos[1], pos[2] = sx(p0, 16) / 4096.0, sx(p0 >> 16, 16) / 4096.0
                vertex = True
            elif cmd == 0x28:
                pos[0] += sx(p0, 10) / 32768.0
                pos[1] += sx(p0 >> 10, 10) / 32768.0
                pos[2] += sx(p0 >> 20, 10) / 32768.0
                vertex = True
            elif cmd == 0x40:
                if strip:
                    emit()
                strip = []
                prim = p0 & 3
            elif cmd == 0x41:
                emit()
                strip = []
            if vertex:
                strip.append((tuple(pos), slot, normal, u, v))
    if strip:
        emit()
    return tris


def lid(t):
    """The eyelid pictures kept: by the last digit, 1 and 2 show 0, 3 shows 4."""
    if not t:
        return t
    if t[-1] in '12':
        return t[:-1] + '0'
    if t[-1] == '3':
        return t[:-1] + '4'
    return t


class Part:
    def __init__(self):
        self.pos, self.nrm, self.uv, self.jnt, self.idx = [], [], [], [], []

    def stream(self, attr, dbl):
        o = bytearray()
        for i in range(len(self.pos)):
            if attr == 'jnt':
                o += struct.pack('<4h', self.jnt[i] + 1, 1, 1, 1)
            else:
                vals = self.pos[i] if attr == 'pos' else self.nrm[i] if attr == 'nrm' else self.uv[i]
                o += struct.pack(f'<{len(vals)}f', *vals)
        once = bytes(o)
        if not dbl:
            return once
        r = bytearray(once + once)
        if attr == 'nrm':
            for i in range(len(once) + 3, len(r), 4):
                r[i] ^= 0x80
        return bytes(r)

    def indices(self, dbl):
        o = struct.pack(f'<{len(self.idx)}H', *self.idx)
        if dbl:
            o += struct.pack(f'<{len(self.idx)}H', *[(i + len(self.pos)) & 0xFFFF for i in reversed(self.idx)])
        return o


class Model:
    """One BMD with its animation (face_gltf.Model)."""

    def __init__(self, bmd, bca, btp):
        self.bmd, self.bca, self.btp = bmd, bca, btp
        self.bind = []
        for b in bmd.bones:
            m = local(b.scale, b.rot, b.trans)
            self.bind.append(mul(m, self.bind[b.parent]) if b.parent >= 0 else m)
        self.prims = []
        self.blinks = {}
        self._geometry()
        if btp is not None:
            self._blinks()

    def _geometry(self):
        s = float(1 << self.bmd.scale_shift)
        root = self.bmd.bones[0]
        for pi in range(len(root.mats)):
            mi = root.mats[pi]
            tf, data = self.bmd.dls[root.dls[pi]]
            mat = self.bmd.materials[mi]
            sw = shh = 1.0
            if mat['tex'] >= 0:
                sw, shh = float(self.bmd.textures[mat['tex']]['w']), float(self.bmd.textures[mat['tex']]['h'])
            env = (mat['words'][5] >> 30) == 2
            tris = []
            for tri in display_list(data):
                vs = []
                for vpos, vslot, vnorm, vu, vv in tri:
                    bone = self.bmd.tmap[tf[vslot]]
                    w = self.bind[bone]
                    pos = apply([vpos[0] * s, vpos[1] * s, vpos[2] * s, 1.0], w, 4)
                    n = apply(list(vnorm) if vnorm is not None else [0.0, 1.0, 0.0], w, 3)
                    ln = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
                    if ln == 0:
                        ln = 1.0
                    n = [n[0] / ln, n[1] / ln, n[2] / ln]
                    uu, vvv = vu / sw, vv / shh
                    if env:
                        uu, vvv = 0.5 + 0.5 * n[0], 0.5 - 0.5 * n[1]
                    raw = (tuple(bits(x) for x in vpos), bone,
                           tuple(bits(x) for x in vnorm) if vnorm is not None else None, bits(vu), bits(vv))
                    vs.append({'pos': pos, 'nrm': n, 'u': uu, 'v': vvv, 'bone': bone, 'raw': raw})
                tris.append(vs)
            self.prims.append((mi, tris))

    def _blinks(self):
        for mat in self.btp.order:
            keys = self.btp.seqs[mat]
            kept = sorted({k[1] for k in keys if lid(k[1]) == k[1]})
            shown = []
            for f in range(self.btp.frames):
                cur = None
                for k in keys:
                    if k[0] <= f:
                        cur = k
                t = lid(cur[1])
                shown.append(kept.index(t) if t in kept else -1)
            self.blinks[mat] = (kept, shown)

    def part(self, pi, variant, root):
        mi, tris = self.prims[pi]
        is_lid = root or self.bmd.materials[mi]['name'] in self.blinks
        inv0 = invert(self.bind[0])
        used = sorted({v['bone'] for t in tris for v in t})
        part, seen = Part(), {}
        for t in tris:
            for v in t:
                joint = 0 if is_lid else used.index(v['bone'])
                key = (v['raw'], joint)
                at = seen.get(key)
                if at is None:
                    at = seen[key] = len(part.pos)
                    if is_lid:
                        part.pos.append(apply(v['pos'] + [1.0], inv0, 4))
                        part.nrm.append(apply(v['nrm'], inv0, 3))
                    else:
                        part.pos.append(v['pos'])
                        part.nrm.append(v['nrm'])
                    part.uv.append([v['u'], v['v']])
                    part.jnt.append(joint)
                part.idx.append(at)
        return part

    def bone_curves(self, keys):
        o = {}
        n = self.bca.frames
        for bone in range(len(self.bmd.bones)):
            vals = [[0.0] * len(keys) for _ in range(9)]
            for i, (a, b, t) in enumerate(keys):
                sa, ra, ta = self.bca.sample(bone, a % n)
                sb, rb, tb = self.bca.sample(bone, b % n)
                for ax in range(3):
                    y = rb[ax] + 4096 * round((ra[ax] - rb[ax]) / 4096.0)
                    vals[ax][i] = sa[ax] * (1 - t) + sb[ax] * t
                    vals[3 + ax][i] = (ra[ax] * (1 - t) + y * t) * TURN
                    vals[6 + ax][i] = ta[ax] * (1 - t) + tb[ax] * t
            for ax in range(3):
                for i in range(1, len(keys)):
                    vals[3 + ax][i] += 2 * math.pi * round((vals[3 + ax][i - 1] - vals[3 + ax][i]) / (2 * math.pi))
            for c, ch in enumerate(('scale', 'rot', 'pos')):
                for ax in range(3):
                    o[f'{bone}/{ch}/{ax}'] = vals[c * 3 + ax]
        return o

    def lid_curves(self, keys):
        o = {}
        inv0 = invert(self.bind[0])

        def mean(which):
            s, count = [0.0, 0.0, 0.0], 0
            for mi, tris in self.prims:
                if not which(mi):
                    continue
                for t in tris:
                    for v in t:
                        lp = apply(v['pos'] + [1.0], inv0, 4)
                        for k in range(3):
                            s[k] += lp[k]
                        count += 1
            return [s[0] / count, s[1] / count, s[2] / count]
        centre = mean(lambda mi: True)
        for name, (textures, shown) in self.blinks.items():
            mid = mean(lambda mi: self.bmd.materials[mi]['name'] == name)
            side = name[-1:]
            for variant in range(len(textures)):
                pos = [[0.0] * len(keys) for _ in range(3)]
                scale = [0.0] * len(keys)
                for i, (a, b, t) in enumerate(keys):
                    on = shown[(a if t < 0.5 else b) % len(shown)] == variant
                    for ax in range(3):
                        pos[ax][i] = 0.0 if on else centre[ax] - 0.5 * mid[ax]
                    scale[i] = 1.0 if on else 0.5
                for ax in range(3):
                    o[f'{side}/{variant}/pos/{ax}'] = pos[ax]
                    o[f'{side}/{variant}/scale/{ax}'] = scale
        return o


def window(start, end, blend_frames):
    """The game frames the loop plays: (frame a, frame b, weight of b)."""
    w = []
    for f in range(start, end):
        k = f - (end - blend_frames)
        w.append((f, start - blend_frames + k, (k + 1) / blend_frames) if k >= 0 else (f, f, 0.0))
    w.append((start, start, 0.0))
    return w


def swizzle(rgba, w, h, fmt):
    """RGBA (top row first) as a 3DS texture: bottom row first, in 8x8 tiles of
    Morton-ordered pixels; format 3 RGB565, 2 RGBA5551."""
    o = bytearray(w * h * 2)
    at = 0
    morton = [0] * 64
    for y in range(8):
        for x in range(8):
            m = 0
            for bit in range(3):
                m |= ((x >> bit) & 1) << (bit * 2) | ((y >> bit) & 1) << (bit * 2 + 1)
            morton[y * 8 + x] = m
    for ty in range(h // 8):
        for tx in range(w // 8):
            for y in range(8):
                for x in range(8):
                    src = ((h - 1 - (ty * 8 + y)) * w + tx * 8 + x) * 4
                    r, g, b, a = rgba[src:src + 4]
                    px = ((b >> 3) | ((g & 0xfc) << 3) | ((r & 0xf8) << 8) if fmt == 3 else
                          (a >> 7) | ((b & 0xf8) >> 2) | ((g & 0xf8) << 3) | ((r & 0xf8) << 8))
                    m = morton[y * 8 + x]
                    o[at + m * 2] = px & 0xff
                    o[at + m * 2 + 1] = px >> 8
            at += 128
    return bytes(o)


def centred(logo, w, h):
    """The logo, its light-blue halo dropped, centred in a w x h RGBA texture."""
    pw, ph, px = logo
    drop = [False] * (pw * ph)
    for y in range(ph):
        for x in range(pw):
            r, g, b, a = px[y * pw + x]
            if a == 0 or max(r, g, b) < 100 or b <= r + 8:
                continue
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                if nx < 0 or ny < 0 or nx >= pw or ny >= ph or px[ny * pw + nx][3] == 0:
                    drop[y * pw + x] = True
                    break
    o = bytearray(w * h * 4)
    x0, y0 = (w - pw) // 2, (h - ph) // 2
    for y in range(ph):
        for x in range(pw):
            X, Y = x0 + x, y0 + y
            if X < 0 or Y < 0 or X >= w or Y >= h or drop[y * pw + x]:
                continue
            at = (Y * w + X) * 4
            o[at:at + 4] = bytes(px[y * pw + x])
    return bytes(o)


def face_banner(dsmt, template, recipe, logo, log):
    """The 3D banner (a CGFX): face.template with the values the recipe says
    filled in from the dump's own files; None if they aren't the recipe's."""
    files, models = {}, {}
    start = end = blend_frames = -1
    rows = [line.split('\t') for line in recipe if line]
    for f in rows:
        if f[0] == 'file':
            data = dsmt.get(f[1])
            if data is None:
                log(f'Banner: no {f[1]} in the extracted files.')
                return None
            if len(data) != int(f[2]) or sha256(data) != f[3]:
                log(f'Banner: {f[1]} is not the one the banner was made from.')
                return None
            files[f[1]] = data
        elif f[0] == 'model':
            if f[2] not in files or f[3] not in files or (f[4] != '-' and f[4] not in files):
                raise BuildError("The banner's recipe is damaged.")
            models[f[1]] = Model(Bmd(files[f[2]]), Bca(files[f[3]]), None if f[4] == '-' else Btp(files[f[4]]))
        elif f[0] == 'window':
            start, end, blend_frames = int(f[1]), int(f[2]), int(f[3])
    if not models or start < 0:
        raise BuildError("The banner's recipe is damaged.")
    keys = window(start, end, blend_frames)
    cgfx = bytearray(template)

    def put(at, data):
        if at < 0 or at + len(data) > len(cgfx):
            raise BuildError("The face banner's recipe is damaged.")
        cgfx[at:at + len(data)] = data

    def floats(at, step, vals):
        for i, x in enumerate(vals):
            put(at + i * step, struct.pack('<f', x))

    def curve(offs, key_list, vals):
        if key_list is None:
            for o in offs:
                floats(o, 4, [vals[0]])
            return
        v = [vals[int(k)] for k in key_list.split(',')]
        for o in offs:
            floats(o, 8, v)
    curves, lids = {}, None
    for f in rows:
        if f[0] in ('file', 'model', 'window') or f[0].startswith('sm64ds'):
            continue
        offs = [int(o) for o in f[1].split(',')]
        if f[0] == 'logo':
            if logo is not None:
                data = swizzle(centred(logo, int(f[3]), int(f[4])), int(f[3]), int(f[4]), int(f[2]))
                for o in offs:
                    put(o, data)
            continue
        if f[2] not in models:
            raise BuildError("The banner's recipe is damaged.")
        m = models[f[2]]
        if f[0] in ('vtx', 'idx'):
            part = m.part(int(f[3]), int(f[4]), f[-1] == 'root')
            data = part.indices(f[6] == '1') if f[0] == 'idx' else part.stream(f[5], f[7] == '1')
            for o in offs:
                put(o, data)
        elif f[0] == 'tex':
            ti, pi = int(f[3]), int(f[4])
            t = m.bmd.textures[ti]
            data = swizzle(m.bmd.rgba(ti, pi), t['w'], t['h'], int(f[5]))
            for o in offs:
                put(o, data)
        elif f[0] == 'rest':
            b = m.bmd.bones[int(f[3])]
            v = b.scale + [r * TURN for r in b.rot] + b.trans
            for o in offs:
                floats(o, 4, v)
        elif f[0] == 'ibm':
            inv = invert(m.bind[int(f[3])])
            v = [f32(inv[c * 4 + r]) for r in range(3) for c in range(4)]
            for o in offs:
                floats(o, 4, v)
        elif f[0] in ('const', 'keys'):
            if f[2] not in curves:
                curves[f[2]] = m.bone_curves(keys)
            curve(offs, f[6] if f[0] == 'keys' else None, curves[f[2]][f'{f[3]}/{f[4]}/{f[5]}'])
        elif f[0] in ('lidc', 'lidk'):
            if lids is None:
                lids = m.lid_curves(keys)
            curve(offs, f[7] if f[0] == 'lidk' else None, lids[f'{f[3]}/{f[4]}/{f[5]}/{f[6]}'])
        else:
            raise BuildError(f"The face banner's recipe is damaged ({f[0]}).")
    return bytes(cgfx)


# --- the icon, sound and banner formats (bannertool 1.2.3's) ----------------

def rgb565(px):
    """bannertool's colour: each channel times its alpha (a float), cut down."""
    r, g, b, a = px
    if a != 255:
        k = f32(a / 255.0)
        r, g, b = int(f32(r * k)) & 0xff, int(f32(g * k)) & 0xff, int(f32(b * k)) & 0xff
    return ((r & ~0x7) << 8) | ((g & ~0x3) << 3) | (b >> 3)


def tiles(px, w, h):
    out = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            i = (((y >> 3) * (w >> 3) + (x >> 3)) << 6) + ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                                                          ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3))
            out[i] = rgb565(px[y * w + x])
    return struct.pack(f'<{w * h}H', *out)


def smdh(icon, short, long_, publisher, slots=None):
    """bannertool makesmdh: every language slot the same titles, region free,
    visible + 3D + usage record, the 48x48 icon and a 24x24 of its 2x2 averages.
    slots: {language slot: (short, long)} where one differs (7 is Korean)."""
    def text(s, n):
        b = s.encode('utf-16-le')[:n * 2]
        return b + bytes(n * 2 - len(b))
    title = text(short, 0x40) + text(long_, 0x80) + text(publisher, 0x40)
    if slots:
        return b'SMDH' + struct.pack('<HH', 0, 0) + b''.join(
            text(slots[i][0], 0x40) + text(slots[i][1], 0x80) + text(publisher, 0x40) if i in slots else title
            for i in range(16)) + smdh(icon, short, long_, publisher)[8 + 0x200 * 16:]
    small = []
    for y in range(0, 48, 2):
        for x in range(0, 48, 2):
            four = [icon[(y + oy) * 48 + x + ox] for oy in (0, 1) for ox in (0, 1)]
            small.append(tuple(sum(p[k] for p in four) // 4 for k in range(4)))
    settings = bytes(16) + struct.pack('<IIQIHHII', 0x7FFFFFFF, 0, 0, 0x105, 0, 0, 0, 0)
    return b'SMDH' + struct.pack('<HH', 0, 0) + title * 16 + settings + bytes(8) + \
        tiles(small, 24, 24) + tiles(icon, 48, 48)


def wav_pcm(data):
    """(channels, rate, bits, interleaved samples) of a PCM WAV, as dr_wav reads it."""
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise BuildError('The banner sound (banner.wav) is not a WAV file.')
    at, fmt, pcm = 12, None, None
    while at + 8 <= len(data):
        cid, size = data[at:at + 4], u32(data, at + 4)
        body = data[at + 8:at + 8 + size]
        if cid == b'fmt ':
            fmt = body
        elif cid == b'data':
            pcm = body
        at += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        raise BuildError('The banner sound (banner.wav) is damaged.')
    tag, channels, rate = struct.unpack_from('<HHI', fmt, 0)
    block, bps = struct.unpack_from('<HH', fmt, 12)
    if tag == 0xFFFE and len(fmt) >= 26:
        tag = u16(fmt, 24)
    if tag != 1 or bps not in (8, 16):
        raise BuildError('The banner sound (banner.wav) has to be 8- or 16-bit PCM.')
    return channels, rate, bps, pcm[:len(pcm) // block * block]


def silence():
    """A short silent banner sound, for a builder-files without banner.wav."""
    rate, ch, n = 22050, 2, 22050 // 2
    size = n * ch * 2
    return (b'RIFF' + struct.pack('<I', 36 + size) + b'WAVEfmt ' +
            struct.pack('<IHHIIHH', 16, 1, ch, rate, rate * ch * 2, ch * 2, 16) +
            b'data' + struct.pack('<I', size) + bytes(size))


def cwav(wav):
    """bannertool's CWAV: PCM8/16, each channel's samples in a block of its own."""
    ch, rate, bps, pcm = wav_pcm(wav)
    width = bps // 8
    header_size = 64
    info_size = (32 + ch * (8 + 20) + 0x1F) & ~0x1F
    data_size = 32 + len(pcm)
    out = bytearray(header_size + info_size + data_size)
    struct.pack_into('<4sHHIIHH', out, 0, b'CWAV', 0xFEFF, header_size, 0x02010000, len(out), 2, 0)
    struct.pack_into('<HHII', out, 0x14, 0x7000, 0, header_size, info_size)
    struct.pack_into('<HHII', out, 0x20, 0x7001, 0, header_size + info_size, data_size)
    frames = len(pcm) // ch // width
    struct.pack_into('<4sIBBHIIII I', out, header_size, b'INFO', info_size, 1 if bps == 16 else 0, 0, 0,
                     rate, 0, frames, 0, ch)
    per = len(pcm) // ch
    for c in range(ch):
        struct.pack_into('<HHI', out, header_size + 32 + c * 8, 0x7100, 0, 4 + ch * 8 + c * 20)
        struct.pack_into('<HHIHHI', out, header_size + 32 + ch * 8 + c * 20, 0x1F00, 0, 24 + c * per,
                         0, 0, 0xFFFFFFFF)
    d = header_size + info_size
    struct.pack_into('<4sI', out, d, b'DATA', data_size)
    for c in range(ch):
        base = d + 8 + 24 + c * per
        if width == 2:
            samples = pcm[c * 2::ch * 2], pcm[c * 2 + 1::ch * 2]
            plane = bytearray(per)
            plane[0::2], plane[1::2] = samples
        else:
            plane = pcm[c::ch]
        out[base:base + per] = plane
    return bytes(out)


def lz11(data):
    """bannertool's LZ11, to the byte: at each point the longest match among
    the 4096 bytes before it (not the one just before), the farthest of equals."""
    n = len(data)
    out = bytearray(b'\x11' + struct.pack('<I', n)[:3])
    chains = {}
    flags_at, blocks = None, 8
    p = 0

    def common(q, limit):
        lo, hi = 0, limit
        while lo < hi:   # the longest prefix data[q:] and data[p:] share, up to limit
            mid = (lo + hi + 1) // 2
            if data[q:q + mid] == data[p:p + mid]:
                lo = mid
            else:
                hi = mid - 1
        return lo
    while p < n:
        if blocks == 8:
            flags_at, blocks = len(out), 0
            out.append(0)
        limit = min(n - p, 0x10110)
        best = disp = 0
        if limit >= 3 and p >= 2:
            low = p - min(p, 0x1000)
            for q in chains.get(data[p:p + 3], ()):
                if q < low or q > p - 2:
                    continue
                length = common(q, limit)
                if length > best:
                    best, disp = length, p - q
                    if best == limit:
                        break
        if best < 3:
            out.append(data[p])
            step = 1
        else:
            out[flags_at] |= 1 << (7 - blocks)
            if best > 0x110:
                v = best - 0x111
                out += bytes((0x10 | (v >> 12) & 0x0F, (v >> 4) & 0xFF, (v << 4) & 0xF0 | ((disp - 1) >> 8) & 0x0F,
                              (disp - 1) & 0xFF))
            elif best > 0x10:
                v = best - 0x11
                out += bytes(((v >> 4) & 0x0F, (v << 4) & 0xF0 | ((disp - 1) >> 8) & 0x0F, (disp - 1) & 0xFF))
            else:
                out += bytes((((best - 1) << 4) & 0xF0 | ((disp - 1) >> 8) & 0x0F, (disp - 1) & 0xFF))
            step = best
        for k in range(p, p + step):
            if k + 3 <= n:
                lst = chains.setdefault(data[k:k + 3], [])
                lst.append(k)
                if len(lst) > 8192 and lst[0] < k - 0x1000:
                    del lst[:len(lst) - 4096]
        p += step
        blocks += 1
    return bytes(out + bytes(-len(out) & 3))


def bnr(cgfx, wav):
    """bannertool makebanner -ci: CBMD, the CGFX in LZ11, 16-aligned, then the CWAV."""
    packed = lz11(cgfx)
    size = 0x88 + len(packed)
    cw_at = (size + 0xF) & ~0xF
    head = b'CBMD' + struct.pack('<I', 0) + struct.pack('<I', 0x88) + bytes(13 * 4 + 0x44) + struct.pack('<I', cw_at)
    return head + packed + bytes(cw_at - size) + cwav(wav)


# --- makerom ---------------------------------------------------------------

def makerom():
    """builder-files' makerom for this computer, runnable."""
    system, machine = platform.system(), platform.machine().lower()
    if system == 'Windows':
        name = 'makerom.exe'
    elif system == 'Linux' and machine in ('x86_64', 'amd64'):
        name = os.path.join('makerom', 'linux-x86_64', 'makerom')
    elif system == 'Darwin':
        name = os.path.join('makerom', 'macos-arm64' if machine in ('arm64', 'aarch64') else 'macos-x86_64',
                            'makerom')
    else:
        raise BuildError(f'There is no makerom here for {system} on {machine}: builder-files has them for '
                         'Windows, Linux (x86-64) and macOS. Build makerom 0.19.0 (github.com/3DSGuy/Project_CTR) '
                         'and pass it with --makerom PATH.')
    p = kit(name)
    if system != 'Windows':
        mode = os.stat(p).st_mode
        if not mode & stat.S_IXUSR:
            os.chmod(p, mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        if system == 'Darwin':   # a download's quarantine keeps macOS from running it
            subprocess.run(['xattr', '-d', 'com.apple.quarantine', p], stderr=subprocess.DEVNULL)
    return p


# --- the build -------------------------------------------------------------

def build(rom, output, options, log, progress, keep=False, tool=None, second=None, language=None):
    """second: a second dump (ASMJ, ASMK or ASME), for that cartridge's edition;
    language: the edition's language (LANGUAGES), the second dump's if None."""
    rom, output = os.path.abspath(rom), os.path.abspath(output)
    if not os.path.isfile(os.path.join(KIT, 'LABEL.txt')):
        kit('LABEL.txt')
    try:
        with open(rom, 'rb') as f:
            rb = f.read()
    except OSError as e:
        raise BuildError(f'The dump could not be read: {e.strerror}.')
    if len(rb) < 0x200:
        raise BuildError('This file is too small to be a DS dump.')
    code = rb[12:16].decode('ascii', 'replace')
    if not code.startswith('ASM'):
        raise BuildError(f"This is not a Super Mario 64 DS dump (its game code is {code.replace(chr(0), ' ').strip()}).")
    # the American or Japanese cartridge alone: its own edition (standalone_extract)
    alone = code in REGION_DUMPS and not second
    if alone:
        if language not in (None, REGION_DUMPS[code]):
            raise BuildError(f'The {REGION[code]} cartridge alone makes the {LANGUAGES[REGION_DUMPS[code]][0]} '
                             f'edition. For the {LANGUAGES[language][0]} one, give a European dump (ASMP) first '
                             f'and the {REGION[code]} one second.' if language in LANGUAGES and
                             LANGUAGES[language][2] == code else
                             f'The {REGION[code]} cartridge alone makes the {LANGUAGES[REGION_DUMPS[code]][0]} '
                             f'edition; the {LANGUAGES.get(language, (language,))[0]} game is made from a European '
                             'dump (ASMP).')
        language = REGION_DUMPS[code]
    elif code in DUMP_LANGUAGE:
        name = LANGUAGES[DUMP_LANGUAGE[code]][0]
        raise BuildError(f'This is the {REGION[code]} Super Mario 64 DS ({code}). The {name} edition of {TITLE} is '
                         'built from it together with a dump of the European cartridge (ASMP), which has the game the '
                         f'port runs: give the European dump first and the {REGION[code]} one second.')
    if code != 'ASMP' and not alone:
        raise BuildError(f'This is Super Mario 64 DS, but not the European version (game code {code}). {TITLE} is '
                         "built from the European cartridge (ASMP); its data does not come out of another region's.")
    sb = None
    if second:
        try:
            with open(os.path.abspath(second), 'rb') as f:
                sb = f.read()
        except OSError as e:
            raise BuildError(f'The second dump could not be read: {e.strerror}.')
        have = sb[12:16].decode('ascii', 'replace') if len(sb) >= 16 else ''
        if have not in DUMP_LANGUAGE:
            raise BuildError(f'The second dump is not the Japanese (ASMJ), Korean (ASMK) or American (ASME) Super '
                             f"Mario 64 DS (its game code is {re.sub('[^ -~]', '.', have) or 'missing'}).")
        if language is None:
            language = DUMP_LANGUAGE[have]
    if language is not None and language not in LANGUAGES:
        raise BuildError(f"There is no language '{language}'. The languages are: {', '.join(LANGUAGES)}.")
    if language is not None and not alone:
        need = LANGUAGES[language][2]
        if need and (sb is None or sb[12:16] != need.encode()):
            raise BuildError(f'The {LANGUAGES[language][0]} edition is built from the European dump and a dump of '
                             f'the {REGION[need]} cartridge ({need}): give that one second.')
        if not need and sb is not None:
            raise BuildError(f'The {LANGUAGES[language][0]} game comes out of the European dump alone; the second '
                             "dump would change it, so leave it out (or don't give --language).")
    tool = tool or makerom()
    tmp = tempfile.mkdtemp(prefix='SM64DSEnhancedBuilder-')
    try:
        progress(1, 'Starting...')
        log('Extracting the game data from your dump...')
        sub = lambda s: log('  ' + s)
        if alone:
            log(f'Making the European game out of your {REGION[code]} dump (it has no European dump beside it)...')
            files, assets = standalone_extract(rb, kit('romdata.recipe.tsv'), kit('romdata.manifest'), sub,
                                               lambda pct, what: progress(2 + pct * 68 // 100, what))
            sb = rb
        else:
            files, assets = extract(rb, kit('romdata.recipe.tsv'), kit('romdata.manifest'), sub,
                                    lambda pct, what: progress(2 + pct * 68 // 100, what))
        # the HOME Menu banner is made from the European files (its recipe is theirs)
        dsmt = {p[len('data/DSMT/'):]: b for p, b in files.items() if p.startswith('data/DSMT/')}
        if language == 'ko':
            progress(71, 'Reading the Korean text...')
            log('Extracting the Korean text from your Korean dump...')
            korean_apply(files, assets, korean_extract(sb, sub), sub)
        elif language in ('ja', 'en-us'):
            name = LANGUAGES[language][0]
            progress(71, f'Reading the {name} text...')
            log(f'Extracting the {name} text from your {REGION[LANGUAGES[language][2]]} dump...')
            region_apply(files, assets, region_extract(sb, language, sub), language, sub)
        elif language is not None:
            assets['langpack.bin'] = langpack(language)
            log(f'The game runs in {LANGUAGES[language][0]}.')

        progress(72, 'Packing the game files...')
        romfs = os.path.join(tmp, 'romfs')
        adir = os.path.join(romfs, 'build', 'assets')
        os.makedirs(adir)
        names = ('files.tsv', 'handles.tsv', 'nitrofs.tsv', 'nitrofs_fat.bin', 'nitrofs_fnt.bin',
                 'nitrofs_ovt9.bin', 'romdata.bin', 'romdata.manifest')
        if language:
            names += ('langpack.bin',) + (('korean.bin',) if language == 'ko' else ())
        for name in names:
            if name not in assets:
                raise BuildError(f'The extraction did not produce {name}. Try again; if it happens again, the dump '
                                 'may be damaged.')
            with open(os.path.join(adir, name), 'wb') as f:
                f.write(assets[name])
        dsd = os.path.join(romfs, 'extracted', 'dsd')
        os.makedirs(dsd)
        with open(os.path.join(dsd, 'header.yaml'), 'wb') as f:
            f.write((f"title: {rb[0:12].decode('ascii', 'replace').rstrip(chr(0))}\n"
                     f"gamecode: {code}\nnormal_cmd_setting: {u32(rb, 0x60)}\n").encode())
        with open(os.path.join(romfs, 'extracted', 'files.pak'), 'wb') as f:
            f.write(pack(files))
        log(f"Packed {len([p for p in files if p != 'files.txt'])} game files.")
        with open(os.path.join(romfs, 'options.txt'), 'wb') as f:
            f.write(options.file_text().encode())
        log(f'Settings: {options.summary()}.')

        progress(80, 'Drawing the HOME Menu icon and banner...')
        art = os.path.join(tmp, 'art')
        os.makedirs(art)
        icon = icon48(ds_icon(rb))
        logo = title_logo(rb)
        if logo is None:
            log("Couldn't read the title logo.")
        with open(kit('face.template'), 'rb') as f:
            template = f.read()
        with open(kit('face.recipe'), encoding='utf-8') as f:
            recipe = f.read().splitlines()
        model = face_banner(dsmt, template, recipe, logo, log)
        if model is None:
            raise BuildError("The dump's files aren't the ones the HOME Menu banner is made from.")
        log(f'Made the 3D banner ({len(model) // 1024} KB).')
        sound = os.path.join(KIT, 'banner.wav')
        if os.path.isfile(sound):
            with open(sound, 'rb') as f:
                wav = f.read()
        else:
            wav = silence()
        with open(os.path.join(art, 'banner.cgfx'), 'wb') as f:
            f.write(model)
        with open(os.path.join(art, 'icon.smdh'), 'wb') as f:
            f.write(smdh(icon, edition_title(language), edition_title(language), AUTHOR, edition_slots(language)))
        with open(os.path.join(art, 'banner.bnr'), 'wb') as f:
            f.write(bnr(model, wav))

        progress(86, 'Building the .cia...')
        made = os.path.join(tmp, 'out.cia')
        r = subprocess.run([tool, '-f', 'cia', '-o', made, '-elf', kit('game3ds.elf'), '-rsf', kit('game3ds.rsf'),
                            '-icon', os.path.join(art, 'icon.smdh'), '-banner', os.path.join(art, 'banner.bnr'),
                            '-DROMFS_DIR=' + romfs, '-target', 't', '-exefslogo'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        if r.returncode != 0:
            for line in r.stdout.splitlines():
                log('  ' + line)
            raise BuildError(f'Stopped while building the .cia (makerom exit code {r.returncode}).')
        if not os.path.isfile(made) or os.path.getsize(made) < 1 << 20:
            raise BuildError('makerom did not write the .cia.')
        os.makedirs(os.path.dirname(output), exist_ok=True)
        shutil.move(made, output)
        log(f'Wrote {output} ({os.path.getsize(output) >> 20} MB).')
        progress(100, 'Done.')
    finally:
        if keep:
            log('Kept the work folder: ' + tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


# --- the self-test ---------------------------------------------------------

def self_test_pieces():
    """name -> bytes, each a part of the build that needs no dump, from fixed
    inputs: the same on every computer, or that computer's .cia would differ."""
    with open(kit('face.template'), 'rb') as f:
        template = f.read()
    wav = silence()
    if os.path.isfile(os.path.join(KIT, 'banner.wav')):
        with open(os.path.join(KIT, 'banner.wav'), 'rb') as f:
            wav = f.read()
    ds = [(0, 0, 0, 0) if (x * 7 + y * 3) % 11 == 0 else ((x * 8) & 0xf8, (y * 8) & 0xf8, ((x ^ y) * 8) & 0xf8, 255)
          for y in range(32) for x in range(32)]
    rgba = b''.join(bytes(((x * 4 + y) & 0xff, (y * 4) & 0xff, (x * y) & 0xff, 255 if (x + y) % 3 else 0))
                    for y in range(64) for x in range(64))
    files = {f'data/{i % 7}/file{i}.bin': bytes((i * k) & 0xff for k in range(i * 13 % 97)) for i in range(60)}
    return {
        'sin4096': struct.pack('<8192d', *[sin4096(a) for a in range(-4096, 4096)]),
        'lz11': lz11(template),
        'cwav': cwav(wav),
        'smdh': smdh(icon48(ds), TITLE, TITLE, AUTHOR),
        'pack': pack(files),
        'swizzle': swizzle(rgba, 64, 64, 2) + swizzle(rgba, 64, 64, 3),
    }


def self_test(write):
    """Check this computer makes the pieces exactly as the Windows build that
    wrote builder-files/selftest.txt did, then build a .cia with its makerom
    (the game, no game data: it only shows makerom runs here)."""
    want_path = os.path.join(KIT, 'selftest.txt')
    pieces = self_test_pieces()
    got = {name: sha256(data) for name, data in pieces.items()}
    if write:
        with open(want_path, 'w', encoding='utf-8', newline='\n') as f:
            f.writelines(f'{name} {h}\n' for name, h in got.items())
        print(f'wrote {want_path}')
        return 0
    with open(want_path, encoding='utf-8') as f:
        want = dict(line.split() for line in f if line.strip())
    bad = 0
    for name, h in got.items():
        ok = want.get(name) == h
        bad += not ok
        print(f"  {name:8s} {'same' if ok else 'DIFFERENT'}")
    tools = [makerom()]
    if platform.system() == 'Darwin' and platform.machine() == 'arm64':   # the Intel one too, under Rosetta
        other = os.path.join(KIT, 'makerom', 'macos-x86_64', 'makerom')
        if os.path.isfile(other) and subprocess.run(['arch', '-x86_64', '/usr/bin/true'],
                                                    stderr=subprocess.DEVNULL).returncode == 0:
            os.chmod(other, os.stat(other).st_mode | stat.S_IXUSR)
            subprocess.run(['xattr', '-d', 'com.apple.quarantine', other], stderr=subprocess.DEVNULL)
            tools.append(['arch', '-x86_64', other])
    with tempfile.TemporaryDirectory() as tmp:
        romfs = os.path.join(tmp, 'romfs')
        os.makedirs(romfs)
        with open(os.path.join(romfs, 'options.txt'), 'w') as f:
            f.write(Options().file_text())
        with open(kit('face.template'), 'rb') as f:
            banner = bnr(f.read(), silence())
        for name, data in (('icon.smdh', pieces['smdh']), ('banner.bnr', banner)):
            with open(os.path.join(tmp, name), 'wb') as f:
                f.write(data)
        for tool in tools:
            cmd = tool if isinstance(tool, list) else [tool]
            out = os.path.join(tmp, 'test.cia')
            r = subprocess.run(cmd + ['-f', 'cia', '-o', out, '-elf', kit('game3ds.elf'), '-rsf', kit('game3ds.rsf'),
                                      '-icon', os.path.join(tmp, 'icon.smdh'), '-banner', os.path.join(tmp, 'banner.bnr'),
                                      '-DROMFS_DIR=' + romfs, '-target', 't', '-exefslogo'],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
            ok = r.returncode == 0 and os.path.isfile(out) and os.path.getsize(out) > 1 << 20
            bad += not ok
            print(f"  makerom  {' '.join(cmd[:-1] + [os.path.relpath(cmd[-1], HERE)])}: "
                  f"{'built a .cia' if ok else 'FAILED (exit %d) %s' % (r.returncode, r.stdout[-500:])}")
            if os.path.isfile(out):
                os.remove(out)
    print(f"self-test on {platform.system()} {platform.machine()}, Python {platform.python_version()}: "
          f"{'PASS' if not bad else 'FAIL'}")
    return 1 if bad else 0


def dump_code(path):
    """A dump's game code, or '' if it can't be read."""
    try:
        with open(path, 'rb') as f:
            return f.read(0x10)[0x0C:0x10].decode('ascii', 'replace')
    except OSError:
        return ''


def main():
    if '--self-test' in sys.argv[1:] or '--self-test-write' in sys.argv[1:]:
        return self_test('--self-test-write' in sys.argv[1:])
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rom', nargs='?', help='your Super Mario 64 DS dump (.nds, the European cartridge)')
    ap.add_argument('second', nargs='?', help='for the Japanese, Korean or American English edition: your dump of '
                    'that cartridge too')
    ap.add_argument('--language', choices=list(LANGUAGES),
                    help="the game's language (default: English, or the second dump's)")
    ap.add_argument('-o', '--out', help='where the .cia goes (default: next to the dump)')
    ap.add_argument('--option', action='append', default=[], metavar='KEY=VALUE')
    ap.add_argument('--keep', action='store_true', help='keep the work folder (its path is printed)')
    ap.add_argument('--log', help='also write the lines to this file')
    ap.add_argument('--makerom', help="a makerom of your own (0.19.0), for a computer builder-files has none for")
    a = ap.parse_args()

    print(f'{TITLE} Builder, {label()}.')
    rom = a.rom
    if not rom:
        print('Drag your Super Mario 64 DS dump (.nds) into this window, or type its path, then press Enter:')
        try:
            rom = input('> ').strip().strip('"').strip("'")
        except EOFError:
            rom = ''
        if not rom:
            return 2
        rom = os.path.expanduser(rom.replace('\\ ', ' '))
    second = a.second
    # the two dumps in either order
    if second and dump_code(rom) in DUMP_LANGUAGE and dump_code(second) == 'ASMP':
        rom, second = second, rom
    # the American or Japanese cartridge alone makes its own edition; the
    # Korean one needs the European dump beside it
    if not second and dump_code(rom) in DUMP_LANGUAGE and dump_code(rom) not in REGION_DUMPS:
        code = dump_code(rom)
        print(f'That is the {REGION[code]} cartridge. The {LANGUAGES[DUMP_LANGUAGE[code]][0]} edition needs your '
              'European dump (.nds) too: drag it in, then press Enter:')
        try:
            other = input('> ').strip().strip('"').strip("'")
        except EOFError:
            other = ''
        if not other:
            return 2
        second, rom = rom, os.path.expanduser(other.replace('\\ ', ' '))
    language = a.language or (DUMP_LANGUAGE.get(dump_code(second)) if second else REGION_DUMPS.get(dump_code(rom)))
    options = Options()
    for line in a.option:
        if not options.set(line):
            print(f'Not a setting: {line}', file=sys.stderr)
            return 2
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(rom)), edition_title(language) + '.cia')
    logf = open(a.log, 'w', encoding='utf-8') if a.log else None
    clock = time.time()

    def log(s):
        print(s)
        if logf:
            logf.write(s + '\n')

    def progress(pct, what):
        if what:
            log(f'[{pct}%] {time.time() - clock:.1f} s  {what}')
    try:
        build(rom, out, options, log, progress, a.keep, a.makerom, second, a.language)
        print('Copy the .cia to your SD card and install it with FBI.')
        return 0
    except BuildError as e:
        log(str(e))
        return 1
    finally:
        if logf:
            logf.close()


if __name__ == '__main__':
    sys.exit(main())
