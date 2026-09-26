# W1-D's game-state gate check.  Two instruments, and the reported count is their sum.
#
#   gamestate_selftest  the SHIPPED sampler (src/engine/gamestate.cpp, linked in, not a copy of its
#                       reasoning) driven against a fake store: the S3.1 depths, the 27-row phase
#                       vocabulary, the Readable() discipline on every pointer change, and the
#                       refusal ladder.  The count is doctest's own assertion total, so deleting a
#                       CHECK lowers it.
#
#   tablecheck.py       the 27 phase rows re-read out of the shipped echovr.exe: the qword at each
#                       row's literal VA, and the OlPrEfIx name that follows it.  Two assertions
#                       per row.  An absent binary is MISSING, which is RED, never a silent zero.
#
# The live half (src/engine/gamestate_live.cpp -- the session's netGame pointer and the four
# anchored getters) needs a running echovr.exe and is NOT exercised here; it is owed an attended
# check, not a green line in this file.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

# Count floors.  tools\gate.ps1: a script check's GATE-RESULT count is self-reported and
# unverifiable, so each packet mutation-tests its own.  Both of these are: dropping a phase row
# takes the selftest red, and pointing tablecheck.py at an absent binary takes it MISSING.
# Raise them when checks are added; never lower one to make a run green.
$selftestFloor = 257
$tableFloor = 54

$failed = $false
$checked = 0

$exe = Join-Path (Join-Path $build 'bin') 'gamestate_selftest.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "no $exe - build first"
    Write-Host 'GATE-RESULT checked=0 subject=gamestate'
    exit 1
}

$out = & $exe --no-colors=true 2>&1
$code = $LASTEXITCODE
$out | ForEach-Object { Write-Host $_ }
if ($code -ne 0) { $failed = $true }

$assertions = 0
foreach ($line in $out) {
    if ($line -match 'assertions:\s+(\d+)\s*\|') { $assertions = [int]$matches[1] }
}
if ($assertions -lt $selftestFloor) {
    Write-Host "gamestate_selftest reported $assertions assertions, floor is $selftestFloor"
    $failed = $true
}
$checked += $assertions

$python = Get-Command python.exe -ErrorAction SilentlyContinue
if (-not $python) {
    Write-Host 'python.exe is not on PATH - tablecheck.py cannot run, and a skipped binary re-read is red'
    $failed = $true
} else {
    $script = Join-Path $repo 'tests/engine/gamestate/tablecheck.py'
    $tableOut = & $python.Source $script 2>&1
    $tableCode = $LASTEXITCODE
    $tableOut | ForEach-Object { Write-Host $_ }
    if ($tableCode -ne 0) { $failed = $true }
    $rows = 0
    foreach ($line in $tableOut) {
        if ($line -match '^TABLE (\d+) STATUS (\w+)$') {
            $rows = [int]$matches[1]
            if ($matches[2] -eq 'SKIPPED') { $tableSkipped = $true }
            elseif ($matches[2] -ne 'PASS') { $failed = $true }
        }
    }
    if (-not $tableSkipped -and $rows -lt $tableFloor) {
        Write-Host "tablecheck.py verified $rows table assertions, floor is $tableFloor"
        $failed = $true
    }
    $checked += $rows
}

Write-Host "GATE-RESULT checked=$checked subject=gamestate"
if ($failed) { exit 1 }
exit 0
