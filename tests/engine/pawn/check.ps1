# W2-P1's pawn-provision gate check.  One instrument: the SHIPPED provisioner
# (src/engine/pawn_provision.cpp, linked in through replay_identity_access, not a copy of its
# reasoning) driven against a fake IIdentity and a fake netgame -- the join record's bytes at every
# offset the engine reads, the state-9 latch, the slice, the ledger and the whole refusal
# vocabulary.  The count is doctest's own assertion total, so deleting a CHECK lowers it.
#
# The ADMISSION CALL is not exercised here and cannot be: ARCH-23 S3.4's prototype promotion has
# not happened, so the product refuses AdmitNotPromoted by name and the last case asserts exactly
# that.  A live check is owed once the prototype lands; a green line here is not one.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

# Count floor.  tools\gate.ps1: a script check's GATE-RESULT count is self-reported and
# unverifiable, so this packet mutation-tests its own -- dropping the postcondition resolve in
# PawnProvisioner::Provision takes the L3-shape case red.  Raise it when checks are added; never
# lower it to make a run green.
$selftestFloor = 70

$failed = $false
$exe = Join-Path (Join-Path $build 'bin') 'pawn_selftest.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "no $exe - build first"
    Write-Host 'GATE-RESULT checked=0 subject=pawn'
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
    Write-Host "pawn_selftest reported $assertions assertions, floor is $selftestFloor"
    $failed = $true
}

Write-Host "GATE-RESULT checked=$assertions subject=pawn"
if ($failed) { exit 1 }
exit 0
