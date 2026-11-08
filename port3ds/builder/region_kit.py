#!/usr/bin/env python3
"""Make builder-files/region-ASME.tsv (or region-ASMJ.tsv): how the builder
makes the European game's data out of the American (or Japanese) cartridge
alone (sm64ds_builder.py's standalone_extract has the format).

  py -I port3ds/builder/region_kit.py EUROPE.nds USA.nds [--out FILE]

It writes port-0.9.1/build/assets/region-ASME.tsv (or -ASMJ), beside the
romdata recipe and manifest it belongs with, where package.py takes it from.

It needs all three: the European dump (the layout romdata.bin and the file
system have), the other cartridge, and the upstream checkout's romdata recipe
and manifest (port-0.9.1/build/assets, the same build's as game3ds.elf) and
its symbols and relocations (port-0.9.1/config/arm9). Run it again after
every upstream update, as tools/pcprobe.sh is.

romdata.bin, piece by piece, is
  - the cartridge's bytes where the two programs line up (each module aligned
    word by word, then what didn't line up looked for anywhere in it);
  - the cartridge's NITRO files (pictures, palettes, screens, cells) for the
    European ones of the same kind, in order, over every piece they span;
  - the cartridge's own table where its program points as the European one
    does, when it is the same size (or says its size and fits);
  - the European pointers where the European program has a relocation;
  - the European file numbers (handles and archive members) where the
    cartridge's bytes are its own numbers for the same files;
  - the European names (file paths) where the cartridge has none;
  - the European numbers of a small table (positions, sprite shapes, file
    numbers) the cartridge has none of, or has in another layout;
  - and nothing (zero) for the rest: the four other languages' text and
    pictures, which the English slot never reads.
None of the cartridge's content goes in the kit (only where it is, and
hashes), and of the European program only its pointers, file numbers, names
and those small tables of numbers: never text, pictures or sound.
"""
import argparse
import bisect
import collections
import difflib
import hashlib
import os
from pathlib import Path
import re
import struct
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sm64ds_builder as B  # noqa: E402

ROOT = HERE.parents[1]
UP = ROOT / 'port-0.9.1'
CFG = UP / 'config' / 'arm9'
K = 6   # the alignment's anchor, in words
LANGS = ('_F_', '_G_', '_I_', '_S_', '/FRA/', '/GER/', '/ITA/', '/SPA/', '_F.', '_G.', '_I.', '_S.',
         '_frn.', '_gmn.', '_itl.', '_spn.', 'cef.narc', 'ceg.narc', 'cei.narc', 'ces.narc', '_FGIS_')


def sha(b):
    return hashlib.sha256(b).hexdigest()


# --- the European program's symbols and relocations -------------------------

def relocs(module):
    path = CFG / 'relocs.txt' if module == 'arm9' else CFG / 'overlays' / module / 'relocs.txt'
    out = []
    for line in open(path):
        m = re.match(r'from:0x([0-9a-f]+) kind:(\S+) to:0x([0-9a-f]+) module:(\S+)', line)
        if m:
            out.append((int(m.group(1), 16), m.group(2), int(m.group(3), 16)))
    return out


def all_relocs(modules):
    out = {}
    for m in modules:
        try:
            out[m] = relocs(m)
        except FileNotFoundError:
            out[m] = []
    return out


# --- the alignment -----------------------------------------------------------

