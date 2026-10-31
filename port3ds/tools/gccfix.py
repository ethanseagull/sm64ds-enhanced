#!/usr/bin/env python3
"""Rewrite, into build-tree copies, the constructs GCC rejects but MSVC and
mwccarm accept. Nothing in the upstream checkout is edited (src/ is the
byte-verified source of truth); this is the 3DS build's analogue of
port/tools/hostgen.py.

  gccfix.py --root TOP --out DIR --list FILE [--rules RULES.json]

FILE holds one source path per line. For each that needs a change, a copy is
written to DIR/<path relative to TOP> and a line is printed:

    <orig>\t<copy or ->\t<language or ->

("-" for "no copy" or "language unchanged"). Files that need nothing are not
printed.

Built-in fixes, every file:
  extern "C" extern T x;        -> extern "C" T x;
  extern struct X;              -> struct X;           (C++: no storage class on a type)
  a .c file whose first line is //cpp is compiled as C++ (the decomp's marker)
  a _ZTVx named anywhere         -> #define _ZTVx _ZTVx__hostap at the top (decomp code: mw_vptr)
  #ifdef _MSC_VER around only #pragma pack lines -> also taken by GCC (decomp code: msc_pack)

Rules from the compiler (RULES.json, written by tools/gccdiag.py from a build
log), keyed by file and exact line text so they survive unrelated upstream
edits:
  {"file": rel, "line": text, "action": "replace", "with": decl}
      the line becomes decl (one of two conflicting declarations of a C-linkage
      name, rewritten to the kept signature)
  {"file": rel, "line": text, "action": "drop"}         comment the line out
      (one of two conflicting declarations of a C function or variable)
  {"file": rel, "line": text, "action": "split_init", "name": n}
      "T n = e;" -> "T n; n = e;"  (a goto crosses the initialisation)
  {"file": rel, "line": text, "action": "extern"}       prefix "extern "
      (MSVC reads an unsized array inside extern "C" { } as a declaration;
       GCC as a definition of unknown size)
  {"file": rel, "action": "prepend", "text": t}         put t before line 1
      (a hand rule: e.g. an #include the PC build got transitively)
  {"file": rel, "action": "rename", "name": n, "to": m}
      the identifier n -> m in this file (a flat C copy of a C++ symbol the
      same file also defines)
"""
import argparse
import json
import os
import re
import sys

EXTERN_EXTERN = re.compile(r'\bextern\s+"C"\s+extern\b')
EXTERN_STRUCT = re.compile(r'^(\s*)extern\s+(struct|class|union)\s+(\w+)\s*;', re.M)


def to_win(p):
    m = re.match(r'^/([a-zA-Z])/(.*)$', p)
    return '%s:/%s' % (m.group(1).upper(), m.group(2)) if m else p


# The PC HAL fakes __thiscall with `T __fastcall f(void *self, void *edx, A a)`:
# on x86 __fastcall passes the first two words in ECX and EDX, so the second
# parameter is a dummy that soaks up EDX. On ARM the game calls slot(this, a)
# with a in r1, so the dummy would swallow `a`. Drop the dummy (a second
# `void *` parameter) from every __fastcall declarator. A direct call that still
# passes it then fails to compile instead of miscompiling.
FASTCALL_DECL = re.compile(
    r'(__fastcall\s*(?:\*\s*)?[\w:~#]*\s*\)?\s*\(\s*)'    # "__fastcall name(" / "__fastcall *p)(" / "name_##Class("
    r'([^,()]*?)'                                         # the first parameter
    r'\s*,\s*void\s*\*\s*(?:\w+\s*)?'                     # the EDX dummy
    r'(,|(?=\)))')                                        # then more parameters, or the end


DUMMY_NAME = re.compile(r',\s*void\s*\*\s*(\w+)\s*$')


