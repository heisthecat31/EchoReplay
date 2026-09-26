# W3-E's gate check.
#
# Two instruments, and the second is the one that keeps the first honest:
#
#   1. events_selftest   the decoders and the marker derivation, against thirteen message bodies
#                        committed beside it. The reported count is doctest's own assertion total, so
#                        deleting a CHECK lowers it.
#   2. events_corpus     the SAME decoders over every message in the reference capture. The fixtures
#                        are a sample; a framing that is right on a sample and wrong on the population
#                        fails here and nowhere else. It also carries the one NEGATIVE claim this
#                        packet makes, with its denominator printed and its probe proved.
#
# The corpus lives outside the repository. Its absence is reported as NOT RUN naming the path, and the
# arm contributes nothing to the count -- never a pass. The selftest alone is a complete T0 gate.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

$bin = Join-Path $build 'bin'
$failed = $false
$checked = 0

# ---- 1. the offline decoders --------------------------------------------------------------------
$exe = Join-Path $bin 'events_selftest.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "no $exe - build first"
    Write-Host 'GATE-RESULT checked=0 subject=events'
    exit 1
}
$out = & $exe --no-colors=true 2>&1
$code = $LASTEXITCODE
$out | ForEach-Object { Write-Host $_ }
if ($code -ne 0) { $failed = $true }

$m = $out | Select-String -Pattern 'assertions:\s+(\d+)\s*\|' | Select-Object -First 1
if ($m) {
    $checked = [int]$m.Matches[0].Groups[1].Value
} else {
    Write-Host 'could not read the assertion count out of doctest output - refusing to invent one'
    $failed = $true
}
if ($checked -eq 0) {
    Write-Host 'events_selftest asserted nothing'
    $failed = $true
}

# ---- 2. the whole population ---------------------------------------------------------------------
$corpusExe = Join-Path $bin 'events_corpus.exe'
if (-not (Test-Path -LiteralPath $corpusExe)) {
    Write-Host "no $corpusExe - build first"
    $failed = $true
} else {
    Write-Host ''
    $cout = & $corpusExe 2>&1
    $ccode = $LASTEXITCODE
    $cout | ForEach-Object { Write-Host $_ }
    if ($ccode -eq 2) {
        # The reader printed the path it looked for. NOT RUN, and it adds nothing to the count.
        Write-Host 'events_corpus: NOT RUN (corpus absent) - this arm contributes 0 to the count'
    } elseif ($ccode -ne 0) {
        $failed = $true
    } else {
        # Count only the arms that reported a per-instance total, so a run that decoded nothing cannot
        # raise the number. The 'ok' lines carry n=<count>; the sum is what this arm actually examined.
        $sum = 0
        foreach ($line in $cout) {
            $mm = [regex]::Match([string]$line, '^ok\s+.*\sn=(\d+)\s*$')
            if ($mm.Success) { $sum += [int]$mm.Groups[1].Value }
        }
        if ($sum -eq 0) {
            Write-Host 'events_corpus exited 0 but examined nothing - that is not a pass'
            $failed = $true
        }
        $checked += $sum
    }
}

Write-Host "GATE-RESULT checked=$checked subject=events"
if ($failed) { exit 1 } else { exit 0 }
