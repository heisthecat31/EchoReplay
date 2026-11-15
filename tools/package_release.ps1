<#
.SYNOPSIS
    Packs a release zip: EchoReplay.dll, tools\watch_replay and EchoReplayViewer with everything it reads.

.DESCRIPTION
    Build first: tools\build.ps1 (the plugin, echodemo.exe and echoreplay_reader.dll), then viewer\build.bat.
    The zip is what Spark downloads from GitHub releases: it looks for EchoReplay.dll and
    EchoReplayViewer.exe anywhere inside it.

    Layout:
        EchoReplay.dll              the plugin (Spark injects it, or copy it to bin\win10\plugins\)
        README.txt
        tools\watch_replay.bat/.ps1, echovr_path.ps1
        viewer\EchoReplayViewer.exe, echoreplay_reader.dll, echodemo.exe
        viewer\bake, data, ui, maps, script                 (copied there by the viewer's build)
        viewer\registry\level_roster.tsv, viewer\recon\name_recovery\   (from EchoConstruct: map names)

.PARAMETER Version
    The release tag, e.g. v1.1.0. The zip is release\EchoReplay-<Version>.zip.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\package_release.ps1 -Version v1.1.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$EchoConstructDir
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $EchoConstructDir) { $EchoConstructDir = Join-Path (Split-Path -Parent $repo) 'EchoConstruct' }

if ($Version -notmatch '^v\d+\.\d+\.\d+$') { throw "Version must look like v1.2.3 (got $Version)" }

$bin = Join-Path $repo 'build\bin'
$viewerBuild = Join-Path $repo 'viewer\build'
$need = @(
    (Join-Path $bin 'EchoReplay.dll'),
    (Join-Path $bin 'echodemo.exe'),
    (Join-Path $viewerBuild 'EchoReplayViewer.exe'),
    (Join-Path $viewerBuild 'echoreplay_reader.dll'),
    (Join-Path $EchoConstructDir 'registry\level_roster.tsv'),
    (Join-Path $EchoConstructDir 'recon\name_recovery\symbol_pool.txt'),
    (Join-Path $EchoConstructDir 'recon\name_recovery\level_names.json')
)
foreach ($f in $need) { if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f - build first" } }

$outDir = Join-Path $repo 'release'
$stage = Join-Path $outDir "EchoReplay-$Version"
$zip = Join-Path $outDir "EchoReplay-$Version.zip"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# the plugin
Copy-Item (Join-Path $bin 'EchoReplay.dll') $stage

# watching in the game
$tools = New-Item -ItemType Directory -Force -Path (Join-Path $stage 'tools')
foreach ($t in 'watch_replay.bat', 'watch_replay.ps1', 'echovr_path.ps1') {
    Copy-Item (Join-Path $repo "tools\$t") $tools
}

# the viewer
$viewer = New-Item -ItemType Directory -Force -Path (Join-Path $stage 'viewer')
Copy-Item (Join-Path $viewerBuild 'EchoReplayViewer.exe'), (Join-Path $viewerBuild 'echoreplay_reader.dll') $viewer
Copy-Item (Join-Path $bin 'echodemo.exe') $viewer
foreach ($d in 'bake', 'data', 'ui', 'maps', 'script') {
    $src = Join-Path $viewerBuild $d
    if (Test-Path -LiteralPath $src) { Copy-Item -Recurse $src (Join-Path $viewer $d) }
}
New-Item -ItemType Directory -Force -Path (Join-Path $viewer 'registry'), (Join-Path $viewer 'recon\name_recovery') | Out-Null
Copy-Item (Join-Path $EchoConstructDir 'registry\level_roster.tsv') (Join-Path $viewer 'registry')
Copy-Item (Join-Path $EchoConstructDir 'recon\name_recovery\symbol_pool.txt'), (Join-Path $EchoConstructDir 'recon\name_recovery\level_names.json') (Join-Path $viewer 'recon\name_recovery')

Copy-Item (Join-Path $repo 'LICENSE'), (Join-Path $repo 'THIRD_PARTY_NOTICES.md') $stage

@"
EchoReplay $Version

Record Echo VR matches as .echodemo files and watch them back, in the game or in EchoReplay Viewer.

RECORD
  With Spark: turn on Settings > Replays > Save to .echodemo format. Spark injects EchoReplay.dll into
  echovr.exe when the game starts, so no plugin loader is needed. If your game folder already has
  bin\win10\plugins\EchoReplay.dll, Spark keeps that copy updated instead.

  Without Spark: install EchoLoader and copy EchoReplay.dll into <game folder>\bin\win10\plugins\.

  Spark has its copy record to your Spark replay folder. A copy in bin\win10\plugins records to
  Documents\EchoReplay. EchoReplay Viewer finds your Spark replay folder and Echo VR install through
  Spark's settings (%APPDATA%\IgniteVR\Spark\settings.json) and lists Documents\EchoReplay as well.

WATCH
  viewer\EchoReplayViewer.exe plays recordings without the game (it reads maps from your Echo VR install).
  tools\watch_replay.bat opens one in the game itself (needs EchoReplay.dll in bin\win10\plugins).

Full documentation: https://github.com/heisthecat31/EchoReplay
"@ | Set-Content -Encoding UTF8 (Join-Path $stage 'README.txt')

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
$size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "Packed $zip ($size MB)"
