# W1-D's gate check. Two instruments, one verdict:
#
#   tick_selftest   the shipped slot-selection table, the stage-index filter and the frame-continuity
#                   arithmetic the acceptance criterion is stated in (src/engine/tick_policy.cpp)
#   pecheck.py      the fallback's byte guard against the real echovr.exe, and our export surface
#                   against the eleven names the engine resolves against a provider handle
#
# The reported count is the sum of what the two actually asserted, not a constant: deleting a case
# lowers it. docs/spikes/W1-D.md S9 records the mutation run that proves that.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

$failed = $false
$checked = 0

# ---------------------------------------------------------------------------------------------------
# 1. The native selftest. doctest reports its own assertion count; we take that number rather than a
#    number this script made up, so a deleted TEST_CASE shows up in the gate total.
# ---------------------------------------------------------------------------------------------------
$exe = Join-Path (Join-Path $build 'bin') 'tick_selftest.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "no $exe - build first"
    exit 1
}

$output = & $exe --reporters=console 2>&1
$output | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) {
    Write-Host 'tick_selftest reported failures'
    $failed = $true
}

# doctest's summary line: "assertions: 42 | 42 passed | 0 failed |"
$assertLine = $output | Where-Object { $_ -match 'assertions:\s*(\d+)' } | Select-Object -First 1
if (-not $assertLine -or $assertLine -notmatch 'assertions:\s*(\d+)') {
    Write-Host 'could not read an assertion count out of tick_selftest; a check whose count cannot be read is not a check'
    exit 1
}
$nativeChecked = [int]$matches[1]
if ($nativeChecked -eq 0) {
    Write-Host 'tick_selftest asserted nothing'
    exit 1
}
$checked += $nativeChecked
Write-Host "  tick_selftest asserted $nativeChecked"

# ---------------------------------------------------------------------------------------------------
# 2. The binary-level checks.
# ---------------------------------------------------------------------------------------------------
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { Write-Host 'python is not on PATH; pecheck.py cannot run'; exit 1 }

$peOutput = & $py.Source (Join-Path $PSScriptRoot 'pecheck.py') 2>&1
$peExit = $LASTEXITCODE
$peOutput | ForEach-Object { Write-Host "  $_" }
if ($peExit -ne 0) { $failed = $true }

$peLine = $peOutput | Where-Object { $_ -match '^PECHECK-CHECKED\s+(\d+)' } | Select-Object -First 1
if (-not $peLine -or $peLine -notmatch '^PECHECK-CHECKED\s+(\d+)') {
    Write-Host 'pecheck.py did not report what it checked'
    exit 1
}
$peChecked = [int]$matches[1]
if ($peChecked -eq 0) { Write-Host 'pecheck.py asserted nothing'; exit 1 }
$checked += $peChecked
Write-Host "  pecheck asserted $peChecked"

# ---------------------------------------------------------------------------------------------------
# 3. The attach probe must FILE A VERDICT even when coexist refuses it.
#
#    Sweep 4 spent two launches and collected nothing from W1-D: several probe DLLs share one plugins
#    folder, all embed the same bootstrap, and coexist::ClaimInstance refuses every copy after the
#    first so that two of them cannot both claim a CNSProvider slot. Attach() then returns false and a
#    verdict written from a module Init is never written. This reproduces that exact condition -- load
#    a copy first so it wins the claim, then load the real probe as the refused one -- and asserts the
#    verdict is filed anyway, because it is written from a static initialiser.
# ---------------------------------------------------------------------------------------------------
$host_ = Join-Path (Join-Path $build 'bin') 'tick_probehost.exe'
$probe = Join-Path (Join-Path $build 'bin') 'probe_tickattach.dll'
foreach ($needed in @($host_, $probe)) {
    if (-not (Test-Path -LiteralPath $needed)) { Write-Host "no $needed - build first"; exit 1 }
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("w1d_probe_" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
try {
    # The copy loads first and wins the instance claim; the real probe is then the refused one.
    $claimer = Join-Path $tmp 'aa_first_claimer.dll'
    Copy-Item -LiteralPath $probe -Destination $claimer -Force

    $hostOut = & $host_ $tmp $claimer $probe 2>&1
    $hostOut | ForEach-Object { Write-Host "  $_" }

    $verdictPath = Join-Path (Join-Path $tmp 'probe_tickattach') 'done.json'
    $filed = Test-Path -LiteralPath $verdictPath
    Write-Host ("  verdict filed by the REFUSED copy: {0}" -f $filed)
    $checked += 1
    if (-not $filed) {
        Write-Host '  FAIL the refused copy filed no done.json - this is exactly the sweep-4 defect'
        $failed = $true
    } else {
        $verdict = Get-Content -Raw -LiteralPath $verdictPath | ConvertFrom-Json
        Write-Host ("  probe={0} verdict={1} checked={2}" -f $verdict.probe, $verdict.verdict, $verdict.checked)

        $checked += 1
        if ([int]$verdict.checked -le 0) {
            Write-Host '  FAIL the filed verdict asserted nothing'; $failed = $true
        }
        $checked += 1
        if ($verdict.probe -ne 'W1-D-attach-offline') {
            Write-Host "  FAIL probe id came back '$($verdict.probe)', not the one REPLAY_PROBE_IDS named"
            $failed = $true
        }
        # The engine-facing half must PASS even in a host process: it needs only our own mapped image.
        # The game-image checks cannot pass here, and they must be the ONLY failures. Read just the
        # failure section - the prose after it names GetProcAddress too, and matching the whole string
        # made this check fire on its own explanation.
        $failures = ($verdict.detail -split ' \| ')[0]
        $checked += 1
        if ($failures -match 'GetProcAddress') {
            Write-Host '  FAIL a GetProcAddress assertion failed, and those need only our own mapped image'
            Write-Host "       $failures"
            $failed = $true
        }
        # Exactly the four game-image assertions, no more: a host process has no echovr.exe mapped, so
        # anything else failing here is a real defect rather than the expected absence.
        $checked += 1
        $failedCount = ([regex]::Matches($failures, 'FAILED:')).Count
        if ($failedCount -ne 4) {
            Write-Host "  FAIL expected exactly the 4 game-image assertions to fail here, saw $failedCount"
            Write-Host "       $failures"
            $failed = $true
        }
        $checked += 1
        if ($failures -notmatch 'mapped game image') {
            Write-Host '  FAIL the failures are not the expected game-image ones'
            $failed = $true
        }
        $checked += 1
        if ($verdict.detail -notmatch 'static initialiser') {
            Write-Host '  FAIL the verdict does not record where it was written from'
            $failed = $true
        }
        $checked += 1
        if ($verdict.detail -notmatch 'DOES NOT ESTABLISH') {
            Write-Host '  FAIL the verdict does not state what it fails to establish'
            $failed = $true
        }
    }
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "GATE-RESULT checked=$checked subject=tick-slot-choice+stage-filter+frame-continuity+prologue-guard+export-surface+attach-verdict-under-refusal"
if ($failed) { exit 1 }
exit 0
