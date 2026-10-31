#!/usr/bin/env python3
"""Export the PC port's game build as a CMake fragment for the 3DS build.

The PC CMake (port/CMakeLists.txt, ~21,000 lines) assembles the game
executable dynamically from 334 slice manifests, per-file generators and
per-file defines. Instead of re-deriving that, configure it once with the
3DS toolchain and a CMake File API query (port3ds/tools/pcprobe.sh does
both, plus the generators), then run this to turn the File API reply into
port3ds/game/manifest.cmake:

    set(GAME_SOURCES ...)                       every compile unit
    set_source_files_properties(... COMPILE_DEFINITIONS ... COMPILE_OPTIONS ...)

for the targets the game executable is made of (walk_window, its shared
slice library and the ntr tier it links). Paths are written relative to
${PORT_ROOT} and ${PC_PROBE} so the fragment does not depend on this machine.

MSVC-only flags are translated or dropped:
    /FI<h>   -> -include <h>        (forced include)
    /Zp4     -> -fpack-struct=4     (ROM struct packing)
    /vmg /vmm /Oy- /wdNNNN          dropped (MSVC pointer-to-member width,
                                     frame pointers, warnings)
Defines the 3DS build sets itself (PORT_REPO_ROOT, SM64DS_PLATFORM_PC) are
left out. Sources listed in port3ds/game/exclude.txt (one glob per line,
relative to PORT_ROOT or PC_PROBE) are omitted.

  pcmanifest.py --probe C:/sm64ds-3ds/build-pcprobe --port-root C:/sm64ds-3ds/port-0.9.1
                --out port3ds/game/manifest.cmake [--exclude port3ds/game/exclude.txt]
"""
import argparse
import fnmatch
import glob
import json
import os
import re
import sys

TARGETS = ('port_slice_shared', 'walk_window', 'ntr_wide_rt')
# The game's own per-target defines that decide which body sits in the
# Stage's vtable slots (hal/stage_bridges.cpp: slot 6 the ROM's
# Stage::Behavior, which pumps the messages, slot 9 its Render). The PC's
# tools/tierscan.py keeps the files that test them out of the shared slice;
# in the probe configure stage_bridges.cpp landed in it all the same, without
# them, and the game got the smoke harness's host stubs. Every unit gets them
# here (only the files that test them change).
GAME_DEFINES = ('SM64DS_STAGE_SLOT6_ROM=1', 'SM64DS_STAGE_SLOT9_ROM=1')
DROP_DEFINES = ('PORT_REPO_ROOT', 'SM64DS_PLATFORM_PC',
                # The PC's framebuffer tiers (ntr/include/ntr/ppu.h): the PC
                # compiles its ntr tier with one and the shared slice without,
                # which only holds together through its per-target tier split.
                # The 3DS builds every unit at the native 256x192 tier: one
                # struct size everywhere, and a wide-tier framebuffer is 4 MB.
                'NTR_WIDE_RT', 'NTR_HIRES', 'NTR_HIRES2', 'NTR_WIDE169')


def msys_to_win(p):
    m = re.match(r'^/([a-zA-Z])/(.*)$', p)
    return '%s:/%s' % (m.group(1).upper(), m.group(2)) if m else p


def cmake_quote(s):
    """A CMake quoted argument holding a ;-list (items escaped by game_only)."""
    return '"%s"' % s.replace('\\', '\\\\').replace('"', '\\"')


