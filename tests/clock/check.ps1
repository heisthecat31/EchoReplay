# gate-tier: T0
#
# W2-D's gate entry point. Discovered by tools\gate.ps1 by convention: tests\<name>\check.ps1.
#
# Two instruments, and the second exists because the first cannot reach the thing this packet's
# whole pause design rests on:
#
#   clock_selftest  the classification and the rate meter, with the naive red arm compiled and run.
#   the bytes       CLOCK.MINELAPSED_OFF's runtime value, re-derived from echovr.exe ON DISK.
#
# ★★★ WHY THE SECOND ARM. This packet's decisive finding is that the CClock constructor leaves
# CLOCK.MINELAPSED_OFF at ZERO, which means the engine's own pause bit drives the scaled delta to 0
# and CLOCK.RECIP_OFF to 1.0f/0.0f = +inf -- and therefore that the 1 us floor is a precondition of
# pause rather than a nicety. That was derived from the offline IDA database. This arm reads the
# same instruction bytes out of the PE with a second reader, so the finding rests on two witnesses
# rather than on one tool. It carries its own known-answer control: the same comparison one byte
# along must REFUSE, or a match here would prove nothing.
#
# ★ WHAT THIS GATE COVERS, stated because a GATE-RESULT number with no scope is a claim: the
# arithmetic and the byte derivation. It covers NO engine memory, no CClock is written and no game
# is launched. The in-vivo half is parked in runs\queue\payload\W2-D.json. THIS GATE BEING GREEN IS
# NOT THE CLOCK WORKING.

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
$buildDir = Join-Path $repo 'build\W2-D'
if ($env:REPLAY_BUILD_DIR) { $buildDir = $env:REPLAY_BUILD_DIR }

$failed = New-Object System.Collections.ArrayList
function Add-Fail([string]$m) { [void]$failed.Add($m) }

$checked = 0

# ---------------------------------------------------------------------------------------------
# Arm 1: the native suite.
# ---------------------------------------------------------------------------------------------
$pathFile = Join-Path $buildDir 'clock_selftest_path.txt'
$exe = $null
if (Test-Path $pathFile) { $exe = (Get-Content $pathFile -Raw).Trim() }

if (-not $exe -or -not (Test-Path $exe)) {
    Add-Fail ("clock_selftest was not built; expected its path in " + $pathFile)
} else {
    # ★ STALENESS GUARD. A check that runs whatever exe happens to be on disk cannot tell "the code
    # passes" from "the code did not build and yesterday's exe passes", and the second reads as the
    # first in every log.
    #
    # ★★ BY CONTENT, NOT BY MTIME. The mtime form red on a NO-OP: eleven agents share this tree, and
    # any git checkout/stash/reset rewrites clock_policy.* byte-for-byte with a fresh timestamp, so
    # this gate went red for everyone over a file nobody had changed. A guard that reds on a no-op
    # is one people learn to mute, and a muted guard is worse than none. tools\build.ps1 already
    # records the SHA-256 of every tracked source in source_hashes.txt AFTER a successful build and
    # from the PRE-build content, so it is a statement about what went into this binary; compare
    # against that. Same content, new mtime -> green. One byte changed with no rebuild -> red.
    #
    # ★ SCOPED TO WHAT THIS BINARY ACTUALLY COMPILES, and that is the whole point. Watching all of
    # src\engine would turn this gate red every time any of the six packets that share that
    # directory saved a file -- a false red, which gets muted exactly as fast as a false green gets
    # trusted. clock_selftest links clock_policy.cpp and this directory, and nothing else.
    #
    # test_clock.cpp only, NOT the whole directory: probe_clock.cpp lives here too and is compiled
    # into probe_clock.dll, which this suite does not link. Watching it would red this gate every
    # time the probe changed, which is the same false red the src\engine narrowing already fixed.
    # release_check.h is watched WITH test_clock.cpp: the suite compiles it, and a mutation of
    # the header with no rebuild is exactly the stale-binary green this guard exists to refuse.
    #
    # Repo-relative with FORWARD slashes, because that is the key build.ps1 writes (it lists them
    # with git ls-files) and because the earlier backslash form carried a real TAB byte where \t
    # had been written -- the path never existed, Test-Path returned false, and this guard silently
    # watched neither file while reporting a count that looked right. And EVERY path that does not
    # resolve is a failure, the src/engine pair included: the first repair fixed the tests/clock
    # loop and left the loop above it skipping silently, which is the same defect two lines up.
    $watchedSources = @(
        'src/engine/clock_policy.cpp',
        'src/engine/clock_policy.h',
        'tests/clock/test_clock.cpp',
        'tests/clock/release_check.h'
    )

    # No record is NOT "nothing changed". A build directory with no source_hashes.txt cannot say
    # what its binaries were compiled from, so nothing here can be asserted.
    $hashRecordFile = Join-Path $buildDir 'source_hashes.txt'
    $recorded = $null
    if (-not (Test-Path $hashRecordFile)) {
        Add-Fail ('the staleness guard has no content record to compare against; expected ' +
                  $hashRecordFile + ' (written by tools\build.ps1 after a successful build). ' +
                  'A build that did not say what it compiled cannot be certified as current')
    } else {
        $recorded = @{}
        foreach ($line in (Get-Content -LiteralPath $hashRecordFile)) {
            $parts = $line -split ' ', 2
            if ($parts.Count -eq 2) { $recorded[$parts[1]] = $parts[0] }
        }
    }

    foreach ($rel in $watchedSources) {
        $full = Join-Path $repo $rel
        if (-not (Test-Path $full)) {
            Add-Fail ("the staleness guard cannot find $full, so it is watching nothing; a path " +
                      'that does not resolve is not a file that has not changed')
            continue
        }
        if ($null -eq $recorded) { continue }
        if (-not $recorded.ContainsKey($rel)) {
            Add-Fail ("$rel is compiled into clock_selftest but the build that produced it recorded " +
                      'no hash for it, so this run cannot tell the binary from a stale one')
            continue
        }
        $now = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
        if ($now -ne $recorded[$rel]) {
            Add-Fail ("$rel has changed since the binary was built (on disk $now, built from " +
                      $recorded[$rel] + '), so this run would report on code that was never compiled')
        }
    }

    $out = & $exe 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }

    $line = ($out | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
    if (-not $line) {
        Add-Fail 'clock_selftest produced no doctest assertion summary; the count cannot be trusted'
    } elseif ($line -match 'assertions:\s+(\d+)\s+\|\s+(\d+)\s+passed\s+\|\s+(\d+)\s+failed') {
        $total = [int]$Matches[1]
        $failedCount = [int]$Matches[3]
        $checked = $total
        if ($failedCount -ne 0) { Add-Fail ("clock_selftest: $failedCount assertion(s) failed") }
        # A suite that ran but asserted almost nothing is a defect in the suite, not a pass. The
        # floor sits well below the real count so it catches a suite that stopped running.
        if ($total -lt 200) {
            Add-Fail ("clock_selftest asserted only $total times; the sweep alone is 18 cells, so this means it did not run")
        }
    } else {
        Add-Fail ('could not parse the doctest summary line: ' + $line)
    }
    if ($code -ne 0) { Add-Fail ("clock_selftest exited $code") }
}