def drop_fastcall_dummy(text):
    # A NAMED dummy may be used in the body (passed on to another shim): it
    # becomes a local of the same name, so that code still compiles; the call
    # it is passed to loses that argument through gccdiag's drop_arg2 rule.
    out, pos = [], 0
    for m in FASTCALL_DECL.finditer(text):
        out.append(text[pos:m.start()])
        out.append(m.group(1) + m.group(2) + (m.group(3) or ''))
        pos = m.end()
        dummy = DUMMY_NAME.search(text[m.start(2):m.end()].rstrip(',').rstrip())
        name = dummy.group(1) if dummy else None
        if name:
            # a definition: the next '{' comes before any ';'
            rest = text[pos:]
            brace, semi = rest.find('{'), rest.find(';')
            close = rest.find(')')
            # Only a real definition: the body's '{' follows the parameter
            # list with nothing but whitespace (or a trailing qualifier)
            # between, never a '{' further on (a signature quoted in a comment).
            gap = rest[close + 1:brace] if 0 <= close < brace else None
            if gap is not None:
                # comments and macro line continuations are fine
                gap = re.sub(r'/\*.*?\*/|//[^\n]*|\\\s*\n', ' ', gap, flags=re.S)
            if brace >= 0 and (semi < 0 or brace < semi) and gap is not None and \
                    re.fullmatch(r'\s*(?:const|noexcept|override)?\s*', gap):
                out.append(rest[:brace + 1] + ' void *%s = 0; (void)%s;' % (name, name))
                pos += brace + 1
    out.append(text[pos:])
    return ''.join(out)


DUMMY_ARGS = {'0', '0u', 'NULL', 'nullptr', '(void *)0', '(void*)0', 'dead_edx', 'edx',
              'dummy', 'unused', '_edx'}


def drop_dummy_args(line):
    """On a line GCC flagged for too many arguments: in every argument list
    whose second argument is the x86 EDX dummy (a literal 0/NULL/nullptr or a
    dummy's name), drop that argument. For calls through cast function
    pointers, `((Fn)vt[n])(self, 0, x)`, whose callee has no name."""
    i = 0
    while True:
        i = line.find('(', i)
        if i < 0:
            return line
        j, depth, commas = i + 1, 1, []
        while j < len(line) and depth:
            d = line[j]
            if d in '([{':
                depth += 1
            elif d in ')]}':
                depth -= 1
            elif d == ',' and depth == 1:
                commas.append(j)
            j += 1
        # An argument list may continue on the next line (depth > 0): the
        # dummy is still the second argument when two commas are on this one.
        if (depth == 0 or len(commas) > 1) and commas and line[i + 1:commas[0]].strip():
            end2 = commas[1] if len(commas) > 1 else j - 1
            if line[commas[0] + 1:end2].strip() in DUMMY_ARGS:
                line = line[:commas[0]] + line[end2:]
        i += 1


def drop_def(lines, i):
    """Comment out the function definition starting at line i (to its closing
    brace): a member the GCC side of a header does not declare."""
    depth, started, j = 0, False, i
    while j < len(lines):
        s = STRING_OR_LINE_COMMENT.sub('', lines[j])
        depth += s.count('{') - s.count('}')
        started = started or '{' in s
        lines[j] = '// gccfix: MSVC-only definition: ' + lines[j]
        if started and depth <= 0:
            return
        j += 1


def drop_arg2(line, callee):
    # Remove the second argument of every call to callee on this line.
    out, i = [], 0
    pat = re.compile(r'\b%s\s*\(' % re.escape(callee))
    while True:
        m = pat.search(line, i)
        if not m:
            out.append(line[i:])
            return ''.join(out)
        out.append(line[i:m.end()])
        j, depth, commas = m.end(), 1, []
        while j < len(line) and depth:
            c = line[j]
            if c in '([{':
                depth += 1
            elif c in ')]}':
                depth -= 1
            elif c == ',' and depth == 1:
                commas.append(j)
            j += 1
        if len(commas) >= 1:
            a, b = commas[0], (commas[1] if len(commas) > 1 else j - 1)
            out.append(line[m.end():a] + line[b:j])
        else:
            out.append(line[m.end():j])
        i = j


# Also a block comment that closes on its line: a replace rule's
# "/* gccfix: was: ... */" carries the old line, braces included, and those
# braces must not end (or extend) an alias's block.
STRING_OR_LINE_COMMENT = re.compile(r'"(?:\\.|[^"\\])*"|//.*$|/\*.*?\*/')


