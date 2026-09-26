# W1-B's gate check: the container self-test, plus the echodemo CLI run against a fixture it generates.
#
# The reported count is the sum of what the four instruments actually asserted -- doctest's own assertion
# total, the structural and completeness checks `verify` ran, the recovery cases `fuzz` ran, and the
# stream blocks `dump` walked. None of those numbers is typed here.
$ErrorActionPreference = 'Stop'

$build = $env:REPLAY_BUILD_DIR
if (-not $build) { Write-Host 'REPLAY_BUILD_DIR is not set'; exit 1 }
$bin = Join-Path $build 'bin'

$selftest = Join-Path $bin 'container_selftest.exe'
$mkdemo = Join-Path $bin 'container_mkdemo.exe'
$cli = Join-Path $bin 'echodemo.exe'
$realdata = Join-Path $bin 'container_realdata.exe'
foreach ($exe in @($selftest, $mkdemo, $cli, $realdata)) {
    if (-not (Test-Path -LiteralPath $exe)) { Write-Host "missing: $exe"; exit 1 }
}

$fixture = Join-Path $build 'container_fixture.echodemo'
$gapFixture = Join-Path $build 'container_gap.echodemo'
$stripped = Join-Path $build 'container_fixture.novoice.echodemo'
$failed = $false

function Run-Tool([string]$exe, [string[]]$toolArgs) {
    $out = & $exe @toolArgs 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }
    return [pscustomobject]@{ Output = $out; Code = $code }
}

function Sum-Match($lines, [string]$pattern) {
    $total = 0
    $found = $false
    foreach ($line in $lines) {
        if ("$line" -match $pattern) { $total += [int]$matches[1]; $found = $true }
    }
    if (-not $found) { return -1 }
    return $total
}

Write-Host '--- container self-test (doctest) ---'
$r = Run-Tool $selftest @('--no-intro=true')
if ($r.Code -ne 0) { $failed = $true }
$assertions = Sum-Match $r.Output 'assertions:\s+(\d+)'
if ($assertions -le 0) { Write-Host 'the self-test reported no assertion total'; $failed = $true; $assertions = 0 }

Write-Host '--- fixture ---'
$r = Run-Tool $mkdemo @($fixture, '--chapters', '24', '--tapreg', 'present')
if ($r.Code -ne 0) { Write-Host 'fixture generation failed'; exit 1 }

Write-Host '--- echodemo info ---'
$r = Run-Tool $cli @('info', $fixture)
if ($r.Code -ne 0) { $failed = $true }

Write-Host '--- echodemo verify ---'
$r = Run-Tool $cli @('verify', $fixture)
if ($r.Code -ne 0) { $failed = $true }
$verifyChecks = Sum-Match $r.Output 'VERIFY-CHECKS\s+(\d+)'
$verifyFails = Sum-Match $r.Output 'VERIFY-FAILS\s+(\d+)'
if ($verifyChecks -le 0) { Write-Host 'verify reported no check count'; $failed = $true; $verifyChecks = 0 }
if ($verifyFails -ne 0) { Write-Host 'verify reported failures on a clean fixture'; $failed = $true }
# The row itself, not just the exit code: the denominator is what L2's verdict divides by, so a green
# verify that printed no row would be a pass with nothing behind it.
if (-not ($r.Output -match '^TAPS registered=\d+ deferred=\d+ givenUp=\d+ unavailable=\d+ excluded=\d+ total=\d+ table=\d+')) {
    Write-Host 'verify printed no TAPS row for a file that carries TAP_REGISTRATION'
    $failed = $true
}

Write-Host '--- echodemo dump ---'
$r = Run-Tool $cli @('dump', $fixture)
if ($r.Code -ne 0) { $failed = $true }
$dumpBlocks = Sum-Match $r.Output 'DUMP-BLOCKS\s+(\d+)'
if ($dumpBlocks -le 0) { Write-Host 'dump walked no stream blocks'; $failed = $true; $dumpBlocks = 0 }

