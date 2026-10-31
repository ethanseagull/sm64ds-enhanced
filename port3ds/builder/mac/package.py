#!/usr/bin/env python3
"""Make the Mac builder: port3ds/builder/mac/build/SM64DS Enhanced Builder.app, and a zip of it.

  python3 port3ds/builder/mac/package.py                     # from the latest release
  python3 port3ds/builder/mac/package.py --from SM64DS-Enhanced-Builder.zip

The Mac app is the Windows builder's twin. The Swift here is a port of the
Windows builder's C# (Builder.cs, Settings.cs, Face.cs, and the extractor,
extract_assets.ps1, which upstream has since put into C# as Extract.cs too).
Everything else comes from the Windows release:

  game3ds.elf, game3ds.rsf, romdata.recipe.tsv, romdata.manifest, banner.wav,
  face.template, face.recipe, LABEL.txt     into Contents/Resources/payload
  music.mp3, done.mp3                       into Contents/Resources
  THIRD-PARTY.txt                           its licences, into the app's own

so the Mac app makes the same .cia from the same 3DS game as the Windows
builder, and ships nothing that ../package.py didn't check for Nintendo's
bytes. The Windows makerom.exe, bannertool.exe and extract_assets.ps1 stay
behind. --from takes the release zip (the .exe beside its builder-files
folder), a builder-files folder (such as ../package.py leaves in
build3ds-wide/SM64DS-Enhanced-Builder), or the first 1.0.0's single .exe,
which carried them all as .NET resources.

makerom 0.19.0 and bannertool 1.2.3 are built from source at pinned commits,
for arm64 and x86_64, for macOS 11 and later, and joined into universal
binaries in Contents/Helpers. (The official makerom Mac builds need macOS 15,
and bannertool has none.) --makerom and --bannertool take ready-made Mac
binaries instead.

Needs the Xcode command line tools (swiftc, clang, make, lipo, iconutil,
codesign, ditto) and, unless --makerom and --bannertool are given, git.
The app is signed ad hoc unless --sign names a Developer ID.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
APP_NAME = 'SM64DS Enhanced Builder'
EXECUTABLE = 'SM64DSEnhancedBuilder'
SWIFT = ['Builder.swift', 'Settings.swift', 'Face.swift', 'Extract.swift']
MIN_MACOS = '11.0'

RELEASES = 'https://api.github.com/repos/ethanseagull/sm64ds-enhanced/releases'
ZIP_NAME = 'SM64DS-Enhanced-Builder.zip'   # the .exe and its builder-files folder
EXE_NAME = 'SM64DS-Enhanced-Builder.exe'   # the first 1.0.0: everything inside the .exe
KIT = 'builder-files'
# what the Mac app has been checked against, byte for byte, with the Windows
# builder's C# run under Mono (extraction, RomFS, 3D banner, options.txt)
TESTED = {
    'd565831d267a6d0361d34aee0e176817f2226d3a4b23b26bf0deb6725874936a': f'v1.0.0, {ZIP_NAME} (31 Oct 2026, 18:40)',
    'd5daea4194606ce754859263a89022dd254c231ec29e34948138339ace25fe48': f'v1.0.0, {ZIP_NAME} (31 Oct 2026, 18:08)',
    '90b37d248702aa2b1b75fe5b71e408bf08200c86ea4b9d0302a454058a200f2e': f'v1.0.0, {EXE_NAME} (29 Oct 2026)',
}

MAKEROM_REPO = 'https://github.com/3DSGuy/Project_CTR.git'
MAKEROM_COMMIT = 'e8f5f529c54ff9b22a2491a480ffa69206bf7b19'      # tag makerom-v0.19.0
BANNERTOOL_REPO = 'https://github.com/carstene1ns/3ds-bannertool.git'
BANNERTOOL_COMMIT = 'da573b13418acd0ac1803b43b30e8a155e5f83e2'   # tag 1.2.3

# what the app takes from the release, and what it leaves behind
PAYLOAD = ['game3ds.elf', 'game3ds.rsf', 'romdata.recipe.tsv', 'romdata.manifest',
           'banner.wav', 'face.template', 'face.recipe', 'LABEL.txt']
SOUNDS = ['music.mp3', 'done.mp3']
NEEDED = PAYLOAD + SOUNDS + ['THIRD-PARTY.txt']
# not in the repository, so not in a builder of your own (../package.py); the
# app then plays nothing, and the HOME Menu banner is silent
OPTIONAL = ['banner.wav', 'music.mp3', 'done.mp3']
WINDOWS_ONLY = ['makerom.exe', 'bannertool.exe', 'extract_assets.ps1']
# the face.recipe lines Face.swift knows (Face.cs's)
RECIPE_KINDS = {'sm64ds-face-banner', 'model', 'file', 'window', 'logo', 'vtx', 'idx', 'tex',
                'rest', 'ibm', 'const', 'keys', 'lidc', 'lidk'}


def say(*a):
    print(*a, flush=True)


def run(cmd, **kw):
    kw.setdefault('check', True)
    return subprocess.run([str(c) for c in cmd], **kw)


def quiet(cmd, **kw):
    """run cmd, showing what it printed only if it fails"""
    p = subprocess.run([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)
    if p.returncode != 0:
        sys.stdout.write(p.stdout)
        sys.exit(f'{Path(str(cmd[0])).name} failed (exit code {p.returncode})')
    return p


def sha256(b):
    return hashlib.sha256(b).hexdigest()


# ---- the Windows release --------------------------------------------------------

def download(tag, work):
    """The release's zip (or, for the first 1.0.0, its .exe), checked against GitHub's digest."""
    url = f'{RELEASES}/latest' if tag == 'latest' else f'{RELEASES}/tags/{tag}'
    headers = {'User-Agent': 'sm64ds-enhanced-mac-package'}
    req = urllib.request.Request(url, headers=dict(headers, Accept='application/vnd.github+json'))
    with urllib.request.urlopen(req, timeout=60) as r:
        rel = json.load(r)
    assets = {a.get('name'): a for a in rel.get('assets', [])}
    asset = assets.get(ZIP_NAME) or assets.get(EXE_NAME)
    if not asset:
        sys.exit(f'release {rel.get("tag_name")} has neither {ZIP_NAME} nor {EXE_NAME}')
    say(f'downloading {asset["name"]} from release {rel.get("tag_name")}')
    req = urllib.request.Request(asset['browser_download_url'], headers=headers)
    with urllib.request.urlopen(req, timeout=300) as r:
        data = r.read()
    digest = asset.get('digest') or ''
    if digest.startswith('sha256:') and digest[7:] != sha256(data):
        sys.exit(f'the download of {asset["name"]} is damaged (its sha256 is not the release\'s)')
    dest = work / asset['name']
    dest.write_bytes(data)
    return dest


def net_resources(b):
    """The .NET manifest resources a PE carries, in order: [u32 length][data], 8-aligned."""
    def u16(o): return struct.unpack_from('<H', b, o)[0]
    def u32(o): return struct.unpack_from('<I', b, o)[0]
    if b[:2] != b'MZ':
        raise ValueError('not a Windows program')
    pe = u32(0x3c)
    if b[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE file')
    nsec, optsz = u16(pe + 6), u16(pe + 20)
    opt = pe + 24
    dirs = opt + (96 if u16(opt) == 0x10b else 112)
    clr_rva, _ = struct.unpack_from('<II', b, dirs + 14 * 8)
    if clr_rva == 0:
        raise ValueError('not a .NET program')
    sections = []
    for i in range(nsec):
        s = opt + optsz + i * 40
        vsize, va, rsize, raw = struct.unpack_from('<IIII', b, s + 8)
        sections.append((va, max(vsize, rsize), raw))

    def offset(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(f'RVA {rva:#x} is in no section')

    cli = offset(clr_rva)
    res_rva, res_size = struct.unpack_from('<II', b, cli + 24)
    if res_rva == 0:
        return []
    base = offset(res_rva)
    if base + res_size > len(b):
        raise ValueError('the resources run past the end of the file')
    out, at = [], 0
    while at + 4 <= res_size:
        n = u32(base + at)
        if at + 4 + n > res_size:
            raise ValueError('a resource runs past the resources')
        out.append(b[base + at + 4:base + at + 4 + n])
        at = (at + 4 + n + 7) & ~7
    return out


def looks_mp3(b):
    return len(b) > 1024 and (b[:3] == b'ID3' or (b[0] == 0xff and b[1] & 0xe0 == 0xe0))


def read_zip(data, what):
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        bad = z.testzip()
        if bad:
            sys.exit(f'{what} is damaged ({bad})')
        return {i.filename: z.read(i) for i in z.infolist() if not i.is_dir()}


def from_exe(data, name):
    """The first 1.0.0's .exe: payload.zip, music.mp3 and done.mp3 as resources, in
    that order (the order its package.py gave csc.exe them)."""
    try:
        res = net_resources(data)
    except (ValueError, struct.error) as e:
        sys.exit(f'{name}: {e}')
    if not res:
        sys.exit(f'{name} carries no files: it is a newer builder, which keeps them in its {KIT} '
                 f'folder; use the release zip, or that folder')
    if len(res) != 3:
        sys.exit(f'{name} has {len(res)} resources, not payload.zip, music.mp3 and done.mp3')
    zdata, music, done = res
    if zdata[:4] != b'PK\x03\x04':
        sys.exit(f'{name}\'s first resource is not payload.zip')
    files = read_zip(zdata, 'its payload.zip')
    files['music.mp3'], files['done.mp3'] = music, done
    return files


def gather(src):
    """{name: bytes} of the release's builder files, from a zip, a folder or an .exe."""
    if src.is_dir():
        kit = src / KIT if (src / KIT).is_dir() else src
        files = {p.name: p.read_bytes() for p in kit.iterdir() if p.is_file() and not p.name.startswith('.')}
    else:
        data = src.read_bytes()
        if data[:2] == b'MZ':
            files = from_exe(data, src.name)
        elif zipfile.is_zipfile(src):
            files = {}
            for path, b in read_zip(data, src.name).items():
                parts = path.split('/')
                if len(parts) >= 2 and parts[-2] == KIT and not parts[-1].startswith('.'):
                    files[parts[-1]] = b
            if not files:
                sys.exit(f'{src.name} has no {KIT} folder')
        else:
            sys.exit(f'{src.name} is not the release zip, a {KIT} folder or the builder .exe')
    missing = [n for n in NEEDED if n not in files and n not in OPTIONAL]
    if missing:
        sys.exit(f'{src.name} is missing ' + ', '.join(missing))
    absent = [n for n in OPTIONAL if n not in files]
    if absent:
        say(f'note: {src.name} has no ' + ', '.join(absent) + ', so the app will be without them')
    unknown = sorted(set(files) - set(NEEDED) - set(WINDOWS_ONLY))
    if unknown:
        sys.exit(f'{src.name} has files this script doesn\'t know: ' + ', '.join(unknown) +
                 ' (if the Windows builder uses them now, port that to port3ds/builder/mac and add them '
                 'to PAYLOAD in package.py)')
    for n in SOUNDS:
        if n in files and not looks_mp3(files[n]):
            sys.exit(f'{src.name}\'s {n} is not an MP3')
    return files


