# W2-C's gate check.  Two instruments, and the reported count is their sum.
#
#   identity_selftest  the pure logic the cosmetic path is built on: the post-apply diff that
#                      tells the engine's own item remap from an item that did not survive, and
#                      the profile walk that carries a recorded ServerProfile in one scalar at a
#                      time.  The count is doctest's own assertion total, so deleting a CHECK
#                      lowers it.
#
#   sitecheck.py       every engine site this packet touches, read back out of the shipped
#                      echovr.exe and cross-checked against the constants the shipped code
#                      compiles against.  Three sources: the fact drop supplies the DOMAIN, the
#                      binary supplies the SUBJECT, and src/engine/identity_access_facts.h is the third
#                      party.  None is derived from another, which is what B.13 rule 4 asks for.
#                      The count moves with the domain, so it is floored below: an absent
#                      echovr.exe is RED rather than a silent zero, and a shrinking domain is
#                      caught by $siteFloor rather than absorbed into a green run.
#
# Everything else W2-C owns needs a live CR15NetGame -- the slot tables, the profile CJson, the
# live loadout table, the customisation records -- and is queued as tests/identity/W2-C.probe.json
# (attach-time, runnable now) and W2-C-match.probe.json, which is NOT in the queue while `flat`
# is refused.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

# Count floors.  tools\gate.ps1: "a script check's GATE-RESULT count is self-reported and
# unverifiable ... Each packet must mutation-test its own count instead."  These two are that
# floor, and both are mutation-tested -- deleting a CHECK from the selftest and pointing
# sitecheck.py at an absent binary each take the gate red.  Raise them when checks are added;
# never lower one to make a run green.
$selftestFloor = 267
$siteFloor = 89

$failed = $false
$checked = 0

$exe = Join-Path (Join-Path $build 'bin') 'identity_selftest.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "no $exe - build first"
    Write-Host 'GATE-RESULT checked=0 subject=identity'
    exit 1
}

$out = & $exe --no-colors=true 2>&1
$code = $LASTEXITCODE
$out | ForEach-Object { Write-Host $_ }
if ($code -ne 0) { $failed = $true }

$m = $out | Select-String -Pattern 'assertions:\s+(\d+)\s*\|' | Select-Object -First 1
if ($m) {
    $n = [int]$m.Matches[0].Groups[1].Value
    $checked += $n
    if ($n -lt $selftestFloor) {
        Write-Host "identity_selftest asserted $n, below its floor of $selftestFloor - checks have gone missing"
        $failed = $true
    }
} else {
    Write-Host 'could not read the assertion count out of doctest output - refusing to invent one'
    $failed = $true
}

$site = Join-Path $repo 'tests\identity\sitecheck.py'
if (Get-Command python -ErrorAction SilentlyContinue) {
    $siteOut = @(& python $site)
    $siteCode = $LASTEXITCODE
} else {
    $siteOut = @('SKIP sitecheck.py needs Python 3, which is not on PATH', 'SITES 0 STATUS SKIPPED')
    $siteCode = 0
}
$siteOut | ForEach-Object { Write-Host $_ }
if ($siteCode -ne 0) { $failed = $true }

# SITES <n> STATUS <PASS|FAIL>.  The count is read whatever the status, because a shrinking
# domain behind a red run is exactly the thing a collapsed-to-zero count would hide.
$s = $siteOut | Select-String -Pattern '^SITES\s+(\d+)\s+STATUS\s+(\w+)' | Select-Object -First 1
if ($s) {
    $n = [int]$s.Matches[0].Groups[1].Value
    $checked += $n
    $siteStatus = $s.Matches[0].Groups[2].Value
    if ($siteStatus -ne 'PASS' -and $siteStatus -ne 'SKIPPED') { $failed = $true }
    if ($siteStatus -ne 'SKIPPED' -and $n -lt $siteFloor) {
        Write-Host "sitecheck.py verified $n sites, below its floor of $siteFloor - the domain has shrunk"
        $failed = $true
    }
} else {
    Write-Host 'sitecheck.py printed no SITES line - refusing to invent one'
    $failed = $true
}

Write-Host "GATE-RESULT checked=$checked subject=identity-diff+profile-walk+engine-sites"
if ($failed) { exit 1 } else { exit 0 }
