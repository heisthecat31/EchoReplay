# Proves, for one run of the panel suite, that the owner's real Documents\EchoReplay is
# not written to -- and proves FIRST that the instrument can say otherwise, by running the same guard
# over a run that DOES write there (USERPROFILE pointed at a scratch root, so the control never goes
# near the owner's folder).
#
# A missing binary is REFUSED, never a pass: a guard that could not ask its question must not report
# a clean folder.
param(
    [switch]$SelfTest
)

# NOT 'Stop': a native process writing to stderr would otherwise abort this script before it can
# record anything, turning a real failure into a run with no verdict.
$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)
if ($env:REPLAY_REPO_ROOT) { $repo = $env:REPLAY_REPO_ROOT }
$buildDir = Join-Path $repo 'build'
if ($env:REPLAY_BUILD_DIR) { $buildDir = $env:REPLAY_BUILD_DIR }

$fails = @()
function Add-Fail($m) { $script:fails += $m }

function Get-TargetPath([string]$name) {
    $f = Join-Path $buildDir ($name + '_path.txt')
    if (-not (Test-Path $f)) {
        Add-Fail ($name + ' was not built; expected its path in ' + $f)
        return ''
    }
    $p = (Get-Content $f -Raw).Trim()
    if (-not (Test-Path $p)) {
        Add-Fail ($name + "'s path file names a file that does not exist: " + $p)
        return ''
    }
    return $p
}

$guard = Get-TargetPath 'owner_folder_guard'
$panel = Get-TargetPath 'ui_paneltest'

if ($guard) {
    # ---- positive control: the guard must RED on a run that writes into the resolved folder ----
    $controlRoot = Join-Path $buildDir 'ownerfolder_control'
    $controlDir = Join-Path $controlRoot 'Documents\EchoReplay'
    if (Test-Path $controlRoot) { Remove-Item -Recurse -Force $controlRoot }
    New-Item -ItemType Directory -Force $controlDir | Out-Null
    $stray = Join-Path $controlDir 'control_stray.echodemo'
    $savedProfile = $env:USERPROFILE
    $text = ''
    $code = -1
    try {
        $env:USERPROFILE = $controlRoot
        $out = & $guard 'cmd.exe' '/c' ('copy /y nul "' + $stray + '" >nul') 2>&1
        $code = $LASTEXITCODE
        $text = ($out | Out-String)
    } finally {
        $env:USERPROFILE = $savedProfile
    }
    if ($code -ne 1) {
        Add-Fail ('positive control: the guard exited ' + $code + ' over a run that wrote control_stray.echodemo; it must exit 1. Output: ' + $text)
    }
    if ($text -notmatch 'GUARD NEW control_stray\.echodemo') {
        Add-Fail ('positive control: the guard did not name control_stray.echodemo. Output: ' + $text)
    }
    if ($text -notmatch 'GUARD VERDICT=STRAYS') {
        Add-Fail ('positive control: no STRAYS verdict. Output: ' + $text)
    }
    Remove-Item -Recurse -Force $controlRoot -ErrorAction SilentlyContinue

    # ---- the real question: does the panel suite leave anything in the owner's folder? ----
    if ($panel) {
        $out = & $guard $panel 2>&1
        $code = $LASTEXITCODE
        $text = ($out | Out-String)
        if ($text -notmatch 'GUARD VERDICT=(CLEAN|STRAYS)') {
            Add-Fail ('the guard produced no verdict over ui_paneltest, so the folder was not compared. Output: ' + $text)
        }
        if ($code -eq 1) {
            $names = (($out | Select-String -Pattern '^GUARD (NEW|CHANGED) ') -join '; ')
            Add-Fail ("ui_paneltest left files in the owner's capture folder: " + $names)
        } elseif ($code -ne 0) {
            Add-Fail ('the guard exited ' + $code + ' over ui_paneltest (2 = it could not ask). Output: ' + $text)
        }
        if ($text -notmatch 'GUARD child-exit=0') {
            Add-Fail ('ui_paneltest itself failed under the guard, so its writes are not a complete sample. Output: ' + $text)
        }
    }
}

if ($fails.Count -gt 0) {
    foreach ($f in $fails) { Write-Host ('FAIL: ' + $f) }
    Write-Host ('hygiene: FAIL (' + $fails.Count + ')')
    exit 1
}
Write-Host "hygiene: PASS -- the panel suite left the owner's capture folder byte-identical, and the guard reds when a run does not"
exit 0