def alias_local(lines, want, name, serial):
    """Every block-scope declaration line equal to `want`: rename `name` to a
    local alias from that line to the end of its enclosing block, and bind the
    alias to the real symbol with an asm label. One C-linkage symbol, a
    differently-typed view per block -- what mwccarm compiled -- instead of
    one TU-wide type that some block then disagrees with."""
    word = re.compile(r'\b%s\b' % re.escape(name))
    i, found = 0, 0
    while i < len(lines):
        if lines[i].strip() != want:
            i += 1
            continue
        found += 1
        serial[0] += 1
        alias = '%s__l%d' % (name, serial[0])
        decl = word.sub(alias, lines[i], count=1)
        k = decl.rstrip().rfind(';')          # the label goes before the final ';'
        if k < 0:
            i += 1
            continue
        lines[i] = '%s __asm__("%s")%s' % (decl[:k], name, decl[k:])
        depth, j = 0, i + 1
        while j < len(lines):
            s = STRING_OR_LINE_COMMENT.sub('', lines[j])
            end = -1
            for pos, c in enumerate(s):
                if c == '{':
                    depth += 1
                elif c == '}':
                    depth -= 1
                    if depth < 0:
                        end = pos
                        break
            if end >= 0:                      # the enclosing block closes here
                lines[j] = word.sub(alias, lines[j][:end]) + lines[j][end:]
                break
            lines[j] = word.sub(alias, lines[j])
            j += 1
        i = j + 1
    return found


VARIADIC_DEF = re.compile(
    r'^(?P<head>[^\n;{}#]*?\b(?P<name>\w+)\s*)\((?P<params>[^()]*?),\s*\.\.\.\s*\)\s*\{', re.M)


def vararg_home(text):
    """A variadic definition that takes the address of a NAMED parameter to
    reach the arguments after it (`*(struct V2 *)&a`, `&fmt + 4`: a hand-rolled
    va_start). mwccarm spills r0-r3 contiguously, and on x86 everything is on
    the stack, so the named and anonymous arguments are one array. GCC on ARM
    saves only the anonymous registers there and spills a named one elsewhere.

    So the named parameters from the first address-taken one on are made
    anonymous (GCC then saves them in that same array), and each is rebound,
    for the rest of the body, through a pointer into it. Every such parameter
    is one word (checked; anything else is left alone and reported)."""
    out, pos, done = [], 0, []
    for m in VARIADIC_DEF.finditer(text):
        params = [p.strip() for p in m.group('params').split(',')]
        names = []
        for p in params:
            nm = re.search(r'(\w+)\s*$', p)
            names.append(nm.group(1) if nm else None)
        # the body: to the brace that closes the definition
        depth, j = 1, m.end()
        while j < len(text) and depth:
            depth += {'{': 1, '}': -1}.get(text[j], 0)
            j += 1
        body = text[m.end():j - 1]
        taken = [i for i, n in enumerate(names)
                 if n and re.search(r'&\s*%s\b' % re.escape(n), body)]
        if not taken:
            continue
        k = taken[0]
        moved = list(zip(params[k:], names[k:]))
        if any(re.search(r'\b(long\s+long|double|struct|union)\b', p) and '*' not in p
               for p, _ in moved):
            sys.stderr.write('gccfix: %s: a moved parameter is wider than a word; '
                             'left alone\n' % m.group('name'))
            continue
        kept = params[:k]
        sig = '%s(%s...)' % (m.group('head'), ''.join(p + ', ' for p in kept))
        # With no named parameter left (k == 0) the second operand is a
        # placeholder: GCC still points the list at the r0 save slot.
        prologue = ['__builtin_va_list gccfix_va; __builtin_va_start(gccfix_va, %s);'
                    % (names[k - 1] if k else '0'),
                    'char *gccfix_args = *(char **)&gccfix_va;']
        for i, (p, n) in enumerate(moved):
            ty = p[:p.rfind(n)].strip()
            prologue.append('%s *gccfix_p_%s = (%s *)(gccfix_args + %d);' % (ty, n, ty, 4 * i))
        for _, n in moved:
            body = re.sub(r'(?<![\w.>])%s\b' % re.escape(n), '(*gccfix_p_%s)' % n, body)
        out.append(text[pos:m.start()])
        out.append(sig + ' {  /* gccfix: vararg_home, named arguments read from the '
                   'variadic save area */\n    ' + ' '.join(prologue) + '\n' + body + '}')
        pos = j
        done.append(m.group('name'))
    if not done:
        return text
    out.append(text[pos:])
    return ''.join(out)


FUNC_DEF = re.compile(
    r'^(?P<ret>(?:[A-Za-z_][\w<>]*(?:::[A-Za-z_][\w<>]*)*[ 	\*&]+)+?)(?P<name>[A-Za-z_][\w:~]*)[ 	]*'
    r'\((?P<params>[^;{}()]*(?:\([^()]*\)[^;{}()]*)*)\)\s*(?:const\s*)?\{', re.M)


MSVC_IF = re.compile(r'#\s*if(?:def\s+_MSC_VER\b|\s+defined\s*\(?\s*_MSC_VER\b|\s+_MSC_VER\b)')


