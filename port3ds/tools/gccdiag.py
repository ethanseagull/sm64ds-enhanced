#!/usr/bin/env python3
"""Turn a build log's GCC diagnostics into gccfix.py rules.

  gccdiag.py --log build3ds/game_build.log --rules port3ds/game/gccfix_rules.json
             --root C:/sm64ds-3ds [--gccfix-out C:/sm64ds-3ds/build3ds/gccfix]

Reads the errors that have a mechanical, behaviour-preserving fix and merges
rules for them into RULES (existing rules are kept, duplicates skipped):

  conflicting declaration of C function 'S'  /  conflicting declaration 'S'
  /  conflicting return type specified for 'S'
      Two declarations of one C-linkage name in one translation unit with
      different types (the decomp's "declaration agreement" debt: mwccarm and
      MSVC accept it). Keep the DEFINITION's spelling when one of the two lines
      is the definition, else the first declaration; drop the other, if it is a
      one-line declaration in the source file itself (never a header).
  jump to label 'L' ... crosses initialization of 'T n'
      Split "T n = e;" into "T n; n = e;" on that line.
  storage size of 'X' isn't known
      Prefix "extern" (an unsized array MSVC took as a declaration).
  Error: symbol `S' is already defined   (assembler)
      The file defines a C++ function and a flat C copy under one mangled
      name: rename the flat spelling in that file to S__flat.

A diagnostic in a gccfix copy is mapped back to the original's path (the
copy mirrors it under --gccfix-out). Prints what it added and what it could
not handle.
"""
import argparse
import json
import os
import re
import sys

LOC = re.compile(r'^(?P<file>[A-Za-z]:/[^:]+|/[^:]+):(?P<line>\d+):(?P<col>\d+): (?P<kind>error|note|warning): (?P<msg>.*)$')
ASM_DUP = re.compile(r"Error: symbol `(?P<sym>[^']+)' is already defined")
OBJ_OF = re.compile(r'\*\*\* \[CMakeFiles/[^:]+/build\.make:\d+: CMakeFiles/[^/]+\.dir/(?P<obj>.+?)\.o\] Error')


def to_win(p):
    m = re.match(r'^/([a-zA-Z])/(.*)$', p)
    return '%s:/%s' % (m.group(1).upper(), m.group(2)) if m else p


def read_line(path, n):
    try:
        with open(to_win(path), encoding='utf-8', errors='replace') as f:
            for i, l in enumerate(f, 1):
                if i == n:
                    return l.rstrip('\n')
    except OSError:
        pass
    return None


def is_one_line_decl(text, name):
    if text is None or name not in text:
        return False
    s = re.sub(r'/\*.*?\*/', '', text)
    s = s.split('//', 1)[0].strip()
    return s.endswith(';') and '{' not in s and '}' not in s and '=' not in s