# ---------------------------------------------------------------------------------------------
# Arm 2: the CClock constructor's field defaults, read out of echovr.exe with a second reader.
# ---------------------------------------------------------------------------------------------
$kExpectedMd5 = 'caa05ee3bca2f5df0809994e93e2cc05'
$game = $env:REPLAY_GAME_EXE
if (-not $game) {
    . (Join-Path $PSScriptRoot '..\..\tools\echovr_path.ps1')
    $game = Find-EchoVrExe
}
if (-not $game) { $game = '(echovr.exe not found - set ECHOVR_DIR or REPLAY_ECHOVR_EXE)' }

function Get-FileOffset([byte[]]$img, [uint32]$rva) {
    $lfanew = [BitConverter]::ToInt32($img, 0x3C)
    $numSections = [BitConverter]::ToUInt16($img, $lfanew + 6)
    $sizeOptional = [BitConverter]::ToUInt16($img, $lfanew + 20)
    $sectionTable = $lfanew + 24 + $sizeOptional
    for ($i = 0; $i -lt $numSections; $i++) {
        $s = $sectionTable + ($i * 40)
        $virtSize = [BitConverter]::ToUInt32($img, $s + 8)
        $virtAddr = [BitConverter]::ToUInt32($img, $s + 12)
        $rawSize  = [BitConverter]::ToUInt32($img, $s + 16)
        $rawPtr   = [BitConverter]::ToUInt32($img, $s + 20)
        $span = $virtSize
        if ($rawSize -gt $span) { $span = $rawSize }
        if ($rva -ge $virtAddr -and $rva -lt ($virtAddr + $span)) {
            return [int]($rawPtr + ($rva - $virtAddr))
        }
    }
    return -1
}

function Test-Bytes([byte[]]$img, [uint32]$rva, [string]$wantHex, [string]$what) {
    $want = $wantHex.Split(' ') | ForEach-Object { [Convert]::ToByte($_, 16) }
    $off = Get-FileOffset $img $rva
    if ($off -lt 0) {
        Add-Fail ("could not map RVA 0x{0:X} to a file offset for {1}" -f $rva, $what)
        return $false
    }
    for ($i = 0; $i -lt $want.Length; $i++) {
        if ($img[$off + $i] -ne $want[$i]) {
            Add-Fail ("{0}: byte {1} at RVA 0x{2:X} is 0x{3:X2}, expected 0x{4:X2}" -f $what, $i, $rva, $img[$off + $i], $want[$i])
            return $false
        }
    }
    return $true
}