def body_end(text, start):
    """The index just past the `}` that closes the body opened before
    `start`, counting braces only in code GCC compiles: an `#ifdef _MSC_VER`
    arm is skipped up to its `#else`/`#endif` (both arms of
    src/_ZN9dScDSMT_c8BehaviorEv.cpp open a block that one `}` closes), and
    comments too. None when the body does not close."""
    depth, j, n = 1, start, len(text)
    stack = []          # per open #if: True while in the skipped MSVC arm
    line_start = True
    while j < n:
        c = text[j]
        if line_start:
            k = j
            while k < n and text[k] in ' \t':
                k += 1
            if k < n and text[k] == '#':
                e = text.find('\n', k)
                e = n if e < 0 else e
                d = text[k:e]
                if re.match(r'#\s*if', d):
                    stack.append(bool(MSVC_IF.match(d)))
                elif re.match(r'#\s*el(?:se|if)', d) and stack:
                    stack[-1] = False
                elif re.match(r'#\s*endif', d) and stack:
                    stack.pop()
                j = e
                continue
        line_start = c == '\n'
        if c == '/' and text.startswith('//', j):
            e = text.find('\n', j)
            j = n if e < 0 else e
            continue
        if c == '/' and text.startswith('/*', j):
            e = text.find('*/', j + 2)
            j = n if e < 0 else e + 2
            continue
        if not any(stack):
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if not depth:
                    return j + 1
        j += 1
    return None


def return_at_end(text):
    """C++ only: a non-void function that can flow off its end. The ROM's
    function hands back whatever r0 held (mwccarm emits nothing); GCC makes
    flowing off the end of a C++ function unreachable -- a trap (udf), or, as
    in BigBrickBlock::OnAttacked2 (early `return;`s, then `Kill();` and the
    closing brace), NO EPILOGUE AT ALL: the code ran on into the next function
    and returned through the stack into the block's own memory (it crashed
    punching Jolly Roger Bay's coin block). Value-initialise the result
    instead: `return {};` before the closing brace of every such body whose
    last statement is not a return (one after a final if/else that returns on
    both arms is dead code). Definitions at the start of a line only."""
    out, pos = [], 0
    for m in FUNC_DEF.finditer(text):
        ret = m.group('ret').strip()
        words = re.findall(r'\w+', ret)
        if not words or 'void' in words and '*' not in ret or \
                any(w in ('return', 'else', 'typedef', 'struct', 'class', 'union',
                          'enum', 'namespace', 'template', 'operator', 'asm', '__asm')
                for w in words):
            continue
        if m.group('name') in ('if', 'while', 'for', 'switch', 'do', 'else', 'return', 'sizeof')                 or any(w in ('if', 'while', 'for', 'switch', 'do', 'goto', 'case') for w in words):
            continue
        if '~' in m.group('name') or '&' in ret:
            continue
        j = body_end(text, m.end())
        if j is None:
            continue
        body = re.sub(r'//[^\n]*|/\*.*?\*/', '', text[m.end():j - 1], flags=re.S)
        # (an asm body leaves its result in r0 itself)
        if re.search(r'\b(?:asm|__asm__|__asm)\b', body):
            continue
        # its last statement a return, or a call that never comes back: done
        tail = re.sub(r'^\s*#.*$', '', body, flags=re.M).rstrip()
        if re.search(r'(?:\breturn\b|\b(?:throw|abort|exit|__builtin_trap|__debugbreak|'
                     r'__builtin_unreachable)\b)[^;{}]*;$', tail):
            continue
        out.append(text[pos:j - 1])
        out.append('    return {};  /* gccfix: the ROM falls off the end */\n')
        pos = j - 1
    if not out:
        return text
    out.append(text[pos:])
    return ''.join(out)


# The decomp's own vtable names. mwccarm's _ZTV symbols label a vtable's
# address point, so decomp code that names a table means that point (e.g.
# `*(void ***)stage = _ZTV5Stage;`, reading `_ZTVx[n]` as slot n). Where GCC
# emits the real table, the symbol is the Itanium start, 8 bytes earlier, so
# every such use is two slots off. So each table a decomp file names gets
# `#define _ZTVx _ZTVx__hostap` at the top of the file, before any #include,
# and linkfix.py resolves it per table: _ZTVx + 8 for a table GCC emitted,
# _ZTVx for one the HAL hosts as a plain array.
MW_NAME = re.compile(r'\b(_ZTV\w+)\b')


MW_COMMENT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)


