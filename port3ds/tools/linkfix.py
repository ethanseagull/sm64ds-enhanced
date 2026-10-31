#!/usr/bin/env python3
"""Pre-link fixups that make the PC port's MSVC link model work under GNU ld.

The PC port (port/, built with 32-bit MSVC) leans on two things the GNU
toolchain does not do the same way:

1. TWO NAME SPACES. MSVC decorates C++ names its own way (?Fn@Cls@@QAEXXZ),
   so a HAL file can define a forwarder under the game's flat Itanium name
   (extern "C" _ZN3Cls2FnEv) right beside the real C++ method in src/ without
   a clash. Under GCC the two ARE the same symbol, so the link reports a
   multiple definition. Rule here: the game's own code (src/, port/unmatched)
   wins. Each HAL/host copy of a symbol that game code also defines strongly
   is made weak in place (objcopy -W).

2. /alternatename. `#pragma comment(linker, "/alternatename:A=B")` means
   "resolve A to B if nothing defines A". The PC HAL carries ~4,200 of these
   and GCC ignores the pragma. GNU ld's PROVIDE(A = B) in a linker script has
   the same meaning. This tool scans the rows, translates both names into the
   GCC (Itanium/ELF) spelling, and writes PROVIDE lines for every symbol the
   link would otherwise leave undefined.

   Translation:
     __Zxxx           -> _Zxxx              (x86 cdecl underscore dropped)
     _name            -> name
     ?name@@3...      -> name               (global data: Itanium does not mangle it)
     ?name@A@B@@3...  -> _ZN1B1A4nameE      (scoped data)
     ??_7Cls@@6B@     -> _ZTV3Cls, at -8    (vtable: MSVC names the address
                                             point, Itanium the table start)
     ?fn@Cls@@<sig>   -> matched by qualified name against the undefined
                         Itanium symbols (demangled with c++filt), only when
                         the match is unique.

Usage:
  linkfix.py --out aliases.ld --alias-src DIR [--alias-src DIR...]
             --tools C:/devkitPro/devkitARM/bin  OBJ...
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from collections import defaultdict

ROW = re.compile(r'/alternatename:([^=\s"]+)=([^\s"]+)')

PORT_ROOT_NAME = 'port-0.9.1'   # set from --port-root-name


def set_port_root_name(name):
    global PORT_ROOT_NAME
    PORT_ROOT_NAME = name


def is_game_object(path):
    """Built from the decomp's src/ or port/unmatched/ (the ROM's own code),
    as opposed to the PC HAL or this port's own sources. CMake mirrors an
    out-of-tree source's absolute path under the object dir, so the upstream
    checkout's directory name survives in the object path."""
    p = path.replace('\\', '/')
    root = '/%s/' % PORT_ROOT_NAME
    return ((root + 'src/') in p or (root + 'port/unmatched/') in p
            or is_hostgen_object(p) or '/gccfix/src/' in p)


def decomp_first(objs):
    """The objects built from the decomp's own src/ (or its hostgen copy), if
    any; else all of them. port/unmatched/ holds the PC port's stand-ins."""
    src = [o for o in objs if '/port/unmatched/' not in o.replace('\\', '/')]
    return src or objs


def game_keeper(sym, objs):
    """Which of several game objects keeps a symbol they all define, or None.
      - the decomp's src/ over port/unmatched/ "faces" (a face gives a method
        the decomp matched as C++ its C name; under GCC both are one symbol);
      - the file named after the symbol (the decomp's filename-is-symbol rule);
      - among destructor/constructor variant files, see variant_keeper (GCC
        emits every variant, and the vtable, from each);
      - else the one file that is not a variant file: a D0 file may define
        the class's key function again only so mwccarm emits the vtable there,
        the real body living in its own TU."""
    cands = decomp_first(objs)
    for o in cands:
        if os.path.basename(o).split('.')[0] == sym:
            return o
    if len(cands) == 1:
        return cands[0]
    keep = variant_keeper(sym, cands)
    if keep:
        return keep
    plain = [o for o in cands if not DTOR_FILE.match(os.path.basename(o))]
    return plain[0] if len(plain) == 1 else None


