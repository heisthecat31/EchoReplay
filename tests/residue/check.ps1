# gate-tier: T0
#
# W3-A's gate entry point. Discovered by tools\gate.ps1 by convention: tests\<name>\check.ps1.
#
# ★ WHAT THIS GATE COVERS, stated here because a GATE-RESULT number with no scope is a claim:
#   the entity -> ncaPhysics -> CPhTarget -> CPhBody resolve chain and every guard in it, the three
#   per-body effects and their widths, the eight rows' availability and cause routing, and the two
#   order-dependent registration pairs -- all over a FABRICATED component-system image.
#
#   It covers NO engine memory and NO engine call -- with one exception: residue::BindCsSite, the
#   netGame -> NETGAME.CS_SPACE_OFF -> CS.LOOKUP_BY_SYMBOL hop, runs over a synthetic CR15NetGame.
#   residue::ResolveCsSite's and ResolvePhysSite's reads of the CR15Game singleton and the CPhSystem
#   global are still substituted here; whether the five
#   guarded entry points do to a REAL CI and a REAL body what their decompiled bodies say is parked
#   in runs\queue\payload\W3-A.json for a launch. This gate being green is not the battery working.
#
# One instrument, judged three ways: exit code, assertion count, and -- because a suite that never
# reached its subject exits 0 having asserted plenty about nothing -- a named test case that must be
# present in the run.

[CmdletBinding()]
param(
    [switch]$SelfTest
)

# NOT 'Stop': under 'Stop' a native process writing to stderr raises NativeCommandError and aborts
# this script before it can record anything, turning a real failure into a run with no verdict.
$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)
if ($env:REPLAY_REPO_ROOT) { $repo = $env:REPLAY_REPO_ROOT }
$buildDir = Join-Path $repo 'build\W3-A'
if ($env:REPLAY_BUILD_DIR) { $buildDir = $env:REPLAY_BUILD_DIR }

$failed = New-Object System.Collections.ArrayList
function Add-Fail([string]$m) { [void]$failed.Add($m) }

# ★★ BY CONTENT, NOT BY MTIME. The mtime form went red on a NO-OP: eleven agents share this tree, and
# any git checkout/stash/reset rewrites a source byte-for-byte with a fresh timestamp, so the gate went
# red for everyone over a file nobody had changed. A guard that reds on a no-op is one people learn to
# mute, and a muted guard is worse than none. tools\build.ps1 records the SHA-256 of every tracked
# source in source_hashes.txt AFTER a successful build and from the PRE-build content, so it is a
# statement about what went into this binary; compare against that. Same content, new mtime -> green.
# One byte changed with no rebuild -> red.
$hashRecordFile = Join-Path $buildDir 'source_hashes.txt'
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
    Add-Fail ('the staleness guard has no content record to compare against; expected ' +
              $hashRecordFile + ' (written by tools\build.ps1 after a successful build). A build ' +
              'that did not say what it compiled cannot be certified as current')
}

$pathFile = Join-Path $buildDir 'residue_selftest_path.txt'
$exe = $null
if (Test-Path $pathFile) {
    $exe = (Get-Content $pathFile -Raw).Trim()
}

$checked = 0