# MSVC-only packs. Some src/ files restore the ROM's layout for MSVC with
#     #ifdef _MSC_VER
#     #pragma pack(push, 4)
#     #endif
# around records with 64-bit fields: mwccarm aligns long long to 4, MSVC and
# GCC's ARM EABI to 8. GCC needs the same pack, or the alarm record's fields
# shift and the cancel code writes into .text. So an `#ifdef _MSC_VER` block
# that only contains `#pragma pack(...)` lines is taken by GCC too.
MSC_PACK_BLOCK = re.compile(r'^([ \t]*)#[ \t]*ifdef[ \t]+_MSC_VER\b[^\n]*\n(.*?)^[ \t]*#[ \t]*endif\b',
                            re.M | re.S)


def msc_pack(text):
    def widen(m):
        body = MW_COMMENT.sub(' ', m.group(2))
        lines = [l.strip() for l in body.split('\n') if l.strip()]
        if not lines or not all(re.match(r'#\s*pragma\s+pack\s*\(', l) for l in lines):
            return m.group(0)
        head_end = m.group(0).index('\n')
        return m.group(1) + '#if defined(_MSC_VER) || defined(__GNUC__)' + m.group(0)[head_end:]
    return MSC_PACK_BLOCK.sub(widen, text)


def mw_vptr(text):
    # names the CODE uses: one named only in a comment would still rename the
    # headers' declarations, which can disagree on linkage
    code = MW_COMMENT.sub(' ', text)
    names = sorted({n for n in MW_NAME.findall(code) if not n.endswith('__hostap')})
    if not names:
        return text
    head = ''.join('#define %s %s__hostap\n' % (n, n) for n in names)
    if text.startswith('//cpp'):
        nl = text.find('\n') + 1
        return text[:nl] + head + text[nl:]
    return head + text


# hostgen's __fastcall calls. tools/hostgen.py's copies of src/ files call
# through the PC's thiscall-shaped vtables with a cast and a dummy EDX argument:
#     ((int (__fastcall *)(void *, void *, BMD_File *, int, int))vt[2])(self, 0, file, c, d)
# The dummy parameter and the dummy argument have to go together, or on ARM the
# 0 lands where `file` belongs. Only this exact shape is rewritten; anything
# else is left to drop_fastcall_dummy and the compiler.
def _match_close(t, i):
    depth = 0
    for j in range(i, len(t)):
        if t[j] == '(':
            depth += 1
        elif t[j] == ')':
            depth -= 1
            if depth == 0:
                return j
    return -1


def _split_args(inner):
    out, depth, cur = [], 0, ''
    for ch in inner:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    out.append(cur)
    return out


def fastcall_cast_calls(text):
    pos = 0
    while True:
        m = re.search(r'\(\s*__fastcall\s*\*\s*\)\s*\(', text[pos:])
        if not m:
            return text
        decl = pos + m.start()
        pos = decl + 1
        params_open = pos - 1 + m.end() - m.start() - 1
        params_close = _match_close(text, params_open)
        if params_close < 0:
            continue
        params = _split_args(text[params_open + 1:params_close])
        if len(params) < 2 or not re.fullmatch(r'\s*void\s*\*\s*\w*\s*', params[1]):
            continue
        # the cast's own '(' : the nearest unmatched '(' left of the declarator
        depth, k = 0, decl - 1
        while k >= 0:
            if text[k] == ')':
                depth += 1
            elif text[k] == '(':
                if depth == 0:
                    break
                depth -= 1
            k -= 1
        if k < 1:
            continue
        cast_close = _match_close(text, k)
        j = cast_close + 1
        while j < len(text) and text[j].isspace():
            j += 1
        if cast_close < 0 or j >= len(text):
            continue
        # the operand: a parenthesised expression or a name, then subscripts
        if text[j] == '(':
            operand_close = _match_close(text, j)
        else:
            mm = re.match(r'[\w.>\-]+', text[j:])
            if not mm:
                continue
            operand_close = j + mm.end() - 1
        while operand_close + 1 < len(text) and text[operand_close + 1] == '[':
            depth, q = 0, operand_close + 1
            while q < len(text):
                if text[q] == '[':
                    depth += 1
                elif text[q] == ']':
                    depth -= 1
                    if depth == 0:
                        break
                q += 1
            operand_close = q
        # the callee's own parentheses: "(" + cast + operand + ")" then "(args)"
        o = k - 1
        while o >= 0 and text[o].isspace():
            o -= 1
        c = operand_close + 1
        while c < len(text) and text[c].isspace():
            c += 1
        if o < 0 or text[o] != '(' or c >= len(text) or text[c] != ')':
            continue
        if _match_close(text, o) != c:
            continue
        a = c + 1
        while a < len(text) and text[a].isspace():
            a += 1
        if a >= len(text) or text[a] != '(':
            continue
        args_close = _match_close(text, a)
        args = _split_args(text[a + 1:args_close])
        if len(args) != len(params):
            continue
        del args[1]
        text = text[:a + 1] + ','.join(args).lstrip() + text[args_close:]
        del params[1]
        text = text[:params_open + 1] + ','.join(params).lstrip() + text[params_close:]