def words(b):
    return list(struct.unpack_from('<%dI' % (len(b) // 4), b, 0))


def lis(pairs):
    tails_j, tails, prev = [], [], [None] * len(pairs)
    for k, (i, j) in enumerate(pairs):
        p = bisect.bisect_left(tails_j, j)
        if p == len(tails_j):
            tails_j.append(j)
            tails.append(k)
        else:
            tails_j[p], tails[p] = j, k
        prev[k] = tails[p - 1] if p else None
    out, k = [], tails[-1] if tails else None
    while k is not None:
        out.append(pairs[k])
        k = prev[k]
    return out[::-1]


def align(E, R, reloc_words):
    """emap[i]: the region word European word i lines up with (-1: none)."""
    et, rt = words(E), words(R)
    n, m = len(et), len(rt)
    tok = [(-(i + 2) if i in reloc_words else w) for i, w in enumerate(et)]
    rk = collections.defaultdict(list)
    for j in range(m - K + 1):
        rk[tuple(rt[j:j + K])].append(j)
    anchors = []
    for i in range(n - K + 1):
        key = tuple(tok[i:i + K])
        if min(key) < 0 or len(set(key)) < 3:
            continue
        lst = rk.get(key)
        if lst and len(lst) == 1:
            anchors.append((i, lst[0]))
    chain = lis(anchors)
    emap = [-1] * n
    for i, j in chain:
        for k in range(K):
            emap[i + k] = j + k

    def eq(i, j):
        return 0 <= i < n and 0 <= j < m and (tok[i] == rt[j] or tok[i] < 0)
    for i, j in chain:
        a, b = i + K, j + K
        while eq(a, b) and emap[a] == -1:
            emap[a] = b
            a, b = a + 1, b + 1
        a, b = i - 1, j - 1
        while eq(a, b) and emap[a] == -1:
            emap[a] = b
            a, b = a - 1, b - 1
    i = 0
    while i < n:
        if emap[i] != -1:
            i += 1
            continue
        s = i
        while i < n and emap[i] == -1:
            i += 1
        lo = emap[s - 1] + 1 if s > 0 else 0
        hi = emap[i] if i < n else m
        if hi <= lo:
            continue
        sm = difflib.SequenceMatcher(None, tok[s:i], rt[lo:hi], autojunk=False)
        for op, a1, a2, b1, b2 in sm.get_opcodes():
            if op == 'equal' or (op == 'replace' and a2 - a1 == b2 - b1):
                for k in range(a2 - a1):
                    emap[s + a1 + k] = lo + b1 + k
    return emap


def find(img, seg, mask, hint):
    """Where in img seg is (mask's bytes), 4-aligned, the nearest hint."""
    best = (0, 0)
    cur = st = 0
    for k in range(len(seg)):
        if mask[k]:
            if cur == 0:
                st = k
            cur += 1
            if cur > best[1]:
                best = (st, cur)
        else:
            cur = 0
    a0, n = best
    if n < 4:
        return None
    anc = seg[a0:a0 + n]
    hits, p = [], img.find(anc)
    while p != -1 and len(hits) < 64:
        s = p - a0
        if s >= 0 and s % 4 == 0 and s + len(seg) <= len(img):
            t = img[s:s + len(seg)]
            if all((t[i] ^ seg[i]) & mask[i] == 0 for i in range(len(seg))):
                hits.append(s)
        p = img.find(anc, p + 1)
    if not hits:
        return None
    return min(hits, key=lambda s: abs(s - hint)) if hint is not None else hits[0]


# --- the cartridges ----------------------------------------------------------

class Rom:
    def __init__(self, path):
        self.rb = open(path, 'rb').read()
        self._members = {}
        self.code = self.rb[12:16].decode()
        self.mods = B.region_modules(self.rb)
        fat, table, size = B.u32(self.rb, 0x48), B.u32(self.rb, 0x50), B.u32(self.rb, 0x54)
        self.ram = {}
        for i in range(size // 32):
            e = table + i * 32
            self.ram[f'ov{B.u32(self.rb, e):03d}'] = B.u32(self.rb, e + 4)
        self.ram['arm9'] = B.u32(self.rb, 0x28)
        self.dirs = B.fnt_dirs(self.rb)
        self.files = {p: B.rb_file(self.rb, i) for i, p in B.fnt_walk(self.dirs)}
        self.leaves = {}
        for p, b in self.files.items():
            if p.startswith('ARCHIVE/'):
                names, _ = B.narc_members(b)
                data = B.narc_files(b)
                for n, i in names.items():
                    self.leaves[p + ':' + n] = data[i]
            else:
                self.leaves[p] = b

    def handles(self):
        """The handle table's paths (overlay 0's table of path pointers)."""
        o, base = self.mods['ov000'], self.ram['ov000']
        if self.code == 'ASMP':
            at, n = B.HANDLE_TABLE_ADDRESS - base, B.HANDLE_COUNT
        else:
            def is_path(v):
                return base <= v < base + len(o) and o[v - base:v - base + 3] in (b'dat', b'MG/')
            s = o.index(b'data/')
            p = None
            for q in range(0, len(o) - 4, 4):
                if is_path(B.u32(o, q)) and is_path(B.u32(o, q + 4)) and is_path(B.u32(o, q + 8)):
                    p = q
                    break
            at, n = p, 0
            while is_path(B.u32(o, at + n * 4)):
                n += 1
        out = []
        for h in range(n):
            q = B.u32(o, at + h * 4) - base
            out.append(o[q:o.index(b'\0', q)].decode('ascii'))
        return out

    def archives(self):
        """[(first id, end id, path)] of the arm9's archive table."""
        a, out = self.mods['arm9'], []
        for m in re.finditer(rb'/ARCHIVE/(\w+)\.narc\0', a):
            ptr = struct.pack('<I', m.start() + 0x02004000)
            for mm in re.finditer(re.escape(ptr), a):
                w = B.u32(a, mm.start() - 8)
                out.append((w & 0xFFFF, w >> 16, f'ARCHIVE/{m.group(1).decode()}.narc'))
        return sorted(set(out))

    def members(self, narc):
        if narc not in self._members:
            names, _ = B.narc_members(self.files[narc])
            self._members[narc] = sorted(names, key=lambda n: names[n])
        return self._members[narc]


def raw(b):
    try:
        if b[:4] == b'LZ77':
            return B.lz77(b)
        if len(b) > 4 and b[0] == 0x10:
            r = B.lz77(b'LZ77' + b)
            if len(r) == B.u32(b, 0) >> 8:
                return r
    except Exception:
        pass
    return b


def lz_kind(b):
    if b[:4] == b'LZ77':
        return 'lz77', len(B.lz77(b))
    if len(b) > 4 and b[0] == 0x10:
        try:
            r = B.lz77(b'LZ77' + b)
            if len(r) == B.u32(b, 0) >> 8:
                return 'lz10', len(r)
        except Exception:
            pass
    return 'raw', len(b)


def norm(path):
    """A file's name with its region's marks taken out, to pair the European
    English files with the cartridge's."""
    p = path.split(':')[-1]
    d, n = os.path.split(p)
    d = re.sub(r'/(EUR|USA|JPN)(/ENG)?$', '', d)
    n = re.sub(r'_(E|EU|U|US|P|EUR|J|JP)(?=[_.]|$)', '', n)
    n = re.sub(r'_(US_)?PAL(?=[_.]|$)', '', n)
    n = re.sub(r'msg_data_(eng|nes)', 'msg_data', n)
    return d + '/' + n


# --- the kit -----------------------------------------------------------------

LEVEL = {f'ov{i:03d}' for i in range(9, 57)}   # the level overlays, all at one address
SMALL = 256   # a table this size or less the cartridge has nothing for keeps the European numbers
OWN = 4096    # nor one the cartridge has in another size (its layout another)


def fits(e, r):
    """Whether the cartridge's table r (from its start) fits the European one's
    room e, for those whose size is in their header (LZ, message banks); None
    if not known."""
    if e[:1] == b'\x10' and r[:1] == b'\x10':
        n = lz10_length(r)
        return None if n is None else n <= len(e)
    if e[:8] == b'GSEM1gmb' and r[:8] == b'GSEM1gmb':
        return B.u32(r, 8) <= len(e)
    if len(e) >= 16 and e[:4] == r[:4] and e[4:6] == b'\xff\xfe' and r[4:6] == b'\xff\xfe':
        return B.u32(r, 8) <= len(e)   # a NITRO file (NCCG, NCCL, NCSC, NCOB...): its size
    return None


def own_ok(e):
    """Whether a European table may keep its own numbers: not text, not
    compressed, not a picture (those are the game's content)."""
    if e[:8] == b'GSEM1gmb' or e[:4] in (b'NCCG', b'NCSC', b'NCCL', b'NCGR', b'NSCR', b'NCLR'):
        return False
    if e[:1] == b'\x10' and len(e) > 8 and len(e) // 2 < B.u32(e, 0) >> 8 < len(e) * 16:
        n = lz10_length(e)
        if n is not None and len(e) - 4 <= n <= len(e):
            return False
    text = sum(1 for c in e if 32 <= c < 127)
    return text < len(e) * 0.6


def lz10_length(b):
    """How many bytes an LZ10 stream takes (None: not one)."""
    try:
        size, i, done = B.u32(b, 0) >> 8, 4, 0
        while done < size:
            flags = b[i]
            i += 1
            for bit in range(8):
                if done >= size:
                    break
                if flags & 0x80 >> bit:
                    done += (b[i] >> 4) + 3
                    i += 2
                else:
                    done += 1
                    i += 1
        return i
    except IndexError:
        return None


def romdata(eu, rg, recipe, names, log):
    """The r/w/b lines of romdata.bin, and the blob they make; names: the
    manifest's symbol at each blob offset."""
    rows = [(int(f[0]), int(f[1]), f[2].lower(), int(f[3])) for f in recipe]
    total = max(o + n for o, n, _, _ in rows)
    mods = sorted(set(r[2] for r in rows))
    rel = all_relocs(mods)
    # every relocation's target, by its module and address (the level overlays
    # share one address)
    eu_val = {}
    for mod in mods:
        for frm, kind_, to in rel[mod]:
            if kind_ == 'load':
                eu_val[(mod, frm)] = to
    # handle and archive-member numbers, the cartridge's to the European
    he, hr = eu.handles(), rg.handles()
    eh = {}
    for i, p in enumerate(he):
        eh.setdefault(p, i)
    ae, ar = eu.archives(), rg.archives()
    ename = {}
    for lo, hi, p in ae:
        for i, n in enumerate(eu.members(p)):
            ename[f'{p}:{n}'] = lo + i

    def pick(leaf, cands):
        cands = sorted(cands, key=lambda k: (any(x in k for x in LANGS), k))
        return cands[0] if cands else None
    by_norm = collections.defaultdict(list)   # (the names as they are: norm reads their case)
    for k in list(eh) + list(ename):
        by_norm[norm(k).lower()].append(k)

    def eu_handle(v):
        if v >= len(hr):
            return None
        p = hr[v]
        if p in eh:
            return eh[p]
        c = pick(p, [k for k in by_norm.get(norm(p).lower(), []) if k in eh])
        if c is None:   # a file the European cartridge has in an archive
            c = pick(p, [k for k in ename if k.split(':', 1)[1] == p])
            return ename[c] if c else None
        return eh[c]

    def eu_member(v):
        for lo, hi, p in ar:
            if lo <= v < hi:
                names = rg.members(p)
                if v - lo >= len(names):
                    return None
                leaf = f'{p}:{names[v - lo]}'
                if leaf in ename:
                    return ename[leaf]
                c = pick(leaf, [k for k in by_norm.get(norm(leaf).lower(), []) if k in ename])
                if c is None:   # the same member path in another archive
                    c = pick(leaf, [k for k in ename if k.split(':')[1] == names[v - lo]])
                if c is None:   # a file the European cartridge has on its own (its handle)
                    return eh.get(names[v - lo])
                return ename[c]
        return None

    def eu_number(v):
        return eu_handle(v) if v < 0x8000 else eu_member(v)

    blob = bytearray(total)
    src = [None] * total   # ('r', mod, off) | ('w',) | ('b',) | ('z',) | None
    stats = collections.Counter()
    emaps = {}
    in_nitro = set()   # the pieces in a NITRO file (content: never the European numbers)
    for mod in sorted(set(eu.mods) & set(rg.mods)):
        E, R = eu.mods[mod], rg.mods[mod]
        base = eu.ram[mod]
        if mod not in rel:
            try:
                rel[mod] = relocs(mod)
            except FileNotFoundError:
                rel[mod] = []
        rw = set((f - base) // 4 for f, _, _ in rel[mod] if base <= f < base + len(E))
        log(f'  aligning {mod} ({len(E)} bytes)')
        emaps[mod] = align(E, R, rw)

    def numbers(mrows, E, R):
        """The European numbers where the cartridge has its own for the same files."""
        for off, n, _, soff in mrows:
            for k in range(0, n - 1, 2):
                if (soff + k) % 2:
                    continue
                s0, s1 = src[off + k], src[off + k + 1]
                if not (s0 and s1 and s0[0] == 'r' and s1[0] == 'r' and s1[2] == s0[2] + 1):
                    continue
                ev = B.u16(E, soff + k)
                rv = B.u16(R, s0[2])
                if ev != rv and eu_number(rv) == ev:
                    struct.pack_into('<H', blob, off + k, ev)
                    src[off + k] = src[off + k + 1] = ('w',)
                    stats['number'] += 2
    # the European pointers, by the module each points into
    targets = collections.defaultdict(list)   # module: [(target, from module, from)]
    for fmod in rel:
        for frm, kind_, to in rel[fmod]:
            if kind_ != 'load':
                continue
            for tmod in mods:
                if eu.ram[tmod] <= to < eu.ram[tmod] + len(eu.mods[tmod]) and \
                        (tmod == fmod or not (tmod in LEVEL and fmod in LEVEL)):
                    targets[tmod].append((to, fmod, frm))
    for mod in mods:
        E, R = eu.mods[mod], rg.mods[mod]
        base, rbase = eu.ram[mod], rg.ram[mod]
        emap = emaps[mod]
        mrows = [r for r in rows if r[2] == mod]
        for off, n, _, soff in mrows:
            for k in range(n):
                b = soff + k
                w = b // 4
                a = base + (b & ~3)
                if b >= len(E):   # past the module's end (its .bss): zero, as the recipe has it
                    src[off + k] = ('z',)
                    continue
                if (mod, a) in eu_val:
                    v = eu_val[(mod, a)]
                    if E[b] != v >> 8 * (b % 4) & 0xFF:
                        stats['pointer-mismatch'] += 1
                    blob[off + k] = v >> 8 * (b % 4) & 0xFF
                    src[off + k] = ('w',)
                    stats['pointer'] += 1
                    continue
                if src[off + k] is not None:
                    continue
                if w < len(emap) and emap[w] != -1:
                    j = emap[w] * 4 + b % 4
                    if j < len(R):
                        ev, rv = E[b], R[j]
                        src[off + k] = ('r', mod, j)
                        blob[off + k] = rv
                        stats['aligned' if ev == rv else 'changed'] += 1
        # what didn't line up: looked for anywhere in the module
        for off, n, _, soff in mrows:
            k = 0
            while k < n:
                if src[off + k] is not None:
                    k += 1
                    continue
                s = k
                while k < n and src[off + k] is None:
                    k += 1
                seg = E[soff + s:soff + k]
                if seg.count(0) == len(seg) or (soff + s) % 4:
                    continue
                mask = bytearray(b'\xff' * len(seg))
                hint = None
                for q in range(s - 1, -1, -1):
                    if src[off + q] and src[off + q][0] == 'r':
                        hint = src[off + q][2] + s - q
                        break
                at = find(R, seg, mask, hint)
                if at is not None:
                    for q in range(len(seg)):
                        src[off + s + q] = ('r', mod, at + q)
                        blob[off + s + q] = R[at + q]
                    stats['found'] += len(seg)
        # the NITRO files in the program (pictures, palettes, screens, cells:
        # each one or more of the recipe's pieces): the cartridge's of the same
        # kind in the same order, over all the pieces the European one spans,
        # and nothing past its end
        def nitro(img):
            out = collections.defaultdict(list)
            for m in re.finditer(rb'NC[A-Z]{2}\xff\xfe', img):
                size = B.u32(img, m.start() + 8)
                if m.start() + size <= len(img):
                    out[img[m.start():m.start() + 4]].append((m.start(), size))
            return out
        en, rn = nitro(E), nitro(R)

        def key(img, at, size):   # what makes two of a kind the same file: cells, or about the size
            if img[at:at + 4] == b'NCOB':
                c = img.find(b'CELL', at, at + size)
                return B.u32(img, c + 8) if c >= 0 else -1
            return size // 64
        for magic, el in en.items():
            rl = rn.get(magic, [])
            sm = difflib.SequenceMatcher(None, [key(E, a, n) for a, n in el], [key(R, a, n) for a, n in rl],
                                         autojunk=False)
            pairs = [(el[a + k], rl[b + k]) for a, b, n in sm.get_matching_blocks() for k in range(n)]
            for (es, esize), (rs, rsize) in pairs:
                if rsize > esize:
                    log(f"    a {magic.decode()} at {base + es:#x}: the cartridge's is bigger; left as it lines up")
                    continue
                for off, n, _, soff in mrows:
                    if soff + n <= es or soff >= es + esize:
                        continue
                    in_nitro.add(off)
                    for k in range(max(es, soff) - soff, min(es + esize, soff + n) - soff):
                        if src[off + k] == ('w',):
                            continue
                        q = soff + k - es
                        if q < rsize:
                            src[off + k] = ('r', mod, rs + q)
                            blob[off + k] = R[rs + q]
                        else:
                            src[off + k] = ('z',)
                            blob[off + k] = 0
                stats['nitro'] += esize
        # a table still missing some: the cartridge's own, where its program
        # points (the same place in it as the European pointer to the table)
        tgt = sorted(targets[mod])
        tkeys = [t[0] for t in tgt]
        found = []
        for off, n, _, soff in mrows:
            name = names.get(off, '')
            if name.startswith('port_') or n <= SMALL or off in in_nitro or \
                    not any(src[off + k] is None and E[soff + k] for k in range(n)):
                continue
            a0 = base + soff
            votes = collections.Counter()
            for i in range(bisect.bisect_left(tkeys, a0), bisect.bisect_left(tkeys, a0 + n)):
                to, fmod, frm = tgt[i]
                fb = eu.ram[fmod]
                fe = emaps.get(fmod)
                q = (frm - fb) // 4
                if fe is None or q >= len(fe) or fe[q] == -1:
                    continue
                wv = B.u32(rg.mods[fmod], fe[q] * 4) - (to - a0)
                if rbase <= wv < rbase + len(R):
                    votes[wv - rbase] += 1
            # a NITRO file (NCCG, NCSC...) in the program: the cartridge's of
            # the same kind in the same order
            if E[soff + 4:soff + 6] == b'\xff\xfe' and re.match(rb'[A-Z]{4}', E[soff:soff + 4]):
                el = [m.start() for m in re.finditer(re.escape(E[soff:soff + 4]) + rb'\xff\xfe', E)]
                rl = [m.start() for m in re.finditer(re.escape(E[soff:soff + 4]) + rb'\xff\xfe', R)]
                if len(el) == len(rl) and soff in el:
                    votes[rl[el.index(soff)]] += 1000
            # a message bank: the cartridge's one (its English or Japanese)
            if E[soff:soff + 8] == b'GSEM1gmb':
                rl = [m.start() for m in re.finditer(rb'GSEM1gmb', R)]
                if len(rl) == 1:
                    votes[rl[0]] += 1000
            if not votes:
                stats['missing'] += sum(1 for k in range(n) if src[off + k] is None and E[soff + k])
                continue
            found.append((off, n, soff, votes.most_common(1)[0][0]))
        # where the cartridge's tables start, as far as known: each ends where
        # the next begins
        starts = set(a for _, _, _, a in found)
        for off, n, _, soff in mrows:
            if src[off] and src[off][0] == 'r' and src[off][1] == mod:
                starts.add(src[off][2])
        starts = sorted(starts)
        for off, n, soff, at in found:
            name = names.get(off, '')
            fit = fits(E[soff:soff + n], R[at:])
            i = bisect.bisect_right(starts, at)
            size = min(starts[i] if i < len(starts) else len(R), len(R)) - at
            if fit or (fit is None and size == n):
                # the same size (or one that says its own): all of it but the European pointers
                for k in range(min(n, len(R) - at)):
                    if src[off + k] is None or src[off + k][0] == 'r':
                        src[off + k] = ('r', mod, at + k)
                        blob[off + k] = R[at + k]
                stats['counterpart'] += n
                continue
            # another size: the cartridge's is another layout of it (more cells,
            # fewer languages), which the European program can't read in this
            # room; a table of positions and shapes keeps the European numbers
            if n <= OWN and own_ok(E[soff:soff + n]) and off not in in_nitro:
                for k in range(n):
                    if src[off + k] != ('w',):
                        src[off + k] = ('b',)
                        blob[off + k] = E[soff + k]
                stats['own-table'] += n
                log(f"    {name}: {n} bytes, the cartridge's {size}: its own numbers")
            else:
                log(f"    {name}: {n} bytes, the cartridge's {size}: not the same, left as it lines up")
        numbers(mrows, E, R)
        # the names: strings the cartridge doesn't have
        for off, n, _, soff in mrows:
            k = 0
            while k < n:
                if src[off + k] is not None:
                    k += 1
                    continue
                s = k
                while k < n and src[off + k] is None:
                    k += 1
                seg = E[soff + s:soff + k]
                text = sum(1 for c in seg if 32 <= c < 127)
                if n <= SMALL and not names.get(off, '').startswith('port_') and own_ok(E[soff:soff + n]) and \
                        off not in in_nitro:
                    # a small table of the European program's own (screen
                    # positions, sprite shapes, file numbers): its numbers
                    blob[off + s:off + k] = seg
                    for q in range(s, k):
                        src[off + q] = ('b',)
                    stats['own-table'] += len(seg) - seg.count(0)
                elif text + seg.count(0) == len(seg) and text > len(seg) // 3:
                    for m in re.finditer(rb'[ -~]+', seg):
                        blob[off + s + m.start():off + s + m.end()] = m.group(0)
                        for q in range(m.start(), m.end()):
                            src[off + s + q] = ('b',)
                    stats['name'] += text
                else:
                    stats['empty'] += len(seg) - seg.count(0)
        # and the European names where the cartridge's line up with other names
        for off, n, _, soff in mrows:
            seg = E[soff:soff + n]
            for m in re.finditer(rb'[A-Za-z0-9_./%:-]{3,}\0', seg):
                a, z = off + m.start(), off + m.end()
                if blob[a:z] != m.group(0):
                    blob[a:z] = m.group(0)
                    for q in range(a, z):
                        src[q] = ('b',)
                    stats['name'] += z - a
    log('  ' + ', '.join(f'{v} {k}' for k, v in stats.items()))
    # the lines
    out = []
    k = 0
    while k < total:
        s = src[k]
        if s is None or s == ('z',):
            k += 1
            continue
        if s[0] == 'r':
            a = k
            while k < total and src[k] and src[k][0] == 'r' and src[k][1] == s[1] and src[k][2] == s[2] + k - a:
                k += 1
            out.append(f'r\t{a}\t{k - a}\t{s[1]}\t{s[2]}')
        elif s[0] == 'w':
            a = k
            while k < total and src[k] == ('w',):
                k += 1
            if (k - a) % 4 or a % 2:
                out.append(f'b\t{a}\t{blob[a:k].hex()}')
            else:
                ws = ' '.join(f'{B.u32(blob, q):x}' for q in range(a, k, 4))
                out.append(f'w\t{a}\t{ws}')
        else:
            a = k
            while k < total and src[k] == ('b',):
                k += 1
            out.append(f'b\t{a}\t{blob[a:k].hex()}')
    return out, bytes(blob)


def files_spec(eu, rg, log):
    """The D/F/A/N/M/T/P lines, and the cartridge's leaves they read (in order)."""
    rleaf_by_sha = collections.defaultdict(list)
    rleaf_by_raw = collections.defaultdict(list)
    for k, b in rg.leaves.items():
        rleaf_by_sha[sha(b)].append(k)
        rleaf_by_raw[sha(raw(b))].append(k)
    rnorm = collections.defaultdict(list)
    for k in rg.leaves:
        rnorm[norm(k).lower()].append(k)
    eleaves = eu.leaves
    english = {}
    for k in eleaves:
        english.setdefault(norm(k).lower(), []).append(k)

    def how_of(k):
        b = eleaves[k]
        path = k.split(':')[-1]
        if k in rg.leaves and rg.leaves[k] == b:
            return ('leaf', k)
        for pool in (rleaf_by_sha.get(sha(b), []), rleaf_by_raw.get(sha(raw(b)), [])):
            if pool:
                c = sorted(pool, key=lambda x: (x.split(':')[-1] != path, x))
                return ('leaf', c[0])
        if k in rg.leaves:
            return ('leaf', k)
        same = [x for x in rg.leaves if x.split(':')[-1] == path]
        if same:
            return ('leaf', same[0])
        if any(x in k for x in LANGS):
            # another language's: the English one's
            for sib in to_english(k):
                if sib in eleaves and not any(y in sib for y in LANGS):
                    return ('copy', sib)
            return ('empty', None)
        c = rnorm.get(norm(k).lower(), [])
        if c:
            kb = lz_kind(b)[1]
            c = sorted(c, key=lambda x: abs(lz_kind(rg.leaves[x])[1] - kb))
            return ('leaf', c[0])
        return ('empty', None)

    hows = {k: how_of(k) for k in eleaves}
    # a copy of a file that is itself a copy or empty: follow it
    for k, h in hows.items():
        while h[0] == 'copy' and hows[h[1]][0] == 'copy':
            h = hows[h[1]]
        hows[k] = h
    lines, used = [], []

    def how_text(k):
        h = hows[k]
        if k == 'data/DSMT/EUR/texBank_E.bmd':
            src = next(x for x in rg.leaves if re.match(r'data/DSMT/(USA|JPN)/texBank(_U)?\.bmd$', x))
            used.append(src)
            return f't\t{src}'
        if h[0] == 'leaf':
            used.append(h[1])
            if ':' in h[1]:
                narc, mem = h[1].split(':', 1)
                return f'm\t{narc}\t{mem}'
            return f'=\t{h[1]}'
        if h[0] == 'copy':
            return f'c\t{h[1]}'
        kind_, size = lz_kind(eleaves[k])
        return f'z\t{kind_}\t{size}'

    for first, parent, ents in eu.dirs:
        lines.append('\t'.join(['D', str(first), str(parent)] +
                               [f'd:{n}:{s}' if t == 'd' else f'f:{n}' for t, n, s in ents]))
    report = collections.Counter()
    for _, p in B.fnt_walk(eu.dirs):
        if p.startswith('ARCHIVE/') and not (p in rg.files and rg.files[p] == eu.files[p]):
            lines.append(f'F\t{p}\tA')
            names, s = B.narc_members(eu.files[p])
            body = eu.files[p][s[b'BTNF'] + 8:s[b'BTNF'] + B.u32(eu.files[p], s[b'BTNF'] + 4)]
            for first, parent, ents in btnf_dirs(body):
                lines.append('\t'.join(['N', p, str(first), str(parent)] +
                                       [f'd:{n}:{s}' if t == 'd' else f'f:{n}' for t, n, s in ents]))
            for n in eu.members(p):
                h = how_text(f'{p}:{n}')
                report[hows[f'{p}:{n}'][0]] += 1
                lines.append(f'M\t{p}\t{n}\t{h}')
            continue
        if p.startswith('ARCHIVE/'):
            used.append(p)
            lines.append(f'F\t{p}\t=\t{p}')
            report['leaf'] += 1
            continue
        report[hows[p][0]] += 1
        lines.append(f'F\t{p}\t{how_text(p)}')
    # the texture bank: the European one's textures, from the cartridge's
    e = B.lz77(eu.files['data/DSMT/EUR/texBank_E.bmd'])
    src = next(x for x in rg.leaves if re.match(r'data/DSMT/(USA|JPN)/texBank(_U)?\.bmd$', x))
    rt, rp = B.texbank_parts(rg.leaves[src])
    et, ep = B.texbank_parts(eu.files['data/DSMT/EUR/texBank_E.bmd'])
    rtn = {norm(n).lower(): n for n in rt}
    rpn = {norm(n).lower().replace('_pl', ''): n for n in rp}
    for i in range(B.u32(e, 0x14)):
        q = B.u32(e, 0x18) + i * 20
        n, at, size, w, h, params = struct.unpack_from('<IIIHHI', e, q)
        name = e[n:e.index(b'\0', n)].decode()
        c = rtn.get(norm(name).lower().replace('_eu', ''))
        ok = c and len(rt[c][3]) == size and rt[c][:2] == (w, h)
        lines.append(f'T\tdata/DSMT/EUR/texBank_E.bmd\t{name}\t{size}\t{w}\t{h}\t{params:x}\t{c if ok else "-"}')
        report['texture' if ok else 'texture-empty'] += 1
    for i in range(B.u32(e, 0x1C)):
        q = B.u32(e, 0x20) + i * 16
        n, at, size = struct.unpack_from('<III', e, q)
        name = e[n:e.index(b'\0', n)].decode()
        key = norm(name).lower().replace('_pl', '').replace('_eu', '')
        c = rpn.get(key) or rpn.get(key.replace('title_option', 'title_option'))
        if name == 'title_option_pl':
            c = next((x for x in rp if x.startswith('title_option')), None)
        ok = c and len(rp[c]) >= size
        lines.append(f'P\tdata/DSMT/EUR/texBank_E.bmd\t{name}\t{size}\t{c if ok else "-"}')
        report['palette' if ok else 'palette-empty'] += 1
    log('  files: ' + ', '.join(f'{v} {k}' for k, v in sorted(report.items())))
    return lines, used, hows


def to_english(k):
    """The European English file's names another language's file may have."""
    out = []
    for lang, dirn, arc, bank in (('F', 'FRA', 'cef', 'frn'), ('G', 'GER', 'ceg', 'gmn'),
                                  ('I', 'ITA', 'cei', 'itl'), ('S', 'SPA', 'ces', 'spn')):
        if f'_{lang}_' not in k and f'_{lang}.' not in k and f'/{dirn}/' not in k and arc not in k \
                and f'_{bank}.' not in k and '_FGIS_' not in k:
            continue
        e = k.replace(f'/{dirn}/', '/ENG/').replace(f'{arc}.narc', 'cee.narc').replace(f'_{bank}.', '_eng.')
        out += [e.replace(f'_{lang}_', '_E_').replace(f'_{lang}.', '_E.'),
                e.replace(f'_{lang}_', '_').replace(f'_{lang}.', '.'),
                e.replace(f'_{lang}_', '_EU_'), e.replace(f'_{lang}_', '_U_')]
    if '_FGIS_' in k:
        out.append(k.replace('_FGIS_', '_'))
    return out


def btnf_dirs(body):
    dirs = []
    for d in range(B.u16(body, 6)):
        off, first, parent = struct.unpack_from('<IHH', body, d * 8)
        ents, o = [], off
        while body[o]:
            t = body[o]
            name = body[o + 1:o + 1 + (t & 0x7F)].decode('ascii')
            o += 1 + (t & 0x7F)
            if t & 0x80:
                ents.append(('d', name, B.u16(body, o)))
                o += 2
            else:
                ents.append(('f', name, None))
        dirs.append((first, parent, ents))
    return dirs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('europe')
    ap.add_argument('region')
    ap.add_argument('--assets', type=Path, default=UP / 'build' / 'assets',
                    help='the romdata recipe and manifest (default: the upstream checkout\'s)')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--report', type=Path, help='also write which file each European file is made from')
    a = ap.parse_args()
    log = print
    eu, rg = Rom(a.europe), Rom(a.region)
    if eu.code != 'ASMP' or rg.code not in B.REGION_DUMPS:
        sys.exit('give the European dump, then the American or Japanese one')
    rec = (a.assets / 'romdata.recipe.tsv').read_text().splitlines()
    man = (a.assets / 'romdata.manifest').read_text().splitlines()
    m = re.match(r'# romdata-manifest v1 ([0-9a-f]{64}) (\d+)', man[0])
    layout_sha, total = m.group(1), int(m.group(2))
    recipe = [line.split('\t') for line in rec if line and not line.startswith('#')]
    log(f'{rg.code}: romdata.bin')
    names = {int(f[3]): f[2] for f in (line.split('\t') for line in man) if f[0] == 'sym'}
    rlines, blob = romdata(eu, rg, recipe, names, log)
    assert len(blob) == total
    log(f'{rg.code}: the file system')
    flines, used, hows = files_spec(eu, rg, log)
    h = hashlib.sha256()
    for k in used:
        h.update(hashlib.sha256(rg.leaves.get(k, rg.files.get(k))).digest())
    fnt_off, fnt_size, fat_off, fat_size, ovt_off, ovt_size = struct.unpack_from('<6I', rg.rb, 0x40)
    imgs = sorted(set(f.split('\t')[3] for f in rlines if f.startswith('r\t')))
    head = [f'# sm64ds-region v1: the European game out of {rg.code} alone (region_kit.py)',
            f'code\t{rg.code}', f'layout\t{layout_sha}\t{total}', f'blob\t{sha(blob)}', f'leaves\t{h.hexdigest()}',
            f'ovt\t{sha(rg.rb[ovt_off:ovt_off + ovt_size])}']
    head += [f'img\t{m}\t{len(rg.mods[m])}\t{sha(rg.mods[m])}' for m in imgs]
    out = a.out or a.assets / f'region-{rg.code}.tsv'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('\n'.join(head + rlines + flines) + '\n', encoding='utf-8')
    log(f'wrote {out} ({out.stat().st_size // 1024} KB)')
    if a.report:
        with open(a.report, 'w', encoding='utf-8') as f:
            for k in sorted(hows):
                f.write(f'{k}\t{hows[k][0]}\t{hows[k][1] or ""}\n')


if __name__ == '__main__':
    main()
