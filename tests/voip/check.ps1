# W3-C's gate check.
#
# Five arms, and four of them carry a negative:
#
#   1. voip_selftest      the receiver resolution walk against a synthetic component system, the
#                         five gates ReceiveVoip checks, the capture tap's dispositions and their
#                         total, the injection arguments, and the container round trip from written
#                         records back into a stand-in for VOIP.RECEIVE. The reported count is
#                         doctest's own assertion total, so deleting a CHECK lowers it.
#   2. voip_factcheck     against the shipped echovr.exe: every value src/audio/voip/voip_facts.h
#                         compiles against, re-decoded from the operand bytes at its site. The
#                         expectation is the header and the evidence is the binary -- two artifacts,
#                         neither generating the other.
#   3b. voip_selftest_mutant  the discontinuity guard neutered; this one MUST fail, and must fail in
#                         the backwards-seek arm specifically.
#   4. voip_factcheck --mutate  perturbs one expected value and REQUIRES the comparison to reject
#                         it. Without this arm the comparisons above have never been observed to
#                         fail and are not evidence of anything.
#
# ★ WHAT THIS CHECK CANNOT SEE, stated so its blind spot is bounded. Nothing here runs the game, so
# nothing here establishes that a replay session HAS a connected receiver instance, that the voice
# provider global is non-null outside a live match, or that an injected frame is audible. Those are
# runs/queue/payload/W3-C.json's reads. A green result from this file means the offline half is
# correct, and says nothing about the in-vivo half.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) {
    Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'
    Write-Host 'GATE-RESULT checked=0 subject=voip'
    exit 1
}

$bin = Join-Path $build 'bin'
$fail = 0
$notRun = 0
# What this run actually asserted, taken from the arms' own output -- doctest's assertion total and
# factcheck's own comparison count -- plus one for each control that was observed catching its
# mutant. A typed number would be a number the gate cannot disbelieve; deleting a CHECK lowers this
# one, which is the demonstration docs\INSTALL.md asks for.
$checked = 0

# ---------------------------------------------------------------------------------------------------
# ★ THE STALENESS GUARD. A test result is only evidence about a source file if the build actually
# recompiled it, and "the code passes" reads identically to "the code did not build and yesterday's
# exe passes" in every log. W2-H was handed exactly that green when another packet's CMakeLists broke
# the generate step and ninja rebuilt nothing. Same shape as W2-B's and W2-H's, deliberately, so
# there is one convention rather than three.
# ---------------------------------------------------------------------------------------------------
# ★★ BY CONTENT, NOT BY MTIME. The mtime form went red on a NO-OP: eleven agents share this tree, and
# any git checkout/stash/reset rewrites a source byte-for-byte with a fresh timestamp, so the gate went
# red for everyone over a file nobody had changed. A guard that reds on a no-op is one people learn to
# mute, and a muted guard is worse than none. tools\build.ps1 records the SHA-256 of every tracked
# source in source_hashes.txt AFTER a successful build and from the PRE-build content, so it is a
# statement about what went into this binary; compare against that. Same content, new mtime -> green.
# One byte changed with no rebuild -> red.
$hashRecordFile = Join-Path $build 'source_hashes.txt'
$recordedHashes = $null
if (Test-Path -LiteralPath $hashRecordFile) {
    $recordedHashes = @{}
    foreach ($line in (Get-Content -LiteralPath $hashRecordFile)) {
        $parts = $line -split ' ', 2
        if ($parts.Count -eq 2) { $recordedHashes[$parts[1]] = $parts[0] }
    }
}
$repoPrefix = ($repo -replace '\\', '/').TrimEnd('/') + '/'

