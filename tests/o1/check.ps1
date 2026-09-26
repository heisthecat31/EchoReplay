# gate-tier: T1
#
# W1-A's gate entry point. Discovered by tools\gate.ps1 by convention: tests\<name>\check.ps1.
#
# Three sub-checks, each judged twice -- by its exit code AND by what it reported covering. A check that
# exits 0 having covered nothing must never be indistinguishable from a check that passed.
#
#   constcheck   every quantisation constant the codec compiles with, read back out of echovr.exe
#   o1 baseline  the round-trip oracle, full sweeps
#   mutations    the generated battery, every mutation classified CAUGHT or EQUIVALENT
#
# ★ The GATE-RESULT count is itself mutation-tested. -SelfTest re-runs the harness with one field removed
# from the domain the count measures and fails unless the number drops by exactly one. A substance count
# nobody has watched move is not a substance count.

[CmdletBinding()]
param(
    [switch]$SelfTest,
    [switch]$Fast          # skip the two 2^32 sweeps; for iterating, never for a gate run
)

# NOT 'Stop': under 'Stop' a native process writing to stderr raises NativeCommandError and aborts this
# script before it can record anything, turning a real failure into a run that produced no verdict.
$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)
if ($env:REPLAY_REPO_ROOT) { $repo = $env:REPLAY_REPO_ROOT }
$buildDir = Join-Path $repo 'build\W1-A'
if ($env:REPLAY_BUILD_DIR) { $buildDir = $env:REPLAY_BUILD_DIR }

$artifactDir = Join-Path $buildDir 'W1-A'
New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
$artifact = Join-Path $artifactDir 'o1_result.txt'

# Stamped FAIL before anything runs: a crashed run must not leave the previous run's PASS on disk for
# something else to read as this run's verdict.
Set-Content -Path $artifact -Encoding utf8 -Value @(
    ("timestamp: " + (Get-Date -Format 'o')),
    'VERDICT FAIL',
    'check.ps1 did not finish; this line is replaced only once every sub-check has reported.')

$failed = New-Object System.Collections.ArrayList
$summary = New-Object System.Collections.ArrayList
$checkedTotal = 0

function Add-Fail([string]$m) { [void]$failed.Add($m) }
function Add-Note([string]$m) { [void]$summary.Add($m) }

# ---------------------------------------------------------------- constcheck

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    Add-Fail 'python is not on PATH, so the constants cannot be read back out of echovr.exe'
} else {
    $out = & $python.Source (Join-Path $here 'constcheck.py') 2>&1
    $code = $LASTEXITCODE
    $line = ($out | Where-Object { $_ -match '^RESULT constcheck' } | Select-Object -Last 1)
    $rows = ($out | Where-Object { $_ -match '^CONSTCHECK ' } | Select-Object -Last 1)
    if ($rows) { Add-Note ([string]$rows) }
    if (-not $line) {
        Add-Fail 'constcheck printed no RESULT line'
        Write-Host ($out | Out-String)
    } elseif ($line -match 'status=NOT-RUN .*reason=echovr\.exe is not on this machine') {
        Write-Host 'SKIP constcheck: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)'
        Add-Note ('constcheck skipped: ' + $line)
    } elseif ($line -match 'status=NOT-RUN') {
        Add-Fail ("constcheck: " + $line)
    } elseif ($code -ne 0 -or $line -notmatch 'status=PASS') {
        Add-Fail ("constcheck: " + $line)
        Write-Host ($out | Out-String)
    } else {
        if ($line -match '\bchecked=(\d+)') { $n = [int]$matches[1] } else { $n = 0 }
        if ($n -lt 20) {
            Add-Fail "constcheck reported checked=$n; it guards 22 constants, so anything less means the check stopped seeing them"
        } else {
            $checkedTotal += $n
        }
        Add-Note ([string]$line)
    }
}

# ---------------------------------------------------------------- locate the harness

$pathFile = Join-Path $buildDir 'o1_harness_path.txt'
$exe = $null
if (Test-Path -LiteralPath $pathFile) {
    $exe = (Get-Content -LiteralPath $pathFile -Raw).Trim()
}
$corpus = Join-Path $here 'corpus\arena_idle.tsv'

