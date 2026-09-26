# gate-tier: T0
#
# W2-B's gate entry point. Discovered by tools\gate.ps1 by convention: tests\<name>\check.ps1.
#
# Two instruments, each judged three ways -- exit code, doctest's own assertion summary, and a floor
# under that count. A suite that exits 0 having asserted nothing must never be indistinguishable from
# one that passed, which is why the count is extracted rather than inferred from the exit code.
#
# ★ WHAT THIS GATE COVERS, stated here because a GATE-RESULT number with no scope is a claim:
#   playback_selftest -- the transport arithmetic (rate invariant, whole-recorded-tick steps, pause,
#   step, end stops, hitch absorption) and the resolver (G2), both with their red arms compiled and
#   asserted to diverge.
#   playback_armtest  -- that playback_module.cpp's Init actually CALLS audio::ArmAudioResidue and
#   residue::ArmPlaybackResidue, that the battery it builds holds all three tables and no id twice,
#   and that a seek reaches both packets' fire paths. This is the one thing playback_selftest cannot
#   see: it links the library, and the library is identical whether or not the module exists.
#
# It covers NO engine memory. ApplyStateAt, the one-node history ring and the interpolator agreement
# are gates G3/G4 in docs\spikes\W2-B.md and are parked in runs\queue\payload.json for a launch. This
# gate being green is not playback working.

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
$buildDir = Join-Path $repo 'build\W2-B'
if ($env:REPLAY_BUILD_DIR) { $buildDir = $env:REPLAY_BUILD_DIR }

# ★★ TWO BUCKETS, BECAUSE "ASKED AND FAILED" AND "COULD NOT ASK" ARE DIFFERENT ANSWERS AND THIS
# SCRIPT USED TO GIVE THEM THE SAME ONE.
#
# tools\gate.ps1 reads a script check three ways: exit 97 is NOT-RUN, exit 0 with checked=0 is
# NOT-RUN, and ANY OTHER non-zero exit is FAIL. So exiting 1 because the binaries were never built
# reports a W2-B failure -- when what actually happened, four times today, is that another packet left
# the tree unable to configure or link and nothing of this packet's was ever run. Both outcomes are
# RED and neither is a pass; the difference is who goes and looks.
#
# Blocked => exit 0 with checked=0, which the gate records as NOT-RUN.
# Failed  => exit 1, which the gate records as FAIL.
$failed = New-Object System.Collections.ArrayList
$blocked = New-Object System.Collections.ArrayList
function Add-Fail([string]$m) { [void]$failed.Add($m) }
function Add-Blocked([string]$m) { [void]$blocked.Add($m) }

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
    Add-Blocked ('the staleness guard has no content record to compare against; expected ' +
                 $hashRecordFile + ' (written by tools\build.ps1 after a successful build). A build ' +
                 'that did not say what it compiled cannot be certified as current')
}

# The path CMake wrote. Searching for the binary ourselves would be a second source of truth about
# where it lives, and the two would drift the first time a generator changed.
function Resolve-Instrument([string]$name) {
    $pathFile = Join-Path $buildDir ($name + '_path.txt')
    if (-not (Test-Path $pathFile)) {
        # NOT a pass, and not a silent skip: name the path that was looked for. Blocked rather
        # than failed -- the binary is absent, so nothing of this packet's was asked.
        Add-Blocked ("$name was not built; expected its path in " + $pathFile)
        return $null
    }
    $exe = (Get-Content $pathFile -Raw).Trim()
    if (-not $exe -or -not (Test-Path $exe)) {
        Add-Blocked ("$name was not built; " + $pathFile + " names " + $exe)
        return $null
    }
    return $exe
}

