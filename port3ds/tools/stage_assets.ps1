# Copy the game data made from your own dump onto an SD card, in the layout
# the port reads (sdmc:/sm64ds3ds):
#
#   <sd>\sm64ds3ds\build\assets\*      files.tsv, handles.tsv, nitrofs tables
#   <sd>\sm64ds3ds\extracted\dsd\files  every NitroFS file
#
# Defaults to Azahar's virtual SD card. For a real console pass the card's
# drive:  powershell -File port3ds\tools\stage_assets.ps1 -Sd E:\
#
# Prerequisites (once, from the upstream checkout):
#   python tools/unpack.py <rom.nds>
#   python tools/asset_catalog.py generate <rom.nds>
#   python -I port3ds/tools/extract_files.py <rom.nds> <checkout>
param(
    [string]$Sd = (Join-Path $env:APPDATA 'Azahar\sdmc'),
    [string]$PortRoot = (Join-Path $PSScriptRoot '..\..\port-0.9.1')
)
$ErrorActionPreference = 'Stop'
$PortRoot = (Resolve-Path $PortRoot).Path
$dest = Join-Path $Sd 'sm64ds3ds'
foreach ($need in 'build\assets\files.tsv', 'build\assets\handles.tsv', 'extracted\dsd\files') {
    if (-not (Test-Path (Join-Path $PortRoot $need))) { throw "missing $need under $PortRoot (see the prerequisites in this script)" }
}
New-Item -ItemType Directory -Force (Join-Path $dest 'build\assets') | Out-Null
Copy-Item (Join-Path $PortRoot 'build\assets\*') (Join-Path $dest 'build\assets') -Recurse -Force
# The cartridge header's fields (hal/nitrofs_boot.cpp seeds the ROM-control word
# of the header mirror from it).
New-Item -ItemType Directory -Force (Join-Path $dest 'extracted\dsd') | Out-Null
Copy-Item (Join-Path $PortRoot 'extracted\dsd\header.yaml') (Join-Path $dest 'extracted\dsd') -Force
# robocopy only copies what changed; exit codes below 8 are success.
robocopy (Join-Path $PortRoot 'extracted\dsd\files') (Join-Path $dest 'extracted\dsd\files') /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
# The same files in one pack, which game3ds opens once instead of each file
# (game/files_pack.cpp: an SD card open costs ~30 ms; the castle grounds' boot
# opens ~180). Rewritten only when it would change.
& py -B (Join-Path $PSScriptRoot 'pack_files.py') (Join-Path $PortRoot 'extracted\dsd\files') (Join-Path $dest 'extracted\files.pak')
if ($LASTEXITCODE -ne 0) { throw "pack_files.py failed ($LASTEXITCODE)" }
$n = (Get-ChildItem (Join-Path $dest 'extracted\dsd\files') -Recurse -File | Measure-Object).Count
Write-Output "staged $n game files + their pack + asset catalog -> $dest"
exit 0