# Symbols two PC-HAL files both define, which MSVC kept apart by decoration
# (class vs struct, static vs member, a shadow class's own vtable name) and
# GCC spells the same: which object keeps it.
HOST_KEEP = {
    # reverse_bridges.cpp: an empty static-member stub; scene_boot.cpp: the
    # face that reaches the ROM's dScene_c::SetFaders.
    '_ZN5Scene9SetFadersEP15FaderBrightness': 'scene_boot.o',
    # the hosted vtable arrays, not the vtables GCC emits for the one-slot
    # shadow classes declared in faces4_rows.cpp / dtor_faces_cpp.cpp
    '_ZTV7Tornado': 'actor_classes_ov096.o',
    **{'_ZTVN5dPa_c7level_c%sE' % c: 'particle_vtable.o' for c in (
        '16bubbleCallback_c', '16splashCallback_c', '24fitWaterSimpleCallback_c',
        '20checkWaterCallback_c', '26checkWaterRippleCallback_c', '14clipCallback_c',
        '18fitWaterCallback_c', '23cleanParticleCallback_c')},
    # equivalent forwarders to one C-linkage body, either copy
    '_ZNK12WithMeshClsn10IsOnGroundEv': 'reverse_bridges.o',
    '_ZN16MeshColliderBase6EnableEP8dActor_c': 'shutter_bob_face.o',
    '_ZN8Particle6System9NewSimpleEjiii': 'actor_classes_l7.o',
    '_ZN8Platform13IsClsnInRangeEii': 'wf_enemy_bridges.o',
}


def is_hostgen_object(path):
    """A src/ file as rewritten by port/tools/hostgen.py (MMIO proxied). Game
    code, and preferred over the unrewritten original when both are linked."""
    return '/host-src/src/' in path.replace('\\', '/')


LIBS = [
    '../arm-none-eabi/lib/armv6k/fpu/libc.a',
    '../arm-none-eabi/lib/armv6k/fpu/libm.a',
    '../arm-none-eabi/lib/armv6k/fpu/libstdc++.a',
    '../arm-none-eabi/lib/armv6k/fpu/libsupc++.a',
    '../../libctru/lib/libctru.a',
]


def library_symbols(tools, cache):
    """Every global symbol the toolchain's runtime libraries define."""
    if cache and os.path.exists(cache):
        return set(open(cache).read().split())
    nm = os.path.join(tools, 'arm-none-eabi-nm')
    out = set()
    libs = [os.path.normpath(os.path.join(tools, l)) for l in LIBS]
    gcc_dir = os.path.normpath(os.path.join(tools, '../lib/gcc/arm-none-eabi'))
    if os.path.isdir(gcc_dir):
        for v in os.listdir(gcc_dir):
            libs.append(os.path.join(gcc_dir, v, 'armv6k/fpu/libgcc.a'))
    for lib in libs:
        if not os.path.exists(lib):
            continue
        r = subprocess.run([nm, '-g', '--defined-only', lib], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] not in 'Uw':
                out.add(parts[2])
    if cache:
        with open(cache, 'w') as f:
            f.write('\n'.join(sorted(out)))
    return out


# also the constructor variants: GCC emits C1 and C2 from either file
DTOR_FILE = re.compile(r'^(_Z.*)(?:D[012]|C[12])Ev\.(?:c|cpp)?\.?o(?:bj)?$')


def variant_keeper(sym, objs):
    """Which object keeps a symbol that several destructor-variant files of
    one class all define. None if these are not such files."""
    names = {o: os.path.basename(o) for o in objs}
    stems = set()
    for o, n in names.items():
        m = DTOR_FILE.match(n)
        if not m:
            return None
        # a this-adjusting thunk's file (_ZThn80_N9ModelAnimD0Ev) is the
        # same class's
        stems.add(re.sub(r'^_ZThn?\d+_', '_Z', m.group(1)))
    if len(stems) != 1:
        return None
    for o, n in names.items():          # the file named after the symbol
        if n.split('.')[0] == sym:
            return o
    for o, n in names.items():          # else the complete-object dtor
        if 'D1Ev.' in n:
            return o
    return sorted(objs)[0]