Write-Host '--- echodemo strip --voip ---'
$r = Run-Tool $cli @('strip', $fixture, $stripped, '--voip')
if ($r.Code -ne 0) { Write-Host 'strip did not verify its own output'; $failed = $true }

# The negative arm. A capture whose sampler dropped records must make verify go red, on every run, so
# the green above is a verdict and not the only outcome this check can produce (design 12.0).
Write-Host '--- echodemo verify on a capture that lost samples (must go red) ---'
$r = Run-Tool $mkdemo @($gapFixture, '--chapters', '6', '--divisor', '2', '--tapreg', 'present')
if ($r.Code -ne 0) { Write-Host 'gap fixture generation failed'; exit 1 }
$r = Run-Tool $cli @('verify', $gapFixture, '--quiet')
$gapChecks = Sum-Match $r.Output 'VERIFY-CHECKS\s+(\d+)'
$gapFails = Sum-Match $r.Output 'VERIFY-FAILS\s+(\d+)'
if ($r.Code -eq 0 -or $gapFails -lt 1) {
    Write-Host 'verify stayed green on a capture that lost samples'
    $failed = $true
}
if ($gapChecks -le 0) { $gapChecks = 0 }

# The real-data arm (acceptance: round-trip on synthetic AND real data). The corpus is the retail
# capture R4 measured from; it lives outside the repository, so its absence is reported as NOT RUN with
# the path, and its bytes are not counted. NOT-RUN is never folded into a pass.
Write-Host '--- real-capture round trip ---'
$corpus = $env:REPLAY_REALCAP
# R4's corpus is the private freshcap capture: REPLAY_REALCAP names the file, or REPLAY_FRESHCAP_DIR
# the folder holding arena.bin.
if (-not $corpus -and $env:REPLAY_FRESHCAP_DIR) { $corpus = Join-Path $env:REPLAY_FRESHCAP_DIR 'arena.bin' }
if (-not $corpus) { $corpus = '(corpus not configured - set REPLAY_FRESHCAP_DIR or REPLAY_REALCAP)' }
$realRecords = 0
$realArm = 'realdata-NOTRUN'
$r = Run-Tool $realdata @($corpus, (Join-Path $build 'container_real.echodemo'))
if ($r.Code -eq 3) {
    Write-Host "  real-data round trip: NOT RUN (corpus absent: $corpus)"
} elseif ($r.Code -ne 0) {
    Write-Host '  real-data round trip: FAILED'
    $failed = $true
} else {
    $realRecords = Sum-Match $r.Output 'REALDATA-RECORDS\s+(\d+)'
    if ($realRecords -le 0) { Write-Host 'the real-data arm compared nothing'; $failed = $true; $realRecords = 0 }
    else { $realArm = 'realdata' }
}