# ★ STALENESS GUARD, and it exists because this gate reported GREEN on a stale binary while the tree
# would not even configure. A check that runs whatever exe happens to be on disk cannot tell "the code
# passes" from "the code did not build and yesterday's exe passes" -- and the second reads as the
# first in every log. If any source this instrument covers is newer than it, that is a FAILURE, not a
# warning: the number it would print measures a build nobody asked for.
#
# ★ The file list is now per-instrument rather than "everything under tests\playback" -- and that is a
# correction, not a relaxation. The two binaries do not compile the same files: playback_selftest
# links the LIBRARY and contains neither playback_module.cpp nor the second test source, so a shared
# list failed it every time either of those was touched, which is a guard that cries wolf until
# somebody widens it in a hurry. Each list now names what its binary actually contains, and between
# the two of them every file under src\playback, src\audio and tests\playback is still covered by the
# binary that holds it.
function Assert-Fresh([string]$name, [string]$exe, [string[]]$roots, [string[]]$files, [string[]]$notMine) {
    $changed = @()
    foreach ($root in $roots) {
        if (Test-Path $root) {
            foreach ($f in (Get-ChildItem -Path $root -Recurse -File -Include *.cpp, *.h)) {
                if ($notMine -contains $f.FullName) { continue }
                $why = Get-StaleReason $f.FullName
                if ($why) { $changed += $why }
            }
        }
    }
    foreach ($f in $files) {
        if (Test-Path $f) {
            $why = Get-StaleReason $f
            if ($why) { $changed += $why }
        }
    }
    if ($changed.Count -gt 0) {
        Add-Blocked ($name + " was not built from " + $changed.Count + " of its sources, so the question this gate exists to ask was never asked of the code in the tree; first: " + $changed[0])
    }
}

# Runs one doctest binary and returns how many assertions it reported, or 0 on any doubt.
function Invoke-Instrument([string]$name, [string]$exe, [int]$floor) {
    $out = & $exe 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }

    $total = 0
    # doctest's own summary: "assertions: 123 | 123 passed | 0 failed |"
    $line = ($out | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
    if (-not $line) {
        Add-Fail "$name produced no doctest assertion summary; the count cannot be trusted"
    } elseif ($line -match 'assertions:\s+(\d+)\s+\|\s+(\d+)\s+passed\s+\|\s+(\d+)\s+failed') {
        $total = [int]$Matches[1]
        $failedCount = [int]$Matches[3]
        if ($failedCount -ne 0) {
            Add-Fail ("$name" + ": $failedCount assertion(s) failed")
        }
        # A suite that ran but asserted almost nothing is a defect in the suite, not a pass. The floor
        # is well below the real count so it catches a suite that stopped running, not one that
        # legitimately changed size.
        if ($total -lt $floor) {
            Add-Fail ("$name asserted only $total times, below its floor of $floor; this means it did not run")
        }
    } else {
        Add-Fail ("$name" + ': could not parse the doctest summary line: ' + $line)
    }

    if ($code -ne 0) {
        Add-Fail ("$name exited $code")
    }
    return $total
}

$checked = 0
# ★ COUNTED, BECAUSE AN INSTRUMENT THAT NEVER RAN LOOKED EXACTLY LIKE ONE THAT PASSED. A variable
# named $selftest here silently bound to the [switch]$SelfTest parameter above, the assignment threw,
# the `if` fell through, and this gate printed GREEN on the arm test alone with 72,060 assertions
# never executed. Naming both instruments in one place and requiring both to report is what makes
# that unrepresentable rather than a thing to be careful about.
$expectedInstruments = 2
$ran = 0

$mainExe = Resolve-Instrument 'playback_selftest'
$mainCount = 0
if ($mainExe) {
    # playback_module.cpp is compiled onto the DLL and into playback_armtest, never into the library
    # this binary links -- so it is the arm test's source, and it is checked there.
    Assert-Fresh 'playback_selftest' $mainExe @((Join-Path $repo 'src\playback\core')) `
        @((Join-Path $repo 'tests\playback\test_playback.cpp')) `
        @((Join-Path $repo 'src\playback\core\playback_module.cpp'))
    $mainCount = Invoke-Instrument 'playback_selftest' $mainExe 1000
    $checked += $mainCount
    $ran += 1
}

$armExe = Resolve-Instrument 'playback_armtest'
if ($armExe) {
    # It compiles W2-H's and W3-A's module translation units too, so their sources are its sources.
    Assert-Fresh 'playback_armtest' $armExe `
        @((Join-Path $repo 'src\playback'), (Join-Path $repo 'src\audio')) `
        @((Join-Path $repo 'tests\playback\test_playback_arm.cpp'))
    $checked += (Invoke-Instrument 'playback_armtest' $armExe 25)
    $ran += 1
}