# '' when the file's content is what the build recorded, otherwise the reason it cannot be certified.
# A missing record is NOT "nothing changed": a build directory that never said what it compiled cannot
# certify anything, and a file the record does not name is one that build could not attribute either.
function Get-StaleReason([string]$full) {
    if ($null -eq $script:recordedHashes) {
        return ('no content record in ' + $script:hashRecordFile)
    }
    $rel = ($full -replace '\\', '/')
    if ($rel.StartsWith($script:repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        $rel = $rel.Substring($script:repoPrefix.Length)
    }
    if (-not $script:recordedHashes.ContainsKey($rel)) {
        return ("$rel is compiled in, but the build that produced this binary recorded no hash for it, " +
                'so this run cannot tell a current binary from a stale one')
    }
    $now = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
    if ($now -ne $script:recordedHashes[$rel]) {
        return ("$rel has changed since the binary was built (on disk $now, built from " +
                $script:recordedHashes[$rel] + ')')
    }
    return ''
}

if ($null -eq $recordedHashes) {
    Write-Host ("FAIL: the staleness guard has no content record to compare against; expected " +
                $hashRecordFile + " (written by tools\build.ps1 after a successful build)")
    $fail++
}

$srcRoots = @(
    (Join-Path $repo 'src\audio\voip'),
    (Join-Path $repo 'tests\voip')
)
$srcFiles = @()
foreach ($root in $srcRoots) {
    if (Test-Path $root) {
        $srcFiles += Get-ChildItem -Path $root -Recurse -File -Include *.cpp, *.h
    }
}

# The guard's SUBJECT must be the sources the target actually compiles. It was every .cpp and .h
# under src/audio/voip and tests/voip, so editing one arm's source made the OTHER arms report as
# stale -- a guard wider than its hazard, which cries wolf until somebody stops reading it.
# factcheck_voip.cpp builds voip_factcheck.exe alone; the two doctest exes never see it.
$exclusiveSources = @{
    'voip_selftest.exe'        = @('factcheck_voip.cpp')
    'voip_selftest_mutant.exe' = @('factcheck_voip.cpp')
    'voip_factcheck.exe'       = @('test_voip.cpp', 'test_voip_container.cpp', 'test_voip_bind.cpp',
                                   'fakeengine.h')
}

function Assert-Fresh([string]$exePath, [string]$label) {
    if (-not (Test-Path $exePath)) { return $true }
    if ($script:srcFiles.Count -eq 0) {
        Write-Host 'FAIL: the staleness guard found no sources to compare against, so it would pass whatever it was given'
        return $false
    }
    $skip = $script:exclusiveSources[$label]
    $mine = @($script:srcFiles | Where-Object { $skip -notcontains $_.Name })
    if ($mine.Count -eq 0) {
        Write-Host "FAIL: the staleness guard has no sources left for $label after exclusions"
        return $false
    }
    $changed = @()
    foreach ($f in $mine) {
        $why = Get-StaleReason $f.FullName
        if ($why) { $changed += $why }
    }
    if ($changed.Count -gt 0) {
        Write-Host ("FAIL: $label was not built from " + $changed.Count + " of its " + $mine.Count +
                    " sources, so this run would report on code that was never compiled; first: " +
                    $changed[0])
        return $false
    }
    Write-Host ("fresh: $label was built from the recorded content of all " + $mine.Count + " of its sources")
    return $true
}

# ---- 1. the offline suite -------------------------------------------------------------------------
$selftest = Join-Path $bin 'voip_selftest.exe'
if (-not (Test-Path $selftest)) {
    Write-Host "NOT RUN: $selftest was not built"
    $notRun++
} elseif (-not (Assert-Fresh $selftest 'voip_selftest.exe')) {
    $fail++
} else {
    $out = & $selftest 2>&1
    $out | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL: voip_selftest'; $fail++ }

    # A suite that ran but asserted almost nothing is a defect in the suite, not a pass. The floor is
    # well under the real count so it catches a suite that stopped running, not one that changed size.
    $line = ($out | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
    if (-not $line) {
        Write-Host 'FAIL: voip_selftest produced no doctest assertion summary; the count cannot be trusted'
        $fail++
    } elseif ($line -match 'assertions:\s+(\d+)\s+\|\s+(\d+)\s+passed\s+\|\s+(\d+)\s+failed') {
        $total = [int]$Matches[1]
        if ($total -lt 90) {
            Write-Host "FAIL: voip_selftest asserted only $total times; the suite carries well over 100, so this means it did not run"
            $fail++
        }
        $checked += $total
    } else {
        Write-Host ('FAIL: could not parse the doctest summary line: ' + $line)
        $fail++
    }
}

# ---- 1b. the mutant, which MUST fail ---------------------------------------------------------------
# ★ The discontinuity guard shipped unreachable once, and a test asserting "no discontinuity was
# reported" would have passed against that dead code. So the suite's backwards-seek arm is proved by
# building the defect back in and requiring red. A green mutant means the arm is decorative.
$mutant = Join-Path $bin 'voip_selftest_mutant.exe'
if (-not (Test-Path $mutant)) {
    Write-Host "NOT RUN: $mutant was not built, so the discontinuity arm is unproven"
    $notRun++
} elseif (-not (Assert-Fresh $mutant 'voip_selftest_mutant.exe')) {
    $fail++
} else {
    $mout = & $mutant 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host 'FAIL: the mutant passed. With the discontinuity guard neutered the suite still went'
        Write-Host '      green, so the backwards-seek arm does not actually test the guard.'
        $fail++
    } else {
        $seek = ($mout | Where-Object { $_ -match 'backwards seek' } | Select-Object -First 1)
        if ($seek) {
            Write-Host 'mutant caught: neutering the discontinuity guard turns the backwards-seek arm red'
            $checked++
        } else {
            Write-Host 'FAIL: the mutant failed, but not in the backwards-seek arm -- it may be failing for'
            Write-Host '      an unrelated reason, which would make this control prove nothing.'
            $fail++
        }
    }
}

# ---- 2 and 3. the binary arm and the mutation that proves it looks --------------------------------
$factcheck = Join-Path $bin 'voip_factcheck.exe'
. (Join-Path $PSScriptRoot '..\..\tools\echovr_path.ps1')
$exe = Find-EchoVrExe
if (-not $exe) { $exe = '(echovr.exe not found - set ECHOVR_DIR or REPLAY_ECHOVR_EXE)' }

if (-not (Test-Path $factcheck)) {
    Write-Host "NOT RUN: $factcheck was not built"
    $notRun++
} elseif (-not (Assert-Fresh $factcheck 'voip_factcheck.exe')) {
    # It compiles voip_facts.h in, so a stale one checks yesterday's constants.
    $fail++
} elseif (-not (Test-Path $exe)) {
    Write-Host "NOT RUN: echovr.exe is not at $exe, so no W3-C value was checked against the binary"
    $notRun++
} else {
    $fout = & $factcheck $exe 2>&1
    $fcode = $LASTEXITCODE
    $fout | ForEach-Object { Write-Host $_ }
    if ($fcode -ne 0) {
        Write-Host 'FAIL: a value voip_facts.h compiles against does not agree with echovr.exe'
        $fail++
    }
    # Its own count of what it compared, not ours: a header row that stops being emitted stops
    # being counted.
    $vline = ($fout | Where-Object { $_ -match '^(\d+) of (\d+) W3-C values' } | Select-Object -Last 1)
    if ($vline -match '^(\d+) of (\d+) W3-C values') {
        $checked += [int]$Matches[1]
    } else {
        Write-Host 'FAIL: voip_factcheck reported no comparison count, so this arm asserted nothing this run'
        $fail++
    }

    $mfout = & $factcheck $exe --mutate 2>&1
    $mfcode = $LASTEXITCODE
    $mfout | ForEach-Object { Write-Host $_ }
    if ($mfcode -ne 0) {
        Write-Host 'FAIL: the mutation arm did not catch a perturbed expectation'
        $fail++
    } else {
        $checked++
    }
}

# ---- the gate's own verdict -----------------------------------------------------------------------
# ★ The line tools\gate.ps1 reads. Without it this check exited 0 having asserted nothing as far as
# the gate could tell, and was reported NOT-RUN -- which is the correct reading of silence.
Write-Host "GATE-RESULT checked=$checked subject=voip"
if ($fail -gt 0) {
    Write-Host "voip: FAIL ($fail arm(s) failed, $notRun not run)"
    exit 1
}
if ($notRun -gt 0) {
    Write-Host "voip: PASS with $notRun arm(s) NOT RUN -- a check that could not run is not a check that passed"
} else {
    Write-Host 'voip: PASS (4 of 4 arms ran)'
}
exit 0