# Three more negative arms, each forcing a different check red. A branch no test reaches is not a
# check; F2 was exactly that -- the rate detector's arm was executed by nothing in the gate.
Write-Host '--- echodemo verify on a mid-capture rate halving (must go red) ---'
$rateFixture = Join-Path $build 'container_rate.echodemo'
$r = Run-Tool $mkdemo @($rateFixture, '--chapters', '8', '--divisor', '2', '--divisor-from', '4', '--divisor-asseen', '--dishonest-observed', '--tapreg', 'present')
if ($r.Code -ne 0) { Write-Host 'rate fixture generation failed'; exit 1 }
$r = Run-Tool $cli @('verify', $rateFixture, '--quiet')
$rateChecks = Sum-Match $r.Output 'VERIFY-CHECKS\s+(\d+)'
$rateFails = Sum-Match $r.Output 'VERIFY-FAILS\s+(\d+)'
if ($r.Code -eq 0 -or $rateFails -lt 1 -or -not ($r.Output -match 'FAIL: S_ASSEEN rate steps')) {
    Write-Host 'verify did not name the S_ASSEEN rate step of a sampler that halved its rate mid-capture'
    $failed = $true
}
if ($rateChecks -le 0) { $rateChecks = 0 }
Write-Host '--- echodemo verify on a schema that licenses sample dropping (must go red) ---'
$edgeFixture = Join-Path $build 'container_edge.echodemo'
$r = Run-Tool $mkdemo @($edgeFixture, '--chapters', '6', '--legacy-edge', '3', '--tapreg', 'present')
if ($r.Code -ne 0) { Write-Host 'edge fixture generation failed'; exit 1 }
$r = Run-Tool $cli @('verify', $edgeFixture, '--quiet')
$edgeChecks = Sum-Match $r.Output 'VERIFY-CHECKS\s+(\d+)'
$edgeFails = Sum-Match $r.Output 'VERIFY-FAILS\s+(\d+)'
if ($r.Code -eq 0 -or $edgeFails -lt 1) {
    Write-Host 'verify stayed green on a file whose schema declares a retention other than ALL'
    $failed = $true
}
if ($edgeChecks -le 0) { $edgeChecks = 0 }
Write-Host '--- echodemo verify on a block that overstates its stored count (must go red) ---'
$storedFixture = Join-Path $build 'container_stored.echodemo'
$r = Run-Tool $mkdemo @($storedFixture, '--chapters', '8', '--lie-stored-from', '4', '--tapreg', 'present')
if ($r.Code -ne 0) { Write-Host 'stored fixture generation failed'; exit 1 }
$r = Run-Tool $cli @('verify', $storedFixture, '--quiet')
$storedChecks = Sum-Match $r.Output 'VERIFY-CHECKS\s+(\d+)'
$storedFails = Sum-Match $r.Output 'VERIFY-FAILS\s+(\d+)'
if ($r.Code -eq 0 -or $storedFails -lt 1 -or -not ($r.Output -match 'FAIL: S_NETVAR block .* stored is not the')) {
    Write-Host 'verify did not name the S_NETVAR block claiming more records than its bytes hold'
    $failed = $true
}
if ($storedChecks -le 0) { $storedChecks = 0 }
# ---- the tap-registration denominator, three arms -------------------------------------
# The row above is only a verdict if the tool can also produce the other two answers. Each fixture below
# differs from the green one in exactly one property, so nothing else can be what turned it red.
Write-Host '--- echodemo verify on a capture with no TAP_REGISTRATION (must name it) ---'
$noTapFixture = Join-Path $build 'container_notapreg.echodemo'
$r = Run-Tool $mkdemo @($noTapFixture, '--chapters', '4')
if ($r.Code -ne 0) { Write-Host 'no-tapreg fixture generation failed'; exit 1 }
$r = Run-Tool $cli @('verify', $noTapFixture, '--quiet')
$noTapChecks = Sum-Match $r.Output 'VERIFY-CHECKS\s+(\d+)'
$noTapFails = Sum-Match $r.Output 'VERIFY-FAILS\s+(\d+)'
if ($r.Code -ne 1 -or $noTapFails -lt 1) {
    Write-Host 'verify stayed green on a file that never says how many taps were live'
    $failed = $true
}
if (-not ($r.Output -match 'missing TAP_REGISTRATION')) {
    Write-Host 'verify went red on the no-tapreg fixture without naming the missing block'
    $failed = $true
}
if ($noTapChecks -le 0) { $noTapChecks = 0 }