def game_only(item):
    """Source properties are directory-scoped: a smoke gate that compiles the
    same file would inherit the game's per-file defines. Each item applies
    only when the consuming target is ${GAME_OBJ_TARGET}."""
    v = item.replace('>', '$<ANGLE-R>').replace(',', '$<COMMA>').replace(';', '$<SEMICOLON>')
    return '$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,${GAME_OBJ_TARGET}>:%s>' % v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--probe', required=True)
    ap.add_argument('--port-root', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--exclude')
    a = ap.parse_args()

    probe = os.path.abspath(a.probe).replace('\\', '/')
    root = os.path.abspath(a.port_root).replace('\\', '/')
    excludes = []
    if a.exclude and os.path.exists(a.exclude):
        for line in open(a.exclude):
            line = line.split('#', 1)[0].strip()
            if line:
                excludes.append(line)

    replies = glob.glob(os.path.join(probe, '.cmake/api/v1/reply/codemodel-v2-*.json'))
    if not replies:
        sys.exit('no File API reply under %s (run port3ds/tools/pcprobe.sh)' % probe)
    cm = json.load(open(replies[0]))
    rdir = os.path.dirname(replies[0])
    by_name = {t['name']: t['jsonFile'] for t in cm['configurations'][0]['targets']}

    def rel(path):
        """('PORT_ROOT'|'PC_PROBE', relative path) or None."""
        p = msys_to_win(path.replace('\\', '/'))
        if not os.path.isabs(p):
            # The File API gives sources inside the source tree relative to it,
            # and the PC project's source tree is PORT_ROOT/port.
            p = os.path.join(root, 'port', p).replace('\\', '/')
        p = os.path.normpath(p).replace('\\', '/')
        for var, base in (('PC_PROBE', probe), ('PORT_ROOT', root)):
            if p.lower().startswith(base.lower() + '/'):
                return var, p[len(base) + 1:]
        return None

    units = []          # (var, relpath, lang, defines, options)
    seen = set()
    skipped = []
    for name in TARGETS:
        if name not in by_name:
            sys.exit('target %s not in the File API reply' % name)
        t = json.load(open(os.path.join(rdir, by_name[name])))
        group_of = {}
        for g in t.get('compileGroups', []):
            for i in g['sourceIndexes']:
                group_of[i] = g
        for i, s in enumerate(t['sources']):
            g = group_of.get(i)
            if g is None:
                continue                      # headers, objects, non-compiled
            r = rel(s['path'])
            if r is None:
                skipped.append(s['path'])
                continue
            var, rp = r
            if any(fnmatch.fnmatch(rp, pat) for pat in excludes):
                continue
            if (var, rp) in seen:
                continue
            seen.add((var, rp))
            defines = [d['define'] for d in g.get('defines', [])
                       if d['define'].split('=', 1)[0] not in DROP_DEFINES]
            names = {d.split('=', 1)[0] for d in defines}
            try:
                text = open(os.path.join(probe if var == 'PC_PROBE' else root, rp),
                            encoding='utf-8', errors='replace').read()
            except OSError:
                text = ''
            for gd in GAME_DEFINES:
                n = gd.split('=', 1)[0]
                if n not in names and n in text:
                    defines.append(gd)
            opts = []
            for f in g.get('compileCommandFragments', []):
                for tok in f['fragment'].split():
                    if tok.startswith('/FI'):
                        h = rel(tok[3:])
                        # port/unmatched/*_HostAbi.h shadow a header so MSVC's
                        # layout matches the ROM's (one destructor slot,
                        # reversed overloads, no tail-padding reuse,
                        # thiscall). GCC already lays classes out as the ROM
                        # does, so on the 3DS each one would move the ROM's
                        # vtable slots instead (dBgW's: slot 6, the ground
                        # probe, became GetTriangleOrigin). Kept: the fBase_c
                        # one, whose layout is GCC's own (the destructor at
                        # slots 16/17) and which is what makes its TU emit
                        # fBase_c's vtable (the ROM supplies it as data).
                        if h and re.match(r'port/unmatched/\w+HostAbi\.h$', h[1]) and \
                                not h[1].endswith('FBase_DtorDispatch_HostAbi.h'):
                            continue
                        opts += ['-include', ('${%s}/%s' % h) if h else msys_to_win(tok[3:])]
                    elif tok == '/Zp4':
                        opts.append('-fpack-struct=4')
            units.append((var, rp, g['language'], defines, opts))

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', newline='\n') as f:
        f.write('# GENERATED by port3ds/tools/pcmanifest.py from the PC port\'s CMake\n'
                '# (File API reply of a configure under the 3DS toolchain). Do not edit;\n'
                '# re-run port3ds/tools/pcprobe.sh after an upstream sync.\n'
                '# %d compile units from %s.\n\n' % (len(units), ', '.join(TARGETS)))
        f.write('set(GAME_SOURCES\n')
        for var, rp, *_ in units:
            f.write('    "${%s}/%s"\n' % (var, rp))
        f.write(')\n\n')
        for var, rp, lang, defines, opts in units:
            if not defines and not opts:
                continue
            f.write('set_source_files_properties("${%s}/%s" PROPERTIES' % (var, rp))
            if defines:
                f.write('\n    COMPILE_DEFINITIONS %s' % cmake_quote(';'.join(game_only(d) for d in defines)))
            if opts:
                f.write('\n    COMPILE_OPTIONS %s' % cmake_quote(';'.join(game_only(o) for o in opts)))
            f.write(')\n')
    langs = {}
    for u in units:
        langs[u[2]] = langs.get(u[2], 0) + 1
    print('pcmanifest: %d units (%s), %d outside both roots skipped -> %s'
          % (len(units), ', '.join('%s %d' % kv for kv in sorted(langs.items())),
             len(skipped), a.out))
    for s in skipped[:10]:
        print('  skipped:', s)
    return 0


if __name__ == '__main__':
    sys.exit(main())