if ($ran -ne $expectedInstruments) {
    Add-Blocked ("only $ran of $expectedInstruments instruments ran; a gate result covering fewer instruments than it claims is worse than no result")
}

if ($SelfTest) {
    # ★ The count is mutation-tested rather than trusted. Re-run with a filter that excludes the
    # resolver cases and assert the number DROPS -- a substance count nobody has watched move is not
    # a substance count.
    if ($mainExe) {
        $out2 = & $mainExe '--test-case-exclude=*cursor*' 2>&1
        $line2 = ($out2 | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
        if ($line2 -match 'assertions:\s+(\d+)') {
            $reduced = [int]$Matches[1]
            if ($reduced -ge $mainCount) {
                Add-Fail ("SelfTest: excluding the cursor cases did not reduce the assertion count ($mainCount -> $reduced); the count does not measure what it claims")
            } else {
                Write-Host ("SelfTest: assertion count moved $mainCount -> $reduced when the cursor cases were excluded")
            }
        } else {
            Add-Fail 'SelfTest: the reduced run produced no summary'
        }
    }
}

# AUD-W2-B-31 F3 -- ONE definition of the [identity] anchor. The L3 #29 acceptance patterns and
# every line the seat writes have to carry the same module string; two spellings of it is two
# things to rename. viewer_role.cpp owns the definition, playback_module.cpp takes it from there,
# and this check READS it rather than repeating it, so a rename fails here before a launch is spent
# scoring a regex that matches nothing.
$viewerSrc = Join-Path $repo "src/playback/core/viewer_role.cpp"
if (-not (Test-Path $viewerSrc)) {
    Add-Fail "anchor: viewer_role.cpp is missing, so the [identity] module string has no definition"
} else {
    $m = Select-String -Path $viewerSrc -Pattern 'kViewerLogModule\s*=\s*"([^"]+)"' | Select-Object -First 1
    if (-not $m) {
        Add-Fail "anchor: viewer_role.cpp defines no kViewerLogModule string literal"
    } else {
        $anchor = $m.Matches[0].Groups[1].Value
        $checked++
        # The acceptance regexes, built FROM the anchor rather than beside it.
        $oursRe    = '^.*\[' + [regex]::Escape($anchor) + '\] viewer slot 0 role=2 \(spectator\)\s*$'
        $preseatRe = '^.*\[' + [regex]::Escape($anchor) + '\] viewer slot 0 role=[0-7] \(.*\) preseat\s*$'
        if ($anchor -ne 'identity') {
            Add-Fail ("anchor: kViewerLogModule is now {0} -- ENG-W2-B-31 and the L3 #29 clauses anchor on [identity]; update both together" -f $anchor)
        }
        $modSrc = Join-Path $repo "src/playback/core/playback_module.cpp"
        if ((Select-String -Path $modSrc -Pattern 'kViewerModuleName\s*=\s*kViewerLogModule').Count -ne 1) {
            Add-Fail "anchor: playback_module.cpp no longer takes its module string from kViewerLogModule -- the census line can drift from the seat lines"
        }
        Write-Host ("playback: [identity] anchor = {0}; ours=/{1}/ preseat=/{2}/" -f $anchor, $oursRe, $preseatRe)
    }
}

foreach ($f in $failed) { Write-Host ("FAIL: " + $f) }
foreach ($b in $blocked) { Write-Host ("NOT-RUN: " + $b) }

# A real failure outranks a blockage: if anything this packet owns actually failed, say so plainly
# even when something else was also missing.
if ($failed.Count -gt 0) {
    Write-Host 'GATE-RESULT checked=0 subject=playback'
    exit 1
}

if ($blocked.Count -gt 0) {
    # checked=0 on a clean exit is the gate's own NOT-RUN channel: still red, still not a pass, and
    # attributed to the thing that was missing rather than to this packet's code.
    Write-Host ("playback: NOT RUN - {0} blocking condition(s); no verdict was obtained." -f $blocked.Count)
    Write-Host 'GATE-RESULT checked=0 subject=playback'
    exit 0
}

Write-Host "GATE-RESULT checked=$checked subject=playback"
exit 0