def check_payload(files):
    """Upstream package.py's check (the elf, the manifest and the recipe are one
    build's), and that Face.swift knows every line of face.recipe."""
    head = lambda n: files[n].decode('utf-8', 'replace').split('\n', 1)[0]
    m = re.match(r'# romdata-manifest v1 ([0-9a-f]{64}) (\d+)', head('romdata.manifest'))
    r = re.match(r'# romdata-recipe v1 ([0-9a-f]{64}) (\d+)', head('romdata.recipe.tsv'))
    if not (m and r and m.groups() == r.groups()):
        sys.exit('the release\'s romdata.manifest and romdata.recipe.tsv are not one build\'s')
    if files['game3ds.elf'][:4] != b'\x7fELF':
        sys.exit('the release\'s game3ds.elf is not an ELF file')
    if m.group(1).encode() not in files['game3ds.elf']:
        sys.exit('the release\'s game3ds.elf expects another romdata.bin')
    kinds = {line.split()[0] for line in files['face.recipe'].decode('utf-8').splitlines()
             if line.strip() and not line.startswith('#')}
    if kinds - RECIPE_KINDS:
        sys.exit('face.recipe has lines Face.swift doesn\'t know (' + ', '.join(sorted(kinds - RECIPE_KINDS)) +
                 '): port the Windows builder\'s Face.cs changes to port3ds/builder/mac/Face.swift')
    label = files['LABEL.txt'].decode('utf-8').strip()
    if not re.fullmatch(r'[0-9A-Za-z][0-9A-Za-z.+_-]{0,31}', label):
        sys.exit(f'the release\'s LABEL.txt is odd: {label!r}')
    return label