if (-not $exe -or -not (Test-Path -LiteralPath $exe)) {
    Add-Fail "the o1 harness is not built; expected its path in $pathFile"
} elseif (-not (Test-Path -LiteralPath $corpus)) {
    Add-Fail "the stored corpus is missing: $corpus"
} else {
    # ------------------------------------------------------------ baseline
    $args = @("--corpus=$corpus")
    if ($Fast) { $args += '--fast' }
    $out = & $exe @args 2>&1
    $code = $LASTEXITCODE
    $res = ($out | Where-Object { $_ -match '^O1-RESULT ' } | Select-Object -Last 1)
    foreach ($c in ($out | Where-Object { $_ -match '^O1-COVER ' })) { Add-Note ([string]$c) }
    if (-not $res) {
        Add-Fail 'the o1 harness printed no O1-RESULT line'
        Write-Host ($out | Out-String)
    } elseif ($code -ne 0 -or $res -notmatch 'status=PASS') {
        Add-Fail ("o1 baseline: " + $res)
        foreach ($f in ($out | Where-Object { $_ -match '^O1-FAIL ' })) { Write-Host ([string]$f) }
    } else {
        if ($res -match '\bchecked=(\d+)') { $baseChecked = [int]$matches[1] } else { $baseChecked = 0 }
        if ($baseChecked -le 0) {
            Add-Fail 'the o1 baseline reported checked=0'
        } else {
            $checkedTotal += $baseChecked
        }
        Add-Note ([string]$res)

        # -------------------------------------------------------- count mutation
        # Remove one item from the domain the count measures and require the number to drop.
        $drop = 'player.restposeheight'
        $dropOut = & $exe "--corpus=$corpus" "--drop-field=$drop" '--fast' 2>&1
        $dropRes = ($dropOut | Where-Object { $_ -match '^O1-RESULT ' } | Select-Object -Last 1)
        if ($dropRes -and $dropRes -match '\bchecked=(\d+)') {
            $dropped = [int]$matches[1]
            if ($dropped -ne ($baseChecked - 1)) {
                Add-Fail ("the GATE-RESULT count is not measuring its domain: removing '$drop' moved checked " +
                          "from $baseChecked to $dropped, expected $($baseChecked - 1)")
            } else {
                Add-Note "count mutation: removing '$drop' moved checked $baseChecked -> $dropped"
            }
        } else {
            Add-Fail 'the count mutation produced no O1-RESULT line'
        }
    }

    # ------------------------------------------------------------ mutation battery
    #
    # Every mutation must be CAUGHT (an arm goes red) or EQUIVALENT (it provably changes no encoded output
    # anywhere in the field's whole domain, so it is not a defect). A mutation that changes output without
    # turning an arm red is MISSED, and that fails the gate: it is a hole in the oracle.
    $list = & $exe "--corpus=$corpus" '--list-mutations' 2>&1
    $mutations = @($list | Where-Object { $_ -match '\S' })
    if ($mutations.Count -lt 50) {
        Add-Fail "the mutation battery generated only $($mutations.Count) mutations; it is derived from the field table and should generate over a hundred"
    }
    $caught = 0; $equivalent = 0; $missed = New-Object System.Collections.ArrayList
    foreach ($m in $mutations) {
        $mo = & $exe "--corpus=$corpus" "--mutate=$m" '--fast' 2>&1
        $mc = $LASTEXITCODE
        $mr = ($mo | Where-Object { $_ -match '^O1-RESULT ' } | Select-Object -Last 1)
        if ($mc -ne 0 -and $mr -match 'status=FAIL') { $caught++; continue }
        if ($mr -match '\bdivergences=(\d+)') { $div = [int]$matches[1] } else { $div = -1 }
        # EQUIVALENT is a positive claim, not the absence of a failure: the harness must show that the
        # mutation left the field's step count and width alone, which is what makes the encoded bits
        # provably identical. Silence on its own is indistinguishable from a blind spot.
        $frame = ($mo | Where-Object { $_ -match '^O1-COVER mutation_steps=' } | Select-Object -Last 1)
        $unchanged = $false
        if ($frame -match 'mutation_steps=(-?\d+)->(-?\d+) mutation_width=(-?\d+)->(-?\d+)') {
            $unchanged = ($matches[1] -eq $matches[2]) -and ($matches[3] -eq $matches[4]) -and ($matches[1] -ne '-1')
        }
        if ($mc -eq 0 -and $div -eq 0 -and $unchanged) { $equivalent++; continue }
        [void]$missed.Add("$m (exit=$mc divergences=$div frame='$frame')")
    }
    Add-Note ("mutations: total={0} caught={1} equivalent={2} missed={3}" -f $mutations.Count, $caught, $equivalent, $missed.Count)
    foreach ($m in $missed) { Add-Note ("  MISSED " + $m) }
    if ($missed.Count -gt 0) {
        Add-Fail ("{0} generated mutation(s) changed the encoding without turning an arm red: {1}" -f `
            $missed.Count, (($missed | Select-Object -First 6) -join '; '))
    }
    if ($caught -eq 0 -and $mutations.Count -gt 0) {
        Add-Fail 'no generated mutation was caught; a battery that never goes red is not evidence'
    }
    $checkedTotal += $caught + $equivalent
}

# ---------------------------------------------------------------- optional self-test

if ($SelfTest) {
    Add-Note 'self-test: -SelfTest only re-reports; the count mutation above runs on every gate pass'
}

$body = New-Object System.Collections.ArrayList
[void]$body.Add("timestamp: " + (Get-Date -Format 'o'))
foreach ($s in $summary) { [void]$body.Add([string]$s) }
[void]$body.Add($(if ($failed.Count -eq 0) { 'VERDICT PASS' } else { 'VERDICT FAIL' }))
foreach ($f in $failed) { [void]$body.Add([string]$f) }
Set-Content -Path $artifact -Value $body -Encoding utf8

foreach ($s in $summary) { Write-Host ("  " + $s) }
foreach ($f in $failed) { Write-Host ("FAIL " + $f) }

if ($checkedTotal -le 0) { Add-Fail 'nothing reported a non-zero item count; refusing to call that a pass' }

if ($failed.Count -gt 0) {
    Write-Host ("RESULT check.ps1 status=FAIL subchecks=3 failures={0}" -f $failed.Count)
    Write-Host ("GATE-RESULT checked={0} subject=bitio-constants+wire-fields+corpus-payloads+refusals+mutations status=FAIL" -f $checkedTotal)
    exit 1
}
Write-Host 'RESULT check.ps1 status=PASS subchecks=3 failures=0'
Write-Host ("GATE-RESULT checked={0} subject=bitio-constants+wire-fields+corpus-payloads+refusals+mutations status=PASS" -f $checkedTotal)
exit 0