def self_forwarders(tools, objects):
    """{obj: {sym}} for functions whose .text.<sym> section relocates against
    <sym> itself (see step 0b in main)."""
    if not objects:
        return {}
    objdump = os.path.join(tools, 'arm-none-eabi-objdump')
    with tempfile.NamedTemporaryFile('w', suffix='.rsp', delete=False) as rsp:
        for o in objects:
            rsp.write('"%s"\n' % o.replace('\\', '/'))
        rsp_name = rsp.name
    try:
        out = subprocess.run([objdump, '-r', '@' + rsp_name], capture_output=True,
                             text=True).stdout
    finally:
        os.unlink(rsp_name)
    norm = {o.replace('\\', '/'): o for o in objects}
    found = defaultdict(set)
    obj = sect = None
    for line in out.splitlines():
        m = re.match(r'^(.*\.o(?:bj)?):\s+file format', line)
        if m:
            obj = norm.get(m.group(1).replace('\\', '/'))
            sect = None
            continue
        m = re.match(r'^RELOCATION RECORDS FOR \[\.text\.(.+)\]:', line)
        if m:
            sect = m.group(1)
            continue
        if line.startswith('RELOCATION RECORDS FOR'):
            sect = None
            continue
        if obj and sect:
            parts = line.split()
            if len(parts) >= 3 and parts[-1] == sect:
                found[obj].add(sect)
    return found


def run_nm(tools, objects):
    """{obj: (defined_strong:set, defined_weak:set, undefined:set)}"""
    nm = os.path.join(tools, 'arm-none-eabi-nm')
    result = {o: (set(), set(), set()) for o in objects}
    with tempfile.NamedTemporaryFile('w', suffix='.rsp', delete=False) as rsp:
        for o in objects:
            rsp.write('"%s"\n' % o.replace('\\', '/'))
        rsp_name = rsp.name
    try:
        out = subprocess.run([nm, '-A', '-g', '@' + rsp_name], capture_output=True,
                             text=True, check=True).stdout
    finally:
        os.unlink(rsp_name)
    norm = {o.replace('\\', '/'): o for o in objects}
    for line in out.splitlines():
        # "<obj>:<addr> <T> <sym>" or "<obj>:         U <sym>"
        m = re.match(r'^(.*\.o(?:bj)?):\s*(?:[0-9a-fA-F]+)?\s+(\w)\s+(\S+)$', line)
        if not m:
            continue
        obj, kind, sym = m.group(1), m.group(2), m.group(3)
        o = norm.get(obj.replace('\\', '/'))
        if o is None:
            continue
        strong, weak, undef = result[o]
        if kind == 'U':
            undef.add(sym)
        elif kind in 'WVwv':
            weak.add(sym)
        else:
            strong.add(sym)
    return result


def demangle(tools, names):
    if not names:
        return {}
    cxxfilt = os.path.join(tools, 'arm-none-eabi-c++filt')
    out = subprocess.run([cxxfilt], input='\n'.join(names), capture_output=True,
                         text=True, check=True).stdout.splitlines()
    return dict(zip(names, out))


def qualified_of_demangled(d):
    """'Memory::Allocate(unsigned int, int, Heap*)' -> 'Memory::Allocate'."""
    if d.startswith('vtable for '):
        return None
    depth = 0
    for i, ch in enumerate(d):
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        elif ch == '(' and depth == 0:
            return d[:i]
    return d


def itanium_nested(parts):
    if len(parts) == 1:
        return parts[0]
    return '_ZN' + ''.join('%d%s' % (len(p), p) for p in parts) + 'E'


def parse_msvc(name):
    """Return ('data', gcc_name) | ('vtable', gcc_name) | ('func', qualified) | None."""
    if name.startswith('??_7'):
        body = name[4:]
        if '@@' not in body:
            return None
        scopes = body.split('@@', 1)[0].split('@')
        if any(s.startswith('?$') for s in scopes):
            return None
        cls = '::'.join(reversed(scopes))
        parts = list(reversed(scopes))
        mangled = ''.join('%d%s' % (len(p), p) for p in parts)
        if len(parts) > 1:
            mangled = 'N' + mangled + 'E'
        return ('vtable', '_ZTV' + mangled)
    if name.startswith('??0') or name.startswith('??1'):
        body = name[3:]
        if '@@' not in body:
            return None
        scopes = body.split('@@', 1)[0].split('@')
        if any(s.startswith('?$') for s in scopes):
            return None
        cls = scopes[0]
        qual = '::'.join(reversed(scopes))
        return ('func', qual + '::' + ('' if name[2] == '0' else '~') + cls)
    if not name.startswith('?') or name.startswith('??'):
        return None
    body = name[1:]
    if '@@' not in body:
        return None
    head, sig = body.split('@@', 1)
    parts = head.split('@')
    if any(p.startswith('?$') or not p for p in parts):
        return None
    parts = list(reversed(parts))          # outermost scope first
    if sig[:1] in '01234':
        return ('data', itanium_nested(parts))
    return ('func', '::'.join(parts))