# ---- makerom and bannertool -----------------------------------------------------

def fetch(repo, commit, dest):
    """dest checked out at commit (and only that commit)."""
    def head():
        p = run(['git', '-C', dest, 'rev-parse', 'HEAD'], capture_output=True, text=True, check=False)
        return p.stdout.strip() if p.returncode == 0 else None

    def clean():
        p = run(['git', '-C', dest, 'status', '--porcelain', '--ignored'], capture_output=True, text=True)
        return p.stdout.strip() == ''

    if dest.exists() and (head() != commit or not clean()):
        shutil.rmtree(dest)
    if not dest.exists():
        say(f'fetching {repo} at {commit[:12]}')
        dest.mkdir(parents=True)
        run(['git', '-C', dest, 'init', '-q'])
        run(['git', '-C', dest, 'fetch', '-q', '--depth', '1', repo, commit])
        run(['git', '-C', dest, '-c', 'advice.detachedHead=false', 'checkout', '-q', 'FETCH_HEAD'])
    if head() != commit:
        sys.exit(f'{dest} is not at {commit}')


def build_makerom(src, work, archs, jobs):
    """Project_CTR's own build (its CI's `make PROJECT_PLATFORM_ARCH=... deps all`), one per arch."""
    outs = []
    env = dict(os.environ, MACOSX_DEPLOYMENT_TARGET=MIN_MACOS)
    for arch in archs:
        tree = work / f'makerom-{arch}'
        if tree.exists():
            shutil.rmtree(tree)
        shutil.copytree(src / 'makerom', tree)
        say(f'building makerom for {arch}')
        for target in ('deps', 'all'):
            quiet(['make', f'-j{jobs}', f'PROJECT_PLATFORM_ARCH={arch}', 'WARNFLAGS=-w', target],
                  cwd=tree, env=env)
        outs.append(tree / 'bin' / 'makerom')
    return outs