def looks_like_definition(text):
    if text is None:
        return False
    s = re.sub(r'/\*.*?\*/', '', text).split('//', 1)[0]
    return '{' in s or not s.rstrip().endswith(';')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', required=True)
    ap.add_argument('--rules', required=True)
    ap.add_argument('--root', required=True)
    ap.add_argument('--gccfix-out')
    a = ap.parse_args()
    root = os.path.abspath(to_win(a.root)).replace('\\', '/')
    fixdir = os.path.abspath(to_win(a.gccfix_out)).replace('\\', '/') if a.gccfix_out else None

    def rel_of(path):
        # (rel of the ORIGINAL, the path the diagnostic names). Rules are keyed
        # by the original; line text is read from the file GCC compiled, since
        # a copy may differ from the original on the lines rules changed.
        p = os.path.normpath(os.path.abspath(to_win(path))).replace('\\', '/')
        diag = p
        if fixdir and p.lower().startswith(fixdir.lower() + '/'):
            p = root + '/' + p[len(fixdir) + 1:]           # copy -> original
        return os.path.relpath(p, root).replace('\\', '/'), diag

    rules = []
    if os.path.exists(a.rules):
        rules = json.load(open(a.rules))
    have = {json.dumps(r, sort_keys=True) for r in rules}
    added, unhandled = [], []

    def add(r):
        k = json.dumps(r, sort_keys=True)
        if k not in have:
            have.add(k)
            rules.append(r)
            added.append(r)

    lines = open(a.log, encoding='utf-8', errors='replace').read().splitlines()
    pending_asm = None
    for i, raw in enumerate(lines):
        m = ASM_DUP.search(raw)
        if m:
            pending_asm = m.group('sym')
            continue
        if pending_asm:
            mo = OBJ_OF.search(raw)
            if mo:
                obj = mo.group('obj')            # mirrors the source path
                src = None
                for ext in ('.cpp', '.c'):
                    cand = '/' + obj + ext if not obj.startswith('/') else obj + ext
                    if os.path.exists(to_win(cand)):
                        src = cand
                        break
                if src:
                    rel, _ = rel_of(src)
                    add({'file': rel, 'action': 'rename', 'name': pending_asm,
                         'to': pending_asm + '__flat'})
                else:
                    unhandled.append('asm duplicate %s (object %s)' % (pending_asm, obj))
                pending_asm = None
            continue
        m = LOC.match(raw)
        # "too many arguments" inside a template body is reported as a WARNING
        # (-Wtemplate-body) and the template then fails to instantiate.
        if m and m.group('kind') == 'warning' and m.group('msg').startswith('too many arguments'):
            pass
        elif not m or m.group('kind') != 'error':
            continue
        msg, path, ln = m.group('msg'), m.group('file'), int(m.group('line'))
        rel, absp = rel_of(path)
        if rel.startswith('port-0.9.1/include/') or '/include/' in rel and not rel.endswith(('.c', '.cpp')):
            if 'conflicting' in msg:
                pass  # resolved from the other side when it is in the source file
        cm = re.match(r"conflicting (?:declaration of C function|declaration|return type specified for) '(?P<sig>.*)'", msg)
        if cm:
            sig = cm.group('sig')
            # The declared name: a decomp symbol (data_/func_/_Z...) when there is
            # one -- oid (C_f300c::* data_x [])(int) names data_x, not void --
            # else the first identifier followed by ( or [.
            name = re.findall(r'\b((?:data|func)_\w+|_Z\w+)\b', sig)
            if not name:
                name = [n for n in re.findall(r'([A-Za-z_]\w*)\s*(?:\(|\[|$)', sig)
                        if n not in ('void', 'int', 'char', 'short', 'long', 'unsigned',
                                     'signed', 'const', 'struct', 'bool', 'float', 'double')]
            name = name[0] if name else None
            # the matching "previous declaration" note follows
            prev = None
            for j in range(i + 1, min(i + 12, len(lines))):
                n = LOC.match(lines[j])
                if n and n.group('kind') == 'note' and 'previous' in n.group('msg'):
                    prev = (n.group('file'), int(n.group('line')))
                    break
            here = read_line(absp, ln)
            prel, pabs, ptext, psig = None, None, None, None
            if prev:
                prel, pabs = rel_of(prev[0])
                ptext = read_line(pabs, prev[1])
                for j in range(i + 1, min(i + 12, len(lines))):
                    n = LOC.match(lines[j])
                    if n and n.group('kind') == 'note' and 'previous' in n.group('msg'):
                        pm = re.search(r"'(?P<s>.*)'", n.group('msg'))
                        psig = pm.group('s') if pm else None
                        break
            # Replace, never drop: the two declarations often sit in different
            # functions (block-scope declarations of one C-linkage name), and
            # removing one leaves that function with none. Rewrite one line to
            # the kept signature -- the definition's when one side is the
            # definition, else the first declaration's -- as GCC itself spells
            # it in the diagnostic. -fpermissive then converts the call's
            # arguments where the types differ.
            def usable(s):
                return s and '::' not in s.split('(', 1)[-1] and '{' not in s
            def keep_extern(orig, decl):
                # GCC renders the kept declaration without its storage class;
                # a block-scope VARIABLE declaration without extern would be
                # a local definition.
                if re.match(r'\s*extern\b', orig or '') and not decl.startswith('extern'):
                    return 'extern ' + decl
                return decl
            def block_scope(t):
                return t is not None and t[:1] in (' ', '\t')
            # A block-scope declaration (indented: inside a function) gets its
            # own asm-labelled alias for the rest of its block: each function
            # keeps exactly the type its author wrote. Only file-scope
            # conflicts are rewritten to one signature.
            if name and rel.endswith(('.c', '.cpp')) and is_one_line_decl(here, name) \
                    and not looks_like_definition(here) and block_scope(here):
                add({'file': rel, 'line': here.strip(), 'action': 'alias_local', 'name': name})
            elif name and prev and prel.endswith(('.c', '.cpp')) and \
                    is_one_line_decl(ptext, name) and block_scope(ptext):
                add({'file': prel, 'line': ptext.strip(), 'action': 'alias_local', 'name': name})
            elif name and rel.endswith(('.c', '.cpp')) and is_one_line_decl(here, name) \
                    and not looks_like_definition(here) and usable(psig):
                add({'file': rel, 'line': here.strip(), 'action': 'replace',
                     'with': keep_extern(here, psig) + ';'})
            elif name and prev and prel.endswith(('.c', '.cpp')) and \
                    is_one_line_decl(ptext, name) and usable(sig):
                add({'file': prel, 'line': ptext.strip(), 'action': 'replace',
                     'with': keep_extern(ptext, sig) + ';'})
            else:                unhandled.append('%s:%d %s' % (rel, ln, msg[:120]))
            continue
        jm = re.match(r"jump to label '(?P<lab>[^']+)'", msg)
        if jm:
            for j in range(i + 1, min(i + 30, len(lines))):
                n = LOC.match(lines[j])
                if not n:
                    continue
                if n.group('kind') == 'error':
                    break
                cn = re.search(r"crosses initialization of '(?P<decl>.*)'", n.group('msg'))
                if cn:
                    decl = cn.group('decl')
                    name = re.findall(r'([A-Za-z_]\w*)\s*$', decl)
                    nrel, nabs = rel_of(n.group('file'))
                    text = read_line(nabs, int(n.group('line')))
                    if name and text and '=' in text:
                        add({'file': nrel, 'line': text.strip(), 'action': 'split_init',
                             'name': name[0]})
                    else:
                        unhandled.append('%s: crosses %s' % (nrel, decl))
            continue
        tm = re.match(r"too many arguments to function(?: '(?P<sig>.*)')?", msg)
        if tm:
            # A call still passing the x86 EDX dummy to a __fastcall shim that
            # gccfix took the dummy out of: drop that call's second argument.
            # Only in PC-port code, where the dummy convention lives.
            text = read_line(absp, ln)
            callee = None
            if tm.group('sig'):
                nm = re.findall(r'([A-Za-z_]\w*)\s*\(', tm.group('sig'))
                callee = nm[0] if nm else None
            if text and not callee:
                col = int(m.group('col')) - 1
                cm2 = re.match(r'\s*(?:\(\s*\*\s*)?([A-Za-z_]\w*)\s*\(', text[col:])
                if cm2:
                    callee = cm2.group(1)
                else:
                    # GCC's column is often the call's '(' itself: the callee is
                    # the identifier just before it.
                    back = re.search(r'([A-Za-z_]\w*)\s*$', text[:col])
                    callee = back.group(1) if back else None
            if callee and text and '/port/' in rel and ('%s(' % callee in text.replace(' (', '(')):
                add({'file': rel, 'line': text.strip(), 'action': 'drop_arg2', 'callee': callee})
            elif text and '/port/' in rel:
                # a call through a cast function pointer, ((Fn)vt[n])(self, 0, ...):
                # drop the literal dummy argument on this line
                add({'file': rel, 'line': text.strip(), 'action': 'drop_dummy_args'})
            else:
                unhandled.append('%s:%d %s' % (rel, ln, msg[:120]))
            continue
        lk = re.match(r"conflicting declaration of '[^']*?\b(?P<n>_ZT[VIS]\w+)\s*\[[^']*' with 'C' linkage", msg)
        if lk:
            # A HAL-hosted vtable (or RTTI) array spelled with the same name as
            # the one GCC emits for the class: rename the hosted copy in this
            # file so the real table keeps the name. In a header, the rename
            # goes into the translation unit being compiled, before the header.
            name = re.sub(r'__decl(?:__hal)*$', '', lk.group('n'))
            # Two headers that disagree only on linkage: an extern "C"
            # declaration and a plain extern T NAME[]; (the "previous
            # declaration" note points at it). Renaming would rename both;
            # declare the name extern "C" first in the TU, which the plain
            # declaration then inherits.
            plain = None
            for j in range(i + 1, min(i + 12, len(lines))):
                n = LOC.match(lines[j])
                if n and n.group('kind') == 'note' and "previous declaration with 'C++' linkage" in n.group('msg'):
                    _, pabs = rel_of(n.group('file'))
                    ptext = read_line(pabs, int(n.group('line'))) or ''
                    pm = re.match(r'\s*extern\s+([\w ]+?)\s*\b%s\s*(\[\w*\])\s*;' % re.escape(name), ptext)
                    if pm:
                        plain = 'extern "C" %s %s%s;  /* gccfix: headers disagree on linkage */' \
                                % (pm.group(1), name, pm.group(2))
                    break
            tu = rel if rel.endswith(('.c', '.cpp')) else None
            if not tu:
                for j in range(i - 1, max(i - 40, 0), -1):
                    im = re.match(r'(?:In file included |\s+)from (?P<f>[A-Za-z]:/[^:]+|/[^:]+):\d+[:,]', lines[j])
                    if im:
                        tu = rel_of(im.group('f'))[0]
                    elif tu and not lines[j].startswith(('In file', ' ')):
                        break
            if plain and tu:
                add({'file': tu, 'action': 'prepend', 'text': plain})
            elif rel.endswith(('.c', '.cpp')):
                add({'file': rel, 'action': 'rename', 'name': name, 'to': name + '__hal'})
            else:
                tu = None
                for j in range(i - 1, max(i - 40, 0), -1):
                    im = re.match(r'(?:In file included |\s+)from (?P<f>[A-Za-z]:/[^:]+|/[^:]+):\d+[:,]', lines[j])
                    if im:
                        tu = im.group('f')
                    elif tu and not lines[j].startswith(('In file', ' ')):
                        break
                if tu:
                    trel, _ = rel_of(tu)
                    add({'file': trel, 'action': 'prepend',
                         'text': '#define %s %s__decl  /* gccfix: the class vtable keeps the name */'
                                 % (name, name)})
                else:
                    unhandled.append('%s:%d %s' % (rel, ln, msg[:120]))
            continue
        nd = re.match(r"no declaration matches '[^']*::(?P<m>Destructor[01])\(\)'", msg)
        if nd:
            # A definition of the MSVC-only D1/D0 slot members, which the GCC
            # side of the class header does not declare (it has the real
            # virtual destructor).
            text = read_line(absp, ln)
            if text:
                add({'file': rel, 'line': text.strip(), 'action': 'drop_def'})
            continue
        cv = re.match(r"cannot convert '(?P<from>[^']+)'(?: \{aka '[^']+'\})? to '(?P<to>[^']+)'", msg)
        if False and cv and rel.endswith(('.c', '.cpp')):   # retired: alias_local makes casts unnecessary
            # A call passing an object pointer where the (kept) declaration
            # says char* or another pointer type: the conversion mwccarm and
            # MSVC made silently, spelled out. Pointer bits do not change.
            text = read_line(absp, ln)
            to = cv.group('to')
            if text and to.endswith('*') and cv.group('from').endswith('*'):
                add({'file': rel, 'line': text.strip(), 'action': 'cast_arg',
                     'col': int(m.group('col')) - (len(text) - len(text.lstrip())),
                     'type': to})
            else:
                unhandled.append('%s:%d %s' % (rel, ln, msg[:120]))
            continue
        sm = re.match(r"storage size of '(?P<x>[^']+)' isn't known", msg)
        if sm:
            text = read_line(absp, ln)
            bare = re.sub(r'/\*.*?\*/', '', text or '')
            bare = re.sub(r'/\*.*$', '', bare).split('//', 1)[0].rstrip()   # an unclosed comment too
            if text and bare.endswith('[];') and 'extern' not in bare:
                add({'file': rel, 'line': text.strip(), 'action': 'extern'})
            else:
                unhandled.append('%s:%d %s' % (rel, ln, msg[:120]))
            continue

    rules.sort(key=lambda r: (r['file'], r['action'], r.get('line', r.get('name', ''))))
    os.makedirs(os.path.dirname(os.path.abspath(a.rules)), exist_ok=True)
    with open(a.rules, 'w', newline='\n') as f:
        json.dump(rules, f, indent=1)
        f.write('\n')
    print('gccdiag: %d rule(s) added, %d total, %d diagnostic(s) left for hand fixes'
          % (len(added), len(rules), len(unhandled)))
    by = {}
    for r in added:
        by[r['action']] = by.get(r['action'], 0) + 1
    print('  added by action:', by)
    for u in unhandled[:40]:
        print('  hand:', u)
    return 0


if __name__ == '__main__':
    sys.exit(main())
