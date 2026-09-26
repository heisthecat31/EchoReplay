# W1-E's gate check.
#
# Two instruments. entity_selftest is the pure logic E-4 owns -- the fp16 codec every PP-B pose write
# goes through, checked against a transcription of the ENGINE's own widening sequence rather than
# against itself, plus the two constants entity.cpp derives instead of carrying as facts, plus the two
# pools' history pairs. entity_wiretest is the wiring: the real module translation unit through the
# real modreg into a services struct, so a registration that stops publishing IEntity / IPosePublisher
# / IAsSeen is red here rather than silent in a launch (DIAG-WIRING gap 1).
#
# The reported count is the sum of doctest's own assertion totals, not a number this script made up,
# so deleting a CHECK lowers it. docs/spikes/W1-E.md records the mutation run that proves the count can
# move.
#
# Everything else W1-E owns is memory access against engine pools and is not offline-testable at all;
# it is queued as tests/entity/W1-E-match.probe.json and waits for `flat`.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

$failed = $false
$checked = 0

foreach ($name in @('entity_selftest', 'entity_wiretest', 'entity_subjecttest', 'entity_readerstest', 'entity_jsontest', 'entity_statstest', 'entity_spanprooftest')) {
    $exe = Join-Path (Join-Path $build 'bin') "$name.exe"
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Host "no $exe - build first"
        Write-Host 'GATE-RESULT checked=0 subject=entity'
        exit 1
    }

    $out = & $exe --no-colors=true 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }
    if ($code -ne 0) { $failed = $true }

    # doctest prints e.g. "assertions: 24601 | 24601 passed | 0 failed |"
    $m = $out | Select-String -Pattern 'assertions:\s+(\d+)\s*\|' | Select-Object -First 1
    if (-not $m) {
        Write-Host "could not read the assertion count out of $name's doctest output - refusing to invent one"
        $failed = $true
        continue
    }
    $count = [int]$m.Matches[0].Groups[1].Value

    # A count of zero is not a pass: it is a suite that asserted nothing, which is the same floor the
    # harness applies to a probe's done.json.
    if ($count -eq 0) {
        Write-Host "$name asserted nothing"
        $failed = $true
    }
    $checked += $count
}

Write-Host "GATE-RESULT checked=$checked subject=entity"
if ($failed) { exit 1 } else { exit 0 }