def build_bannertool(src, work, archs, jobs):
    """bannertool's CMakeLists.txt (a Release build), done with clang++ directly."""
    out = work / 'bannertool'
    objs = work / 'bannertool-obj'
    if objs.exists():
        shutil.rmtree(objs)
    objs.mkdir(parents=True)
    common = ['clang++', '-std=c++17', '-O3', '-DNDEBUG', '-w', f'-mmacosx-version-min={MIN_MACOS}']
    for a in archs:
        common += ['-arch', a]
    s = src / 'source'
    # the libraries build without their INTERFACE definitions; bannertool builds with them
    libs = [s / 'pc' / f for f in ('stb_image.cpp', 'stb_vorbis.cpp', 'dr_wav.cpp')]
    exe = [s / f for f in ('main.cpp', 'log.cpp', 'types.cpp', 'utils.cpp',
                           '3ds/cbmd.cpp', '3ds/cwav.cpp', '3ds/lz11.cpp')]
    uses = ['-DSTBI_ONLY_PNG', '-DSTBI_NO_LINEAR', '-DSTBI_NO_STDIO',
            '-DSTB_VORBIS_NO_PUSHDATA_API', '-DSTB_VORBIS_NO_STDIO', '-DDR_WAV_NO_STDIO',
            '-DVERSION="1.2.3"', f'-I{s}', f'-I{s / "pc"}']
    # clang (unlike gcc, which bannertool's releases are built with) won't
    # initialise a variable-length array; the padding is never more than 3 bytes
    vla = '\t\tu8 pad[padLength] = {0};\n'
    lz11 = (s / '3ds' / 'lz11.cpp').read_text()
    if lz11.count(vla) != 1:
        sys.exit('bannertool\'s lz11.cpp is not the one this script knows how to build')
    patched = objs / 'lz11.cpp'
    patched.write_text(lz11.replace(vla, '\t\tu8 pad[4] = {0};\n'))
    exe[exe.index(s / '3ds/lz11.cpp')] = patched
    uses.append(f'-I{s / "3ds"}')
    say(f'building bannertool for {" and ".join(archs)}')
    jobs_ = []
    for i, f in enumerate(libs + exe):
        o = objs / f'{i:02d}-{f.stem}.o'
        flags = [f'-I{s / "pc"}'] if f in libs else uses
        jobs_.append(subprocess.Popen([*common, *flags, '-c', str(f), '-o', str(o)]))
        if len(jobs_) >= jobs:
            if jobs_.pop(0).wait() != 0:
                sys.exit('bannertool did not build')
    if any(p.wait() != 0 for p in jobs_):
        sys.exit('bannertool did not build')
    run([*common, *sorted(objs.glob('*.o')), '-o', out])
    return out


def universal(parts, out):
    if len(parts) == 1:
        shutil.copy2(parts[0], out)
    else:
        run(['lipo', '-create', *parts, '-output', out])
    quiet(['strip', '-S', '-x', out])   # (signed again in sign())
    out.chmod(0o755)