Write-Host '--- echodemo verify on a TAP_REGISTRATION that does not close its identities (must refuse) ---'
$badTapFixture = Join-Path $build 'container_badtapreg.echodemo'
$r = Run-Tool $mkdemo @($badTapFixture, '--chapters', '4', '--tapreg', 'corrupt')
if ($r.Code -ne 0) { Write-Host 'corrupt-tapreg fixture generation failed'; exit 1 }
$r = Run-Tool $cli @('verify', $badTapFixture, '--quiet')
$badTapChecks = Sum-Match $r.Output 'VERIFY-CHECKS\s+(\d+)'
if ($r.Code -ne 2 -or -not ($r.Output -match 'REFUSED: TAP_REGISTRATION')) {
    Write-Host 'verify did not REFUSE a file whose denominator does not add up'
    $failed = $true
}
if ($badTapChecks -le 0) { $badTapChecks = 0 }

Write-Host '--- echodemo verify on a table count TAP_TABLE contradicts (must go red) ---'
$skewFixture = Join-Path $build 'container_tapskew.echodemo'
$r = Run-Tool $mkdemo @($skewFixture, '--chapters', '4', '--tapreg', 'table-skew')
if ($r.Code -ne 0) { Write-Host 'table-skew fixture generation failed'; exit 1 }
$r = Run-Tool $cli @('verify', $skewFixture, '--quiet')
$skewChecks = Sum-Match $r.Output 'VERIFY-CHECKS\s+(\d+)'
$skewFails = Sum-Match $r.Output 'VERIFY-FAILS\s+(\d+)'
if ($r.Code -eq 0 -or $skewFails -lt 1) {
    Write-Host 'verify accepted a tap table count the same keyframe contradicts'
    $failed = $true
}
if (-not ($r.Output -match "disagrees with the keyframe's own TAP_TABLE")) {
    Write-Host 'the table-skew arm went red without naming the cross-check'
    $failed = $true
}
if ($skewChecks -le 0) { $skewChecks = 0 }

Write-Host '--- echodemo fuzz ---'
$r = Run-Tool $cli @('fuzz', $fixture, '--seed', '20260827', '--cases', '96')
if ($r.Code -ne 0) { $failed = $true }
$fuzzCases = Sum-Match $r.Output 'FUZZ-CASES\s+(\d+)'
$fuzzFails = Sum-Match $r.Output 'FUZZ-FAILS\s+(\d+)'
if ($fuzzCases -le 0) { Write-Host 'fuzz ran no cases'; $failed = $true; $fuzzCases = 0 }
if ($fuzzFails -ne 0) { Write-Host 'fuzz found a recovery failure'; $failed = $true }

Remove-Item -LiteralPath $fixture, $gapFixture, $rateFixture, $edgeFixture, $storedFixture, $stripped, $noTapFixture, $badTapFixture, $skewFixture -Force -ErrorAction SilentlyContinue

$checked = $assertions + $verifyChecks + $gapChecks + $rateChecks + $edgeChecks + $storedChecks +
           $noTapChecks + $badTapChecks + $skewChecks + $fuzzCases + $dumpBlocks + $realRecords
Write-Host ''
Write-Host "  doctest assertions   : $assertions"
Write-Host "  verify checks        : $verifyChecks"
Write-Host "  verify checks (gap)  : $gapChecks (expected red, saw $gapFails failure(s))"
Write-Host "  verify checks (rate) : $rateChecks (expected red, saw $rateFails failure(s))"
Write-Host "  verify checks (edge) : $edgeChecks (expected red, saw $edgeFails failure(s))"
Write-Host "  verify checks (stored) : $storedChecks (expected red, saw $storedFails failure(s))"
Write-Host "  verify checks (no tapreg) : $noTapChecks (expected red, saw $noTapFails failure(s))"
Write-Host "  verify checks (bad tapreg) : $badTapChecks (expected REFUSED)"
Write-Host "  verify checks (tap table skew) : $skewChecks (expected red, saw $skewFails failure(s))"
Write-Host "  fuzz recovery cases  : $fuzzCases"
Write-Host "  dump stream blocks   : $dumpBlocks"
Write-Host "  real-capture records : $realRecords"
Write-Host "GATE-RESULT checked=$checked subject=echodemo-container+ring+recovery+$realArm"

if ($failed) { exit 1 }
exit 0