def flat_to_gcc(name):
    m = re.match(r'^@(\w+)@\d+$', name)    # __fastcall C name: @name@<arg bytes>
    if m:
        return m.group(1)
    if name.startswith('__Z'):
        return name[1:]
    if name.startswith('_') and not name.startswith('__'):
        return name[1:]
    return None


def main():
    # @FILE reads arguments one per line: the game's ~9,000 object paths are
    # past the Windows command-line limit.
    ap = argparse.ArgumentParser(fromfile_prefix_chars='@')
    ap.add_argument('--out', required=True)
    ap.add_argument('--alias-src', action='append', default=[])
    ap.add_argument('--tools', required=True)
    ap.add_argument('--report', help='write a human-readable report here')
    ap.add_argument('--lib-cache', help='cache file for the runtime-library symbol set')
    ap.add_argument('--port-root-name', default=PORT_ROOT_NAME,
                    help='directory name of the upstream checkout (PORT_ROOT)')
    ap.add_argument('--objects-out',
                    help='write the object list here, native paths, as a g++ @response file')
    ap.add_argument('objects', nargs='+')
    a = ap.parse_args()
    set_port_root_name(a.port_root_name)

    # Paths from a response file arrive as MSYS writes them (/c/...), which
    # MSYS converts only on a command line; the native tools need C:/...
    objects = [re.sub(r'^/([a-zA-Z])/', lambda m: m.group(1).upper() + ':/', o)
               for o in a.objects if o.endswith(('.o', '.obj'))]
    if a.objects_out:
        with open(a.objects_out, 'w', newline='\n') as f:
            for o in objects:
                f.write('"%s"\n' % o.replace('\\', '/'))
    syms = run_nm(a.tools, objects)
    report = []

    # ---- 0. game definitions of runtime library names stay the game's ----
    # The ROM has its own operator new/delete (on the game heap) and its own copies
    # of C library functions. Under MSVC those never met the host runtime, but
    # under GCC src/_ZdlPv.cpp would become the global operator delete for every
    # host std::vector. So inside game objects each such name is renamed
    # __game_<name>: game code calls the game's version, host code keeps the
    # toolchain's.
    libsyms = library_symbols(a.tools, a.lib_cache)
    game_objs = [o for o in objects if is_game_object(o)]
    clash = set()
    for o in game_objs:
        strong, weak, _ = syms[o]
        clash |= (strong | weak) & libsyms
    # The ROM runtime's own vtables/RTTI (_ZTVSt9type_info ...), which the HAL
    # hosts as data: private in every object that names them. This build is
    # -fno-rtti, so only libsupc++ needs the toolchain's.
    for o in objects:
        strong, weak, _ = syms[o]
        clash |= {s for s in (strong | weak) & libsyms if s.startswith('_ZT')}
    if clash:
        rsp = a.out + '.rename'
        with open(rsp, 'w') as f:
            for s in sorted(clash):
                f.write('%s __game_%s\n' % (s, s))
        everywhere = {s for s in clash if s.startswith('_ZT')}
        game_set = set(game_objs)
        objcopy = os.path.join(a.tools, 'arm-none-eabi-objcopy')
        for o in objects:
            strong, weak, undef = syms[o]
            names = strong | weak | undef
            if (o in game_set and names & clash) or names & everywhere:
                subprocess.run([objcopy, '--redefine-syms=' + rsp, o], check=True)
        for s in sorted(clash):
            report.append('private %-60s  -> __game_%s in game objects' % (s, s))
        syms = run_nm(a.tools, objects)

    # ---- 0b. self-forwarders ----
    # The PC HAL bridges its two naming schemes with one-line forwarders like
    #     void GX::BeginLoadTex() { _ZN2GX12BeginLoadTexEv(); }
    # Under GCC both names are the same symbol, so that becomes a function that
    # calls itself forever. A host function whose section relocates against its own
    # symbol is such a forwarder, and it's made weak so the real body wins. Weak,
    # not renamed: a rename would also redirect the object's other references to
    # the forwarder. A forwarder with no other definition is reported missing so
    # closure.py pulls the body from src/. Needs -ffunction-sections.
    # Only the PC port's own code is checked; port3ds code is GCC-native. A false
    # positive (a genuinely recursive function) fails the link rather than
    # miscompiling.
    pc_marker = '/%s/port/' % PORT_ROOT_NAME
    host_objs = [o for o in objects
                 if not is_game_object(o) and pc_marker in o.replace('\\', '/')]
    fwd = self_forwarders(a.tools, host_objs)
    forwarders = set()
    if fwd:
        objcopy = os.path.join(a.tools, 'arm-none-eabi-objcopy')
        for o, names in fwd.items():
            cmd = [objcopy]
            for n in sorted(names):
                cmd += ['-W', n]
                forwarders.add(n)
                report.append('forward %-60s  self-forwarder in %s weakened' % (n, os.path.basename(o)))
            cmd.append(o)
            subprocess.run(cmd, check=True)
        syms = run_nm(a.tools, objects)

    strong_by = defaultdict(list)
    defined = set()
    referenced = set()
    for o, (strong, weak, undef) in syms.items():
        for s in strong:
            strong_by[s].append(o)
        defined |= strong | weak
        referenced |= undef

    # ---- 1. game code wins over host copies -----------------------------
    weaken = defaultdict(list)
    for s, objs in strong_by.items():
        if len(objs) < 2:
            continue
        game = [o for o in objs if is_game_object(o)]
        host = [o for o in objs if not is_game_object(o)]
        if len(game) == 1 and host:
            for o in host:
                weaken[o].append(s)
            report.append('weaken  %-60s  game wins over %d host copy(ies)' % (s, len(host)))
        elif len(game) > 1 and len([o for o in game if is_hostgen_object(o)]) == 1:
            keep = [o for o in game if is_hostgen_object(o)][0]
            for o in objs:
                if o != keep:
                    weaken[o].append(s)
            report.append('weaken  %-60s  hostgen copy wins' % s)
        elif len(game) > 1 and game_keeper(s, game):
            # Several game copies (see game_keeper): keep one, weaken the
            # rest, host copies (a HAL-hosted vtable array) included.
            keep = game_keeper(s, game)
            for o in objs:
                if o != keep:
                    weaken[o].append(s)
            report.append('weaken  %-60s  kept %s%s' % (
                s, os.path.basename(keep),
                ' over %d host copy(ies)' % len(host) if host else ''))
        elif not game and HOST_KEEP.get(s) in [os.path.basename(o) for o in host]:
            keep = [o for o in host if os.path.basename(o) == HOST_KEEP[s]][0]
            for o in objs:
                if o != keep:
                    weaken[o].append(s)
            report.append('weaken  %-60s  kept %s (HOST_KEEP)' % (s, HOST_KEEP[s]))
        else:
            report.append('DUPLICATE %-58s  %s' % (s, ', '.join(os.path.basename(o) for o in objs)))
    objcopy = os.path.join(a.tools, 'arm-none-eabi-objcopy')
    for o, names in weaken.items():
        cmd = [objcopy]
        for n in names:
            cmd += ['-W', n]
        cmd.append(o)
        subprocess.run(cmd, check=True)

    # ---- 1b. HAL views of a vtable GCC now owns ----
    # The PC HAL hosts vtables as arrays where [n] is ROM slot n. GCC's _ZTV symbol
    # starts 8 bytes before the address point, so where the game's table won, a HAL
    # [n] would land two slots low. HAL objects that only reference such a table
    # are pointed at NAME__hostap = NAME + 8; HAL objects that defined the array
    # keep it private so they can't write into GCC's table.
    hostap = sorted({n for o, names in weaken.items() if not is_game_object(o)
                     for n in names if n.startswith('_ZTV')
                     and any(is_game_object(g) for g in strong_by.get(n, ()))})
    if hostap:
        hs = set(hostap)
        rsp = a.out + '.hostap'
        with open(rsp, 'w') as f:
            for n in hostap:
                f.write('%s %s__hostap\n' % (n, n))
        for o in objects:
            if is_game_object(o):
                continue
            strong, weak, undef = syms[o]
            defs = (strong | weak) & hs
            refs = undef & hs
            if defs:
                cmd = [objcopy]
                for n in sorted(defs):
                    cmd += ['--localize-symbol', n]
                subprocess.run(cmd + [o], check=True)
            if refs:
                subprocess.run([objcopy, '--redefine-syms=' + rsp, o], check=True)
        for n in hostap:
            report.append('hostap  %-60s  HAL [n] = GCC address point + n' % n)

    # ---- 2. /alternatename rows -> PROVIDE ------------------------------
    rows = []
    for d in a.alias_src:
        for root, _, files in os.walk(d):
            for f in files:
                if not f.endswith(('.c', '.cpp', '.h', '.inc')):
                    continue
                path = os.path.join(root, f)
                try:
                    text = open(path, encoding='utf-8', errors='replace').read()
                except OSError:
                    continue
                for m in ROW.finditer(text):
                    rows.append((m.group(1), m.group(2)))

    undefined = sorted(referenced - defined)
    dem = demangle(a.tools, [u for u in undefined if u.startswith('_Z')])
    undef_by_qual = defaultdict(list)
    for u in undefined:
        q = qualified_of_demangled(dem.get(u, u))
        if q:
            undef_by_qual[q].append(u)
    defined_dem = demangle(a.tools, sorted(s for s in defined if s.startswith('_Z')))
    defined_by_qual = defaultdict(set)
    for s, d in defined_dem.items():
        q = qualified_of_demangled(d)
        if q:
            defined_by_qual[q].add(s)

    def resolve_rhs(rhs):
        """GCC symbol (and offset) a row's right-hand side names, or None."""
        g = flat_to_gcc(rhs)
        if g:
            return g, 0
        p = parse_msvc(rhs)
        if not p:
            return None
        kind, v = p
        if kind == 'data':
            return v, 0
        if kind == 'vtable':
            return v, 0
        cands = defined_by_qual.get(v, set())
        if len(cands) > 1 and rhs.startswith(('??0', '??1')):
            # one MSVC structor, several Itanium variants: ??1 is the
            # complete-object (non-deleting) destructor, ??0 the constructor
            cands = structor_variants(cands, rhs) or cands
        if len(cands) == 1:
            return next(iter(cands)), 0
        return None

    def structor_variants(syms, msvc):
        """The Itanium variant an MSVC structor name means: D1 (else D2) for
        ??1, C1 (else C2) for ??0."""
        order = ('D1Ev', 'D2Ev') if msvc.startswith('??1') else ('C1E', 'C2E')
        for tag in order:
            hit = {s for s in syms if tag in s}
            if len(hit) == 1:
                return hit
        return None

    # lhs gcc name -> set of (rhs, offset)
    provide = defaultdict(set)
    for lhs, rhs in rows:
        r = resolve_rhs(rhs)
        if not r:
            continue
        rsym, roff = r
        g = flat_to_gcc(lhs)
        targets = []
        off = roff
        if g:
            targets = [g]
        else:
            p = parse_msvc(lhs)
            if not p:
                continue
            kind, v = p
            if kind == 'data':
                targets = [v]
            elif kind == 'vtable':
                # MSVC's ??_7 is the address point; Itanium's _ZTV is 8 bytes
                # before it (offset-to-top, RTTI).
                targets = [v]
                off -= 8
            else:
                targets = undef_by_qual.get(v, [])
                if lhs.startswith(('??0', '??1')) and len(targets) > 1:
                    # never point a deleting (D0) name at a non-deleting body
                    targets = sorted(structor_variants(set(targets), lhs) or [])
        for t in targets:
            if t != rsym:
                provide[t].add((rsym, off))

    # An MSVC row is matched by qualified name, which cannot tell overloads
    # apart. Where the target is an Itanium-named function, keep it only if its
    # parameter list equals the undefined symbol's (both read from c++filt).
    both = sorted({u for u in provide if u.startswith('_Z')} |
                  {r for c in provide.values() for r, _ in c if r.startswith('_Z')})
    dem_all = demangle(a.tools, both)

    def params(sym):
        d = dem_all.get(sym)
        if not d or '(' not in d:
            return None
        return d[d.index('('):]

    for u in list(provide):
        pu = params(u)
        if pu is None or len(provide[u]) < 2:
            continue
        keep = {(r, o) for r, o in provide[u] if not r.startswith('_Z') or params(r) in (None, pu)}
        if keep:
            provide[u] = keep

    # gccfix renames a header's C-linkage declaration of a class's vtable to
    # X__decl in the translation unit that also defines the class (so GCC's own
    # table keeps the name); the declaration's users mean the same table.
    for u in undefined:
        if u.endswith('__decl') and u[:-6] in defined and u not in provide:
            provide[u] = {(u[:-6], 0)}

    lines = []
    resolved = 0
    for u in undefined:
        if u not in provide:
            continue
        # Follow a chain of aliases (A -> B -> C) to something defined.
        chosen = None
        cands = provide[u]
        total = 0
        seen = set()
        while len(seen) < 8:
            if len(cands) > 1:
                # One row names the decomp's own Itanium body, the others flat
                # C forwarders (MSVC bridges onto that same body): the body.
                real = {c for c in cands if c[0].startswith('_Z') and c[0] in defined}
                if len(real) == 1:
                    cands = real
            if len(cands) != 1:
                report.append('AMBIGUOUS %s -> %s' % (u, sorted(cands)))
                break
            rsym, off = next(iter(cands))
            total += off
            if rsym in defined:
                chosen = (rsym, total)
                break
            if rsym in seen or rsym not in provide:
                break
            seen.add(rsym)
            cands = provide[rsym]
        if chosen:
            rsym, off = chosen
            # A HAL name for a vtable GCC emitted (_ZTV9PushBlock -> _ZTV11PowerFlower):
            # the HAL indexes from the address point like the other hostap views, so +8,
            # or every slot lands two low.
            if (u.startswith('_ZTV') and rsym.startswith('_ZTV') and not u.endswith('__hostap')
                    and any(is_game_object(g) for g in strong_by.get(rsym, ()))):
                off += 8
            expr = rsym if off == 0 else '%s %s %d' % (rsym, '-' if off < 0 else '+', abs(off))
            lines.append('PROVIDE(%s = %s);' % (u, expr))
            report.append('alias   %-60s = %s' % (u, expr))
            resolved += 1

    game_refs = set()
    for o in objects:
        if is_game_object(o):
            game_refs |= syms[o][2]

    # ---- 1c. decomp code that stores a vtable symbol as a vptr ----
    # mwccarm's _ZTV symbols label the address point, so the decomp writes
    # `*(void ***)p = _ZTV5Stage;`. GCC's table starts 8 bytes earlier, so gccfix.py
    # rewrites those to NAME__hostap, resolved here: the address point of a table
    # GCC emitted, or the start of one the HAL hosts as a plain array.
    mwvptr = sorted({u[:-len('__hostap')] for u in game_refs
                     if u.startswith('_ZTV') and u.endswith('__hostap')} - set(hostap))
    for n in mwvptr:
        gcc = any(is_game_object(g) for g in strong_by.get(n, ()))
        lines.append('EXTERN(%s)' % n)
        lines.append('PROVIDE(%s__hostap = %s%s);' % (n, n, ' + 8' if gcc else ''))
        report.append('mwvptr  %-60s  %s' % (n, 'GCC table: its address point' if gcc
                                             else 'HAL array: element 0'))
    # A self-forwarder that is still the only definition would loop at run
    # time: ask closure.py for the real body.
    for n in sorted(forwarders):
        if n not in strong_by:
            report.append('missing %s (game)   [only a self-forwarder defines it]' % n)
    for u in undefined:
        if u not in provide and not (u.endswith('__hostap') and u[:-8] in mwvptr):
            report.append('missing %s%s%s' % (u, ' (game)' if u in game_refs else '',
                                              ('   [' + dem[u] + ']') if u in dem else ''))

    for n in hostap:
        # EXTERN: a GC root -- once the HAL's references name the alias,
        # nothing else may keep GCC's table, and --gc-sections would drop it
        # (the alias would then resolve to 0 + 8).
        lines.append('EXTERN(%s)' % n)
        lines.append('PROVIDE(%s__hostap = %s + 8);' % (n, n))
    with open(a.out, 'w') as f:
        f.write('/* generated by port3ds/tools/linkfix.py -- do not edit */\n')
        f.write('\n'.join(lines) + '\n')
    if a.report:
        with open(a.report, 'w') as f:
            f.write('\n'.join(report) + '\n')
    print('linkfix: %d object(s), weakened %d host symbol(s) in %d object(s), '
          '%d alias(es), %d still undefined (before libraries)'
          % (len(objects), sum(len(v) for v in weaken.values()), len(weaken),
             resolved, len(undefined) - resolved))
    return 0


if __name__ == '__main__':
    sys.exit(main())