def check_binary(path, archs):
    """path is a Mach-O executable for every arch in archs, needing at most MIN_MACOS."""
    have = run(['lipo', '-archs', path], capture_output=True, text=True).stdout.split()
    if sorted(have) != sorted(archs):
        sys.exit(f'{path.name} is for {" ".join(have)}, not {" ".join(archs)}')
    for a in archs:
        info = run(['otool', '-arch', a, '-l', path], capture_output=True, text=True).stdout
        found = re.findall(r'\bminos (\d+)\.(\d+)', info)
        if not found:
            sys.exit(f'{path.name} ({a}) says no minimum macOS')
        need = max((int(x), int(y)) for x, y in found)
        if need > tuple(int(x) for x in MIN_MACOS.split('.')):
            sys.exit(f'{path.name} ({a}) needs macOS {need[0]}.{need[1]}; it should run on {MIN_MACOS}')


# ---- the app -----------------------------------------------------------------

def build_swift(work, archs):
    parts = []
    for a in archs:
        out = work / f'{EXECUTABLE}-{a}'
        say(f'compiling the builder for {a}')
        run(['swiftc', '-swift-version', '5', '-O', '-target', f'{a}-apple-macos{MIN_MACOS}',
             '-o', out, *[HERE / f for f in SWIFT]])
        parts.append(out)
    return parts


def info_plist(label, bundle_id):
    usage = 'The builder reads your Super Mario 64 DS dump and saves the .cia next to it.'
    return {
        'CFBundleDevelopmentRegion': 'en',
        'CFBundleExecutable': EXECUTABLE,
        'CFBundleIconFile': 'AppIcon',
        'CFBundleIdentifier': bundle_id,
        'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleName': APP_NAME,
        'CFBundleDisplayName': APP_NAME,
        'CFBundlePackageType': 'APPL',
        'CFBundleShortVersionString': label,
        'CFBundleVersion': label,
        'LSMinimumSystemVersion': MIN_MACOS,
        'LSApplicationCategoryType': 'public.app-category.utilities',
        'NSHighResolutionCapable': True,
        'NSPrincipalClass': 'NSApplication',
        'NSSupportsAutomaticTermination': False,
        'NSSupportsSuddenTermination': False,
        # dropping a dump on the Dock icon, or Open With
        'CFBundleDocumentTypes': [{
            'CFBundleTypeName': 'Nintendo DS dump',
            'CFBundleTypeRole': 'Viewer',
            'CFBundleTypeExtensions': ['nds'],
            'LSHandlerRank': 'Alternate',
        }],
        # what macOS shows when it asks before the builder reads Desktop, Documents,
        # Downloads, a USB stick or a network share
        'NSDesktopFolderUsageDescription': usage,
        'NSDocumentsFolderUsageDescription': usage,
        'NSDownloadsFolderUsageDescription': usage,
        'NSRemovableVolumesUsageDescription': usage,
        'NSNetworkVolumesUsageDescription': usage,
    }


def third_party(payload_text):
    """The Mac header, then the payload's licences, then the ones makerom brings on a Mac."""
    cut = payload_text.find('==== bannertool')
    if cut < 0:
        sys.exit('the payload\'s THIRD-PARTY.txt has no bannertool section')
    text = (HERE / 'THIRD-PARTY.txt').read_text(encoding='utf-8').rstrip() + '\n\n\n'
    text += payload_text[cut:].replace('\r\n', '\n').rstrip() + '\n'
    for name, title in (('mbedtls', 'mbedtls (in makerom)'), ('libyaml', 'libyaml (in makerom)'),
                        ('gpl-3.0', 'GNU General Public License 3 (CUE\'s blz, in makerom)')):
        text += f'\n\n==== {title} ====\n\n' + (HERE / 'licenses' / f'{name}.txt').read_text(encoding='utf-8').rstrip() + '\n'
    return text