if (-not $exe -or -not (Test-Path $exe)) {
    # NOT a pass, and not a silent skip: name the path that was looked for.
    Add-Fail ("residue_selftest was not built; expected its path in " + $pathFile)
} else {
    # ★ STALENESS GUARD. A check that runs whatever exe happens to be on disk cannot tell "the code
    # passes" from "the code did not build and yesterday's exe passes", and the second reads as the
    # first in every log. If any source this suite covers is newer than the binary, that is a
    # FAILURE, not a warning: the number it would print measures a build nobody asked for.
    $srcRoots = @(
        (Join-Path $repo 'src\playback\residue'),
        (Join-Path $repo 'tests\residue')
    )
    $changed = @()
    foreach ($root in $srcRoots) {
        if (Test-Path $root) {
            foreach ($f in (Get-ChildItem -Path $root -Recurse -File -Include *.cpp, *.h)) {
                $why = Get-StaleReason $f.FullName
                if ($why) { $changed += $why }
            }
        }
    }
    if ($changed.Count -gt 0) {
        Add-Fail ("the binary was not built from " + $changed.Count + " of its sources, so this run would report on code that was never compiled; first: " + $changed[0])
    }

    $out = & $exe 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }

    # doctest's own summary: "assertions: 123 | 123 passed | 0 failed |"
    $line = ($out | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
    if (-not $line) {
        Add-Fail 'residue_selftest produced no doctest assertion summary; the count cannot be trusted'
    } elseif ($line -match 'assertions:\s+(\d+)\s+\|\s+(\d+)\s+passed\s+\|\s+(\d+)\s+failed') {
        $total = [int]$Matches[1]
        $failedCount = [int]$Matches[3]
        $checked = $total
        if ($failedCount -ne 0) {
            Add-Fail ("residue_selftest: $failedCount assertion(s) failed")
        }
        if ($total -lt 450) {
            Add-Fail ("residue_selftest asserted only $total times; the suite asserts well over that, so this means it did not run")
        }
    } else {
        Add-Fail ('could not parse the doctest summary line: ' + $line)
    }

    # ★★ THE POSITIVE CONTROL MUST HAVE RUN. Every guard case in this suite is a walk expected to
    # find NOTHING, and a builder that produced an unreachable world would pass all of them for
    # free. The control that proves the image is reachable is one named test case, so its absence
    # from the run is checked here rather than assumed from the total.
    $ctl = & $exe '--test-case=*positive control*' 2>&1
    $ctlLine = ($ctl | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
    if ($ctlLine -match 'assertions:\s+(\d+)\s+\|\s+(\d+)\s+passed\s+\|\s+(\d+)\s+failed') {
        if ([int]$Matches[1] -eq 0) {
            Add-Fail 'the positive control matched no test case; every negative result in this suite is then vacuous'
        } elseif ([int]$Matches[3] -ne 0) {
            Add-Fail 'the positive control FAILED; the synthetic image is unreachable and every guard case passes for free'
        }
    } else {
        Add-Fail 'the positive control run produced no summary'
    }

    # The roster in src/playback/residue/residue.cpp is the denominator coverage is measured against, so
    # it is pinned here: a completeness metric whose denominator is supplied by the work being measured
    # cannot fail. N-1 was reclassified and is not a residue row.
    # The residue table's rows N-2..N-20; the roster must name exactly these.
    $expectedIds = @(2..20 | ForEach-Object { "N-$_" } | Sort-Object -Unique)
    $rosterIds = @($out | ForEach-Object {
        if ($_ -match '^ROSTER-ID (N-\d+)$') { $Matches[1] } } | Sort-Object -Unique)

    if ($rosterIds.Count -eq 0) {
        Add-Fail 'the suite emitted no ROSTER-ID lines, so the denominator cross-check ran against nothing'
    } else {
        $missing = @($expectedIds | Where-Object { $rosterIds -notcontains $_ })
        $extra = @($rosterIds | Where-Object { $expectedIds -notcontains $_ })
        if ($missing.Count -gt 0) {
            Add-Fail ('the roster does not know these residue rows, so the coverage denominator is too small: ' + ($missing -join ', '))
        }
        if ($extra.Count -gt 0) {
            Add-Fail ('the roster claims rows the residue table does not have: ' + ($extra -join ', '))
        }
        if ($missing.Count -eq 0 -and $extra.Count -eq 0) {
            Write-Host ("roster denominator cross-checked: " + $expectedIds.Count + " rows, exact match")
        }
    }

    if ($code -ne 0) {
        Add-Fail ("residue_selftest exited $code")
    }
}

if ($SelfTest) {
    # ★ The count is mutation-tested rather than trusted. Re-run excluding the guard cases and assert
    # the number DROPS -- a substance count nobody has watched move is not a substance count.
    if ($exe -and (Test-Path $exe)) {
        $out2 = & $exe '--test-case-exclude=*resolves to nothing*' 2>&1
        $line2 = ($out2 | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
        if ($line2 -match 'assertions:\s+(\d+)') {
            $reduced = [int]$Matches[1]
            if ($reduced -ge $checked) {
                Add-Fail ("SelfTest: excluding the guard cases did not reduce the assertion count ($checked -> $reduced); the count does not measure what it claims")
            } else {
                Write-Host ("SelfTest: assertion count moved $checked -> $reduced when the guard cases were excluded")
            }
        } else {
            Add-Fail 'SelfTest: the reduced run produced no summary'
        }
    }
}

foreach ($f in $failed) { Write-Host ("FAIL: " + $f) }

if ($failed.Count -gt 0) {
    Write-Host 'GATE-RESULT checked=0 subject=residue'
    exit 1
}

Write-Host "GATE-RESULT checked=$checked subject=residue"
exit 0