def fastcall_named_calls(text):
    """The same for a named pointer: `R (__fastcall *fn)(T, void *, ...) =
    (R (__fastcall *)(T, void *, ...)) expr; ... fn(a, 0, ...)` -- the dummy
    parameter goes from both declarators and the dummy argument from every
    later call of fn in the file."""
    m = re.search(r'\(\s*__fastcall\s*\*\s*(\w+)\s*\)\s*\(', text)
    while m:
        name = m.group(1)
        po = m.end() - 1
        pc = _match_close(text, po)
        params = _split_args(text[po + 1:pc]) if pc > 0 else []
        if len(params) >= 2 and re.fullmatch(r'\s*void\s*\*\s*\w*\s*', params[1]):
            n = len(params)
            del params[1]
            text = text[:po + 1] + ','.join(params).lstrip() + text[pc:]
            # the cast on the initialiser: the next (__fastcall *)( ... )
            cm = re.compile(r'\(\s*__fastcall\s*\*\s*\)\s*\(').search(text, po)
            if cm:
                co = cm.end() - 1
                cc = _match_close(text, co)
                cps = _split_args(text[co + 1:cc])
                if len(cps) == n:
                    del cps[1]
                    text = text[:co + 1] + ','.join(cps).lstrip() + text[cc:]
            call = re.compile(r'(?<![\w.>])%s\s*\(' % re.escape(name))
            k = po
            while True:
                c = call.search(text, k)
                if not c:
                    break
                ao = c.end() - 1
                ac = _match_close(text, ao)
                args = _split_args(text[ao + 1:ac])
                if len(args) == n:
                    del args[1]
                    text = text[:ao + 1] + ','.join(args).lstrip() + text[ac:]
                k = ao + 1
        m = re.compile(r'\(\s*__fastcall\s*\*\s*(\w+)\s*\)\s*\(').search(text, (m.start() + 1))
    return text


def ds_heads(text, heads):
    """A .dsstate group head pinned at its DS address mod 4 (tools/
    check_ds_align.py --fix writes the rules). Pad 0: the head itself word
    aligned. Pad p: p bytes, word aligned, in a section that sorts just before
    the head's, so the head lands p past a word boundary as on the DS (its own
    alignment is at most 2 when its address is not a multiple of 4)."""
    tail = []
    for r in heads:
        n, p = r['name'], int(r.get('pad', 0))
        if p == 0:
            tail.append('extern __typeof__(%s) %s __attribute__((aligned(4)));' % (n, n))
            continue
        m = None
        for c in re.finditer(r'"\.dsstate\$([^"]*)"[^;"]*?\b' + re.escape(n) + r'\b', text):
            line = text[text.rfind('\n', 0, c.start()) + 1:c.start()].lstrip()
            if not line.startswith(('//', '*', '/*', '#')):
                m = c
                break
        if not m:
            raise SystemExit('gccfix: ds_head %s: no .dsstate definition in the file' % n)
        sec = m.group(1)
        before = sec[:-1] + chr(ord(sec[-1]) - 1) + '~'
        tail.append('__declspec(allocate(".dsstate$%s")) __declspec(align(4)) static char '
                    'ds_pad_%s[%d] = {0};' % (before, n, p))
    if not tail:
        return text
    return text + '\n/* gccfix: .dsstate group heads pinned at their DS address mod 4 ' \
        '(tools/check_ds_align.py) */\n' + '\n'.join(tail) + '\n'


