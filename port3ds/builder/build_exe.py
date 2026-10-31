#!/usr/bin/env python3
"""Compile SM64DS-Enhanced-Builder.exe from this folder's source alone.

  py -I port3ds/builder/build_exe.py --label 1.0.0 --out SM64DS-Enhanced-Builder.exe

Builder.cs, Extract.cs, Face.cs and Settings.cs, compiled by the C# 5 compiler
in every Windows (Framework64/v4.0.30319/csc.exe), with builder.ico as its
icon and the label as its version. It needs nothing else of the project, so
the release's .exe is built by GitHub Actions from the public repository
(.github/workflows/builder.yml), where it can be signed; package.py runs this
too, for the local zip.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
CSC = Path(r'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe')
SOURCES = ('Builder.cs', 'Extract.cs', 'Face.cs', 'Settings.cs')


def build(label, out):
    label = label[1:] if re.match(r'v\d', label) else label   # a tag, v1.0.0
    nums = (re.findall(r'\d+', label) + ['0'] * 4)[:4]
    out = Path(out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        # the file's version resource: Explorer's Details tab, and what scanners read
        info = Path(tmp) / 'AssemblyInfo.cs'
        info.write_text(
            'using System.Reflection;\n'
            f'[assembly: AssemblyVersion("{".".join(nums)}")]\n'
            f'[assembly: AssemblyFileVersion("{".".join(nums)}")]\n'
            f'[assembly: AssemblyInformationalVersion("{label}")]\n'
            '[assembly: AssemblyDescription("Builds Super Mario 64 DS Enhanced for the 3DS from your own '
            'cartridge dump")]\n'
            '[assembly: AssemblyCopyright("ethanseagull; MIT and others, see THIRD-PARTY.txt")]\n')
        subprocess.run([str(CSC), '/nologo', '/target:winexe', '/optimize+', f'/out:{out}',
                        f'/win32icon:{HERE / "builder.ico"}', '/r:System.Windows.Forms.dll',
                        '/r:System.Drawing.dll', *[str(HERE / s) for s in SOURCES], str(info)], check=True)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--label', required=True, help='the release (1.0.0, or its tag v1.0.0)')
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    print(build(a.label, a.out))


if __name__ == '__main__':
    main()
