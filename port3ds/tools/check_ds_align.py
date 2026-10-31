"""Fail the build when a hosted DS global sits less aligned than on the DS.

The HAL lays DS globals out in grouped sections at the ROM's own spacing
(__declspec(align(1)), compat/include/msvc_declspec.h), so a group's address
mod 4 is whatever the section before it left. GCC still assumes the DS
alignment (a word field of a struct the DS keeps at a word address) and emits
strd / ldrd / ldm on it, which data-abort on hardware when the base is only 2
aligned. Azahar does not fault, so only a 3DS run ever showed it: the card
bring-up's strd into data_020a8180 (CARDiCommon) at 0x0094b1fa, after an
unrelated change moved the .dsstate run by two bytes.

A global named data_[ovNN_]XXXXXXXX, four bytes or more, whose DS address is
word aligned must be word aligned here too.

    python check_ds_align.py <nm> <elf>                 check (the build runs this)
    python check_ds_align.py <nm> <elf> --fix <rules>   also pin the offending
        groups: for each, its head (walking back while the spacing here equals
        the DS's) gets a "ds_head" rule in game/gccfix_rules.json, which
        tools/gccfix.py turns into a word alignment (a head at a word address)
        or a word-aligned pad of DS-address-mod-4 bytes in a section sorted
        just before the head's. Reconfigure, rebuild, repeat until clean: a
        pinned group stays right whatever moves in front of it.
"""
import json
import os
import re
import subprocess
import sys

NAME = re.compile(r'^data_(?:ov\d+_)?(02[0-9a-f]{6})$')


def symbols(nm, elf):
    out = subprocess.run([nm, '-n', '-S', elf], capture_output=True, text=True,
                         check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 4:
            continue
        m = NAME.match(parts[3])
        if m:
            syms.append((parts[3], int(parts[0], 16), int(parts[1], 16), int(m.group(1), 16)))
    return syms


def head_of(syms, i):
    """Walk back while the previous global sits as far before this one here
    as it does on the DS (one group laid out at the ROM's spacing)."""
    while i > 0:
        pn, ph, ps, pd = syms[i - 1]
        n, h, s, d = syms[i]
        if 0 < d - pd and h - ph == d - pd:
            i -= 1
        else:
            break
    return syms[i]


def find_def(root, name):
    # the section string and the name, possibly on the next line, with no
    # statement end between; not in a comment or a #pragma/#define line
    rx = re.compile(r'"\.dsstate\$[^"]*"[^;"]*?\b' + re.escape(name) + r'\b')
    for sub in ('port/hal', 'port/ntr'):
        base = os.path.join(root, sub)
        for dirpath, _, files in os.walk(base):
            for fn in files:
                if not fn.endswith(('.c', '.cpp')):
                    continue
                p = os.path.join(dirpath, fn)
                text = open(p, encoding='utf-8', errors='replace').read()
                for m in rx.finditer(text):
                    line = text[text.rfind('\n', 0, m.start()) + 1:m.start()].lstrip()
                    if line.startswith(('//', '*', '/*', '#')):
                        continue
                    return os.path.relpath(p, os.path.dirname(root)).replace('\\', '/')
    return None


def main():
    nm, elf = sys.argv[1], sys.argv[2]
    fix = sys.argv[4] if len(sys.argv) > 4 and sys.argv[3] == '--fix' else None
    syms = symbols(nm, elf)
    bad = [i for i, (n, h, s, d) in enumerate(syms) if s >= 4 and d % 4 == 0 and h % 4 != 0]
    if not bad:
        print('check_ds_align: %d DS globals, all word aligned where the DS has them so'
              % len(syms))
        return 0
    print('check_ds_align: %d of %d DS globals are not word aligned (the 3DS faults on '
          'strd/ldm there):' % (len(bad), len(syms)))
    for i in bad[:40]:
        print('  %s at %08x' % (syms[i][0], syms[i][1]))
    heads = []
    for i in bad:
        h = head_of(syms, i)
        if h[0] not in [x[0] for x in heads]:
            heads.append(h)
    print('group heads: ' + ', '.join('%s (DS %% 4 = %d)' % (n, d % 4) for n, _, _, d in heads))
    if not fix:
        print('Pin them: python port3ds/tools/check_ds_align.py <nm> <elf> --fix '
              'port3ds/game/gccfix_rules.json, then reconfigure and rebuild.')
        return 1
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        '..', '..', 'port-0.9.1'))
    raw = open(fix, encoding='utf-8', newline='').read()
    rules = json.loads(raw)
    added = 0
    for n, _, _, d in heads:
        f = find_def(root, n)
        if not f:
            print('  %s: no .dsstate definition found under port-0.9.1/port; fix by hand' % n)
            continue
        if any(r.get('action') == 'ds_head' and r.get('name') == n for r in rules):
            print('  %s: already pinned and still off; look at it by hand' % n)
            continue
        rules.append({'file': f, 'line': '', 'action': 'ds_head', 'name': n, 'pad': d % 4})
        added += 1
        print('  pinned %s in %s (pad %d)' % (n, f, d % 4))
    out = json.dumps(rules, indent=1, ensure_ascii=False) + ('\n' if raw.endswith('\n') else '')
    if '\r\n' in raw:
        out = out.replace('\n', '\r\n')
    open(fix, 'w', encoding='utf-8', newline='').write(out)
    print('%d rule(s) added; reconfigure and rebuild' % added)
    return 1


if __name__ == '__main__':
    sys.exit(main())