def sign(app, identity):
    helpers = sorted((app / 'Contents' / 'Helpers').iterdir())
    extra = [] if identity == '-' else ['--options', 'runtime', '--timestamp']
    for h in helpers:
        quiet(['codesign', '--force', '--sign', identity, *extra, h])
    quiet(['codesign', '--force', '--sign', identity, *extra, app])
    quiet(['codesign', '--verify', '--deep', '--strict', app])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--from', dest='source', type=Path, metavar='PATH',
                    help=f'the Windows release to take the game from: {ZIP_NAME}, its {KIT} folder, '
                         f'or the first 1.0.0\'s {EXE_NAME} (default: download the release)')
    ap.add_argument('--release', default='latest', help='the release to download, by tag (default: latest)')
    ap.add_argument('--makerom', type=Path, help='a Mac makerom 0.19.0 to use instead of building it')
    ap.add_argument('--bannertool', type=Path, help='a Mac bannertool 1.2.3 to use instead of building it')
    ap.add_argument('--arch', choices=['universal', 'arm64', 'x86_64'], default='universal')
    ap.add_argument('--sign', default='-', metavar='IDENTITY',
                    help='a codesign identity, such as "Developer ID Application: ..." (default: ad hoc)')
    ap.add_argument('--bundle-id', default='io.github.ethanseagull.sm64ds-enhanced.builder')
    ap.add_argument('--out', type=Path, default=HERE / 'build',
                    help='where to build (default: port3ds/builder/mac/build)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    archs = ['arm64', 'x86_64'] if a.arch == 'universal' else [a.arch]
    out = a.out.resolve()
    work = out / 'work'
    work.mkdir(parents=True, exist_ok=True)

    # 1. the game, from the Windows release
    src = a.source or download(a.release, work)
    files = gather(src)
    label = check_payload(files)
    if src.is_file():
        src_sha = sha256(src.read_bytes())
        say(f'{src.name}: sha256 {src_sha}')
        if src_sha not in TESTED:
            say('note: the Mac builder hasn\'t been checked against this release; if the Windows '
                'builder\'s C# changed since ' + ' and '.join(sorted(TESTED.values())) +
                ', port the changes to port3ds/builder/mac')
    say(f'the game: build {label}')

    # 2. makerom and bannertool
    if a.makerom:
        makerom = [a.makerom]
    else:
        fetch(MAKEROM_REPO, MAKEROM_COMMIT, work / 'src' / 'Project_CTR')
        makerom = build_makerom(work / 'src' / 'Project_CTR', work, archs, a.jobs)
    if a.bannertool:
        bannertool = a.bannertool
    else:
        fetch(BANNERTOOL_REPO, BANNERTOOL_COMMIT, work / 'src' / '3ds-bannertool')
        bannertool = build_bannertool(work / 'src' / '3ds-bannertool', work, archs, a.jobs)

    # 3. the builder itself
    exe_parts = build_swift(work, archs)

    # 4. the bundle
    app = out / f'{APP_NAME}.app'
    if app.exists():
        shutil.rmtree(app)
    contents = app / 'Contents'
    for d in ('MacOS', 'Helpers', 'Resources/payload'):
        (contents / d).mkdir(parents=True)
    universal(exe_parts, contents / 'MacOS' / EXECUTABLE)
    universal(makerom, contents / 'Helpers' / 'makerom')
    universal([bannertool], contents / 'Helpers' / 'bannertool')
    for p in (contents / 'MacOS' / EXECUTABLE, contents / 'Helpers' / 'makerom', contents / 'Helpers' / 'bannertool'):
        check_binary(p, archs)
    res = contents / 'Resources'
    for name in PAYLOAD:
        if name in files:
            (res / 'payload' / name).write_bytes(files[name])
    for name in SOUNDS:
        if name in files:
            (res / name).write_bytes(files[name])
    (res / 'THIRD-PARTY.txt').write_text(third_party(files['THIRD-PARTY.txt'].decode('utf-8')), encoding='utf-8')
    with open(contents / 'Info.plist', 'wb') as f:
        plistlib.dump(info_plist(label, a.bundle_id), f)
    (contents / 'PkgInfo').write_text('APPL????')

    # the icon, drawn by the builder itself (Look.appIcon)
    iconset = work / 'AppIcon.iconset'
    if iconset.exists():
        shutil.rmtree(iconset)
    run([contents / 'MacOS' / EXECUTABLE, '--icon', iconset])
    run(['iconutil', '-c', 'icns', '-o', res / 'AppIcon.icns', iconset])

    # 5. the helpers answer, then sign and zip
    for tool, want in (('makerom', 'makerom'), ('bannertool', 'bannertool')):
        p = run([contents / 'Helpers' / tool], capture_output=True, text=True, check=False)
        if want not in (p.stdout + p.stderr).lower():
            sys.exit(f'Helpers/{tool} does not run')
    sign(app, a.sign)
    z = out / f'SM64DS-Enhanced-Builder-{label}-mac.zip'
    if z.exists():
        z.unlink()
    run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', app, z])
    say(f'{app}')
    say(f'{z}: {z.stat().st_size:,} bytes, sha256 {sha256(z.read_bytes())[:16]}')


if __name__ == '__main__':
    main()