def fix_text(text, is_cxx, rules, pc_port=False, rom_style=None, hits=None):
    # hits: id(rule) -> the places it applied, for main's check that every
    # rule of a processed file still finds its line
    if hits is None:
        hits = {}

    def hit(r, n=1):
        hits[id(r)] = hits.get(id(r), 0) + n

    # ROM-style code names vtables mwccarm's way: the decomp, hostgen's copies,
    # and port/unmatched/ (the PC port's hand-written ROM stand-ins, which
    # linkfix also counts as game code)
    if rom_style is None:
        rom_style = not pc_port
    new = EXTERN_EXTERN.sub('extern "C"', text)
    if not pc_port and '...' in new:
        new = vararg_home(new)
    if not pc_port and is_cxx:
        new = return_at_end(new)
    if pc_port and '__fastcall' in new:
        new = drop_fastcall_dummy(new)
    if rom_style and '_MSC_VER' in new and 'pack' in new:
        new = msc_pack(new)
    if rom_style and '_ZTV' in new:
        new = mw_vptr(new)
    if not pc_port and '__fastcall' in new:
        new = fastcall_named_calls(fastcall_cast_calls(new))
    if is_cxx:
        # `extern struct X;` isn't valid C++. At file scope it just means struct X;,
        # and inside a function the decomp means the global struct X (a local one
        # would declare a new type). So drop those statements and forward-declare the
        # names once at the top of the file.
        names = []
        def take(m):
            if m.group(3) not in names:
                names.append(m.group(3))
            return m.group(1) + '/* gccfix: extern %s %s; */' % (m.group(2), m.group(3))
        new = EXTERN_STRUCT.sub(take, new)
        if names:
            # A plain local struct X; for a hoisted name would declare a NEW
            # function-local X and split the type again: comment it out too.
            new = re.sub(r'^(\s+)struct (%s)\s*;' % '|'.join(map(re.escape, names)),
                         r'\1/* gccfix: struct \2; */', new, flags=re.M)
            # Lines of their own ahead of the file (after a //cpp marker, which
            # must stay first). Rules are keyed by line TEXT, and gccdiag reads
            # line numbers from the copy GCC compiled, so the shift is harmless.
            head = ''.join('struct %s;\n' % n for n in names)
            if new.startswith('//cpp'):
                nl = new.find('\n') + 1
                new = new[:nl] + head + new[nl:]
            else:
                new = head + new
    for r in rules or ():
        if r['action'] == 'prepend':
            new = r['text'] + '\n' + new
            hit(r)
    if rules:
        lines = new.split('\n')
        # cast_arg: wrap the argument at a column in an explicit cast. The
        # column is relative to the line's indentation (so re-indentation does
        # not move it); several on one line apply right to left.
        casts = {}
        for r in rules:
            if r['action'] == 'cast_arg':
                casts.setdefault(r['line'].strip(), []).append(r)
        for i, l in enumerate(lines):
            rs = casts.get(l.strip())
            if not rs:
                continue
            ind = len(l) - len(l.lstrip())
            for r in sorted(rs, key=lambda x: -x['col']):
                hit(r)
                s = ind + r['col'] - 1
                depth, j = 0, s
                while j < len(l):
                    c = l[j]
                    if c in '([{':
                        depth += 1
                    elif c in ')]}':
                        if depth == 0:
                            break
                        depth -= 1
                    elif c == ',' and depth == 0:
                        break
                    j += 1
                if j > s:
                    l = l[:s] + '(%s)(%s)' % (r['type'], l[s:j]) + l[j:]
            lines[i] = l
        for r in rules:
            act = r['action']
            if act in ('rename', 'prepend', 'cast_arg', 'alias_local', 'ds_head'):
                continue
            want = r['line'].strip()
            for i, l in enumerate(lines):
                if l.strip() != want:
                    continue
                hit(r)
                if act == 'drop':
                    lines[i] = '/* gccfix: conflicting declaration dropped: */ // ' + l
                elif act == 'replace':
                    ind = l[:len(l) - len(l.lstrip())]
                    lines[i] = ind + r['with'] + '   /* gccfix: was: ' + l.strip().replace('*/', '* /') + ' */'
                elif act == 'drop_arg2':
                    lines[i] = drop_arg2(l, r['callee'])
                elif act == 'drop_dummy_args':
                    lines[i] = drop_dummy_args(l)
                elif act == 'drop_def':
                    drop_def(lines, i)
                    break
                elif act == 'extern':
                    lines[i] = re.sub(r'^(\s*)', r'\1extern ', l, count=1)
                elif act == 'split_init':
                    n = r['name']
                    m = re.match(r'^(\s*)(.*?\b' + re.escape(n) + r')\s*=\s*(.*);\s*$', l)
                    if m:
                        lines[i] = '%s%s; %s = %s;' % (m.group(1), m.group(2), n, m.group(3))
        serial = [0]
        for r in rules:
            if r['action'] == 'alias_local':
                hit(r, alias_local(lines, r['line'].strip(), r['name'], serial))
        new = '\n'.join(lines)
        for r in rules:
            if r['action'] == 'rename':
                new, n = re.subn(r'\b%s\b' % re.escape(r['name']), r['to'], new)
                hit(r, n)
        heads = [r for r in rules if r['action'] == 'ds_head']
        new = ds_heads(new, heads)   # stops the build itself on a missing head
        for r in heads:
            hit(r)
    return new


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True, help='paths are mirrored relative to this')
    ap.add_argument('--out', required=True)
    ap.add_argument('--list', required=True)
    ap.add_argument('--rules')
    ap.add_argument('--headers', action='append', default=[],
                    help='a PC-port header directory to mirror, transformed, under --out')
    ap.add_argument('--allow-unmatched', action='store_true',
                    help='report a rule that matched nothing instead of failing')
    a = ap.parse_args()
    root = os.path.abspath(to_win(a.root)).replace('\\', '/')
    out_win = to_win(a.out)

    rules_by_file = {}
    if a.rules and os.path.exists(to_win(a.rules)):
        for r in json.load(open(to_win(a.rules))):
            rules_by_file.setdefault(r['file'], []).append(r)

    hits, seen = {}, set()

    def write_if_changed(dst, text):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        old = None
        if os.path.exists(dst):
            old = open(dst, encoding='utf-8', errors='surrogateescape').read()
        if old != text:
            with open(dst, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
                f.write(text)

    # Headers of the PC port's own code, mirrored whole (changed or not), so a
    # mirrored source's #include same-dir.h and the mirror's place ahead of
    # the originals on the include path both find the transformed copy.
    for hd in a.headers:
        hdw = os.path.abspath(to_win(hd))
        for dirpath, _, files in os.walk(hdw):
            for fn in files:
                if not fn.endswith(('.h', '.hpp', '.inc')):
                    continue
                path = os.path.join(dirpath, fn)
                absn = os.path.normpath(path).replace('\\', '/')
                rel = os.path.relpath(absn, root).replace('\\', '/')
                text = open(path, encoding='utf-8', errors='surrogateescape').read()
                seen.add(rel)
                write_if_changed(os.path.join(out_win, rel),
                                 fix_text(text, False, rules_by_file.get(rel), True, hits=hits))

    for line in open(to_win(a.list), encoding='utf-8'):
        src = line.strip()
        if not src or not src.endswith(('.c', '.cpp')):
            continue
        win = to_win(src)
        try:
            text = open(win, encoding='utf-8', errors='surrogateescape').read()
        except OSError:
            continue
        absn = os.path.normpath(os.path.abspath(win)).replace('\\', '/')
        rel = os.path.relpath(absn, root).replace('\\', '/')
        lang = '-'
        is_cxx = src.endswith('.cpp')
        if src.endswith('.c') and text.lstrip().startswith('//cpp'):
            lang = 'CXX'
            is_cxx = True
        # The PC port's own code: port/ in the checkout, and the PC build's
        # generated non-src files (overlay data and tables written by its tools).
        pc_port = ('/port/' in absn and '/port-' in absn) or \
                  ('/host-src/' in absn and '/host-src/src/' not in absn)
        seen.add(rel)
        new = fix_text(text, is_cxx, rules_by_file.get(rel), pc_port,
                       rom_style=(not pc_port) or '/port/unmatched/' in absn, hits=hits)
        copy = '-'
        # PC-port sources are always mirrored, beside their mirrored headers.
        if new != text or (pc_port and a.headers):
            write_if_changed(os.path.join(out_win, rel), new)
            copy = '%s/%s' % (a.out.rstrip('/'), rel)
        if copy != '-' or lang != '-':
            print('%s\t%s\t%s' % (src, copy, lang))
    # A rule that matches nothing is a fix that stopped applying (the upstream
    # line it keys on changed), and the build would quietly go on without it. So
    # every rule for a processed file must apply somewhere; rules for files not in
    # this list belong to another target's run.
    dead = [r for rel in sorted(seen) for r in rules_by_file.get(rel, ()) if not hits.get(id(r))]
    for r in dead:
        key = r.get('line', r.get('name', r.get('text', '')))
        print('gccfix: rule matched nothing: %s: %s %r' % (r['file'], r['action'], key[:120]),
              file=sys.stderr)
    if dead and not a.allow_unmatched:
        print('gccfix: %d rule(s) matched nothing (--allow-unmatched only reports them)'
              % len(dead), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