if (-not (Test-Path $game)) {
    # NOT a pass and NOT a silent skip: name the path that was looked for, and say what is not
    # covered as a result.
    Write-Host ("NOT-RUN: the constructor byte arm needs echovr.exe; not found at " + $game)
    Write-Host "         CLOCK.MINELAPSED_OFF == 0 therefore rests on ONE witness in this run."
} else {
    $md5 = (Get-FileHash -Path $game -Algorithm MD5).Hash.ToLower()
    if ($md5 -ne $kExpectedMd5) {
        Write-Host ("NOT-RUN: echovr.exe md5 is " + $md5 + ", not the measured build " + $kExpectedMd5)
        Write-Host "         Every offset in facts/ names arbitrary bytes in that file, so no byte is asserted."
    } else {
        $img = [IO.File]::ReadAllBytes($game)

        # CLOCK.CTOR's 20-byte prologue -- which contains `lea rax, off_1416CCDA0`, the store of
        # CLOCK.VTABLE into [this+0] that identifies this function as CClock's constructor.
        if (Test-Bytes $img 0xCD8F0 '40 57 48 83 EC 20 48 8B F9 48 8D 05 A0 F4 5F 01 48 89 01 48' 'CLOCK.CTOR prologue') { $checked++ }

        # xor eax, eax -- what makes the two stores below stores of ZERO rather than of something.
        if (Test-Bytes $img 0xCD907 '33 C0' 'CLOCK.CTOR zeroes rax') { $checked++ }

        # mov dword ptr [rdi+38h], 3F800000h -- CLOCK.TIMESCALE_OFF starts at 1.0f.
        if (Test-Bytes $img 0xCD920 'C7 47 38 00 00 80 3F' 'CLOCK.TIMESCALE_OFF default 1.0f') { $checked++ }

        # ★★★ mov [rdi+58h], rax -- CLOCK.MINELAPSED_OFF starts at ZERO. The whole pause refusal.
        if (Test-Bytes $img 0xCD93E '48 89 47 58' 'CLOCK.MINELAPSED_OFF default 0') { $checked++ }

        # mov qword ptr [rdi+98h], 8235h -- CLOCK.STEPONCE_OFF starts at 33333, not at 0, so the
        # step-once arm does not itself produce a zero raw delta before we ever write it.
        if (Test-Bytes $img 0xCD96F '48 C7 87 98 00 00 00 35 82 00 00' 'CLOCK.STEPONCE_OFF default 33333') { $checked++ }

        # ★ KNOWN-ANSWER CONTROL. The same comparison one byte along must REFUSE. Without it every
        # assertion above could be passing vacuously -- a comparison nobody has watched fail is not
        # a comparison.
        $off = Get-FileOffset $img 0xCD93F
        $want = @(0x48, 0x89, 0x47, 0x58)
        $differs = $false
        for ($i = 0; $i -lt 4; $i++) { if ($img[$off + $i] -ne $want[$i]) { $differs = $true } }
        if (-not $differs) {
            Add-Fail 'the control did not refuse: the four bytes one past CLOCK.MINELAPSED_OFF''s store matched it, so the comparison proves nothing'
        } else {
            $checked++
        }

        # And the header must still claim what the bytes say. Two files, one fact.
        $factsHeader = Get-Content (Join-Path $repo 'src\engine\clock_facts.h') -Raw
        if ($factsHeader -notmatch 'kCtorMinElapsedUs\s*=\s*0u') {
            Add-Fail 'src/engine/clock_facts.h no longer records kCtorMinElapsedUs as 0, but the bytes still do'
        } else {
            $checked++
        }
    }
}

if ($SelfTest) {
    # ★ The count is mutation-tested rather than trusted: re-run with the sweep excluded and assert
    # the number DROPS. A substance count nobody has watched move is not a substance count.
    if ($exe -and (Test-Path $exe)) {
        $out2 = & $exe '--test-case-exclude=*masquerade*' 2>&1
        $line2 = ($out2 | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
        if ($line2 -match 'assertions:\s+(\d+)') {
            $reduced = [int]$Matches[1]
            if ($reduced -ge $checked) {
                Add-Fail ("SelfTest: excluding the sweep did not reduce the assertion count ($checked -> $reduced)")
            } else {
                Write-Host ("SelfTest: assertion count moved $checked -> $reduced when the sweep was excluded")
            }
        } else {
            Add-Fail 'SelfTest: the reduced run produced no summary'
        }
    }
}

foreach ($f in $failed) { Write-Host ("FAIL: " + $f) }

if ($failed.Count -gt 0) {
    Write-Host 'GATE-RESULT checked=0 subject=clock'
    exit 1
}

Write-Host "GATE-RESULT checked=$checked subject=clock"
exit 0
