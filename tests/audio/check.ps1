# W2-H's gate check.
#
# Eight arms, and the second, fourth, fifth, sixth and seventh are the ones that carry a negative:
#
#   1. audio_selftest    the rate ladder, the A/V bias, the O4 verdict, the pin codec, the A6
#                        selector-versus-mapped-runtime classifier, the memo walk against a
#                        synthetic CSoundPlayer, and the residue battery. The reported count is
#                        doctest's own assertion total, so deleting a CHECK lowers it.
#                        (The S_VOIP record codec was HERE and is deleted: capture::VoipRec is the
#                        format, and a second unwired statement of one record is a second source of
#                        truth. Its 38 assertions went with it.)
#   2. audio_exportcheck against the shipped echovr.exe, PLUS its --mutate arm. The design's
#                        "the control surface is offset-free" is only as good as the decorated names
#                        akbind.cpp carries, and a name is the one kind of fact this project cannot
#                        anchor with a displacement. The mutation asks for a name that cannot exist
#                        and requires the check to notice.
#   3. recvguard.py bytes arm: the six sites where VOIP.RECEIVE reads its packet header are
#                        byte-exact in the shipped binary.
#   4. recvguard.py sweep arm plus its mutation: +4 is the ONLY displacement read off that argument,
#                        which is what makes the six-byte replay stub sufficient. This arm needs the
#                        IDA extraction and reports NOT RUN naming the path when it is absent --
#                        because "we could not establish the negative" is not "the negative holds".
#   4b. probe_audio.dll's own freshness. It is the one artifact no offline arm runs and the one a
#                        sweep actually loads into the game, and the harness records its stamp
#                        without being able to tell whether the code in it is this tree's.
#   5. the probe descriptor held against the binary it describes, and against the harness's own
#                        reader. Three negatives: a guard declared without the mechanism, a
#                        mechanism without the declaration, and a field the harness silently drops.
#                        The last one reads the known-field list OUT of the test harness rather
#                        than keeping a second copy, so a rename there reddens here instead of
#                        quietly discarding a declaration -- which is exactly how carriesBootstrap
#                        came to be read as UNKNOWN for every probe that set it.
#   6. the check ledger, held against the artifact a probe writes. The ceiling, the check names
#                        and the reserved words are IMPORTED from audio_ledgerdump.exe -- the
#                        compiled checkledger.h, printed by the code that owns it -- never parsed
#                        out of the header or read out of the artifact. Four committed records must
#                        be caught (launch 13, f870d62's own provisional record, a provisional
#                        record with a forgotten check, a final record still calling a check
#                        pending) and three records the writer produces live must pass.
#   7. the harness coupling this probe rests on, tested from the consumer's side: the test harness
#                        is dot-sourced with -DefineOnly and its own finality reader is fed the
#                        writer's records. A provisional record must not be final, the in-progress
#                        word must not be one the harness tallies, and a record that lost its
#                        `provisional` field must still not end a launch.
#
# ★ WHAT THIS CHECK CANNOT SEE, stated so its blind spot is bounded. Nothing here runs the game, so
# nothing here establishes that SetFrequencyRatio moves the audio clock, that the sink proofs pass on
# real memory, that a reconcile fixes real music, or that an injected VOIP frame is audible. Those are
# O4 and the in-game probe queued at runs/queue/W2-H.probe.json. A green result from this file means
# the offline half is correct, and says nothing about the in-vivo half.
#
# ★ AND THE CEILING IS THE PACKET'S OWN DECLARATION. The design does not enumerate the eight
# scored checks, so there is no source outside this packet to hold checkledger.h against. A change
# that removes a check from the enum, the probe and the offline suite together is caught by nothing
# here; that residual is stated in docs/spikes/W2-H.md rather than papered over with a second regex.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

$bin = Join-Path $build 'bin'
$fail = 0
$notRun = 0

# ---------------------------------------------------------------------------------------------------
# ★ THE STALENESS GUARD, AND IT EXISTS BECAUSE THIS PACKET WAS HANDED A GREEN FROM A BINARY THAT
# PREDATED THE CHANGE IT WAS ASKED TO PROVE.
#
# Verifying two comment edits, the build ran in build/W2-H, CMake's GENERATE step failed on another
# packet's src/control/CMakeLists.txt, ninja therefore rebuilt nothing, and the previous exe ran and
# reported 351 of 351 assertions passing. Every line of that output was true and none of it was
# evidence about the edited code. A test result is only evidence about a source file if the build
# actually recompiled it, and "the code passes" reads identically to "the code did not build and
# yesterday's exe passes" in every log.
#
# So this is a FAILURE, not a warning: the number the run would otherwise print measures a build
# nobody asked for, and a warning next to a green total is read as a green total. It names the count
# and the first offending file, because "something is stale" sends the reader looking and
# "audiopins.cpp is newer than the exe" does not.
#
# Same rule W2-B reached from its own stale green and W2-A reached by building on an overlay; the
# shape here is deliberately W2-B's so there is one convention rather than three.
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
    (Join-Path $repo 'src\audio'),
    (Join-Path $repo 'tests\audio')
)
$srcFiles = @()
foreach ($root in $srcRoots) {
    if (Test-Path $root) {
        $srcFiles += Get-ChildItem -Path $root -Recurse -File -Include *.cpp, *.h
    }
}
# src/engine/audiostate.* is this packet's too, and it is the file the memo walk lives in -- covering
# src/audio alone would leave the N-16 implementation outside the guard that exists for it.
$engineDir = Join-Path $repo 'src\engine'
if (Test-Path $engineDir) {
    $srcFiles += Get-ChildItem -Path $engineDir -File -Filter 'audiostate.*'
    $srcFiles += Get-ChildItem -Path $engineDir -File -Filter 'audiorate.*'
}

# $NotCompiledBy names the files this binary does not translate. It is not a loophole: a guard that
# calls checkledger.h a source of audio_exportcheck.exe is making a FALSE claim, and it is one ninja
# can never satisfy, because that exe has no edge to that header -- so the only way to green it is to
# touch an unrelated file, which is how a guard stops being read. Each name is required to exist in
# the source list, so a rename reddens here instead of quietly excluding nothing. $OnlyThese is the
# same statement from the other side, for a binary whose whole source set is two files.
function Assert-Fresh([string]$exePath, [string]$label, [string[]]$NotCompiledBy = @(), [string[]]$OnlyThese = @()) {
    if (-not (Test-Path $exePath)) { return $true }
    if ($script:srcFiles.Count -eq 0) {
        Write-Host "FAIL: the staleness guard found no sources to compare against, so it would pass whatever it was given"
        return $false
    }
    foreach ($name in ($NotCompiledBy + $OnlyThese)) {
        if (-not (@($script:srcFiles | Where-Object { $_.Name -eq $name }))) {
            Write-Host "FAIL: $label names '$name' in its staleness guard, but no such file is in the source list. A stale name guards nothing and hides that it does."
            return $false
        }
    }
    if ($OnlyThese.Count -gt 0) {
        $mine = @($script:srcFiles | Where-Object { $OnlyThese -contains $_.Name })
    } else {
        $mine = @($script:srcFiles | Where-Object { $NotCompiledBy -notcontains $_.Name })
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

# Which translation unit belongs to which binary, from tests\audio\CMakeLists.txt: audio_selftest is
# test_audio.cpp, audio_exportcheck is exportcheck.cpp, probe_audio.dll is probe_audio.cpp,
# audio_ledgerdump is ledgerdump.cpp, and checkledger.h is included by all but exportcheck.
# audio_wiretest is test_audio_wire.cpp plus the shipping audio_module.cpp.
$selftestSkips = @('exportcheck.cpp', 'probe_audio.cpp', 'ledgerdump.cpp', 'test_audio_wire.cpp')
$exportcheckSkips = @('test_audio.cpp', 'probe_audio.cpp', 'ledgerdump.cpp', 'checkledger.h', 'test_audio_wire.cpp')
$probeSkips = @('test_audio.cpp', 'exportcheck.cpp', 'ledgerdump.cpp', 'test_audio_wire.cpp')
$ledgerdumpOnly = @('ledgerdump.cpp', 'checkledger.h')
$wiretestSkips = @('test_audio.cpp', 'exportcheck.cpp', 'probe_audio.cpp', 'ledgerdump.cpp', 'checkledger.h')

# ---- 1. the offline suite -------------------------------------------------------------------------
$selftest = Join-Path $bin 'audio_selftest.exe'
if (-not (Test-Path $selftest)) {
    Write-Host "NOT RUN: $selftest was not built"
    $notRun++
} elseif (-not (Assert-Fresh $selftest 'audio_selftest.exe' $selftestSkips)) {
    $fail++
} else {
    $out = & $selftest 2>&1
    $out | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL: audio_selftest'; $fail++ }

    # A suite that ran but asserted almost nothing is a defect in the suite, not a pass -- and it is
    # the other way a green total lies. The floor is well under the real count so it catches a suite
    # that stopped running, not one that legitimately changed size.
    $line = ($out | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
    if (-not $line) {
        Write-Host 'FAIL: audio_selftest produced no doctest assertion summary; the count cannot be trusted'
        $fail++
    } elseif ($line -match 'assertions:\s+(\d+)\s+\|\s+(\d+)\s+passed\s+\|\s+(\d+)\s+failed') {
        $total = [int]$Matches[1]
        if ($total -lt 200) {
            Write-Host "FAIL: audio_selftest asserted only $total times; the suite carries over 350, so this means it did not run"
            $fail++
        }
    } else {
        Write-Host ('FAIL: could not parse the doctest summary line: ' + $line)
        $fail++
    }
}

# ---- 1b. the wiring arm: the module tick drives IAudioRate from the transport ---------------------
# DIAG-WIRING gap 4: SetRate/Suspend had no caller on the shipping path, so pause, slow-motion and
# fast-forward never reached Wwise while every unit test stayed green. This arm runs the real module
# TU through modreg and asserts the sink was commanded; audio_selftest cannot see that from outside.
$wiretest = Join-Path $bin 'audio_wiretest.exe'
if (-not (Test-Path $wiretest)) {
    Write-Host "NOT RUN: $wiretest was not built"
    $notRun++
} elseif (-not (Assert-Fresh $wiretest 'audio_wiretest.exe' $wiretestSkips)) {
    $fail++
} else {
    $out = & $wiretest 2>&1
    $out | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL: audio_wiretest'; $fail++ }
    $line = ($out | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
    if (-not $line) {
        Write-Host 'FAIL: audio_wiretest produced no doctest assertion summary; the count cannot be trusted'
        $fail++
    } elseif ($line -match 'assertions:\s+(\d+)\s+\|\s+(\d+)\s+passed\s+\|\s+(\d+)\s+failed') {
        if ([int]$Matches[1] -lt 10) {
            Write-Host ("FAIL: audio_wiretest asserted only " + $Matches[1] + " times; the arm carries 18, so it did not run")
            $fail++
        }
    } else {
        Write-Host ('FAIL: could not parse the doctest summary line: ' + $line)
        $fail++
    }
}

# ---- 2. the export arm, and the mutation that proves it looks ------------------------------------
$exportcheck = Join-Path $bin 'audio_exportcheck.exe'
. (Join-Path $PSScriptRoot '..\..\tools\echovr_path.ps1')
$exe = Find-EchoVrExe
if (-not $exe) { $exe = '(echovr.exe not found - set ECHOVR_DIR or REPLAY_ECHOVR_EXE)' }

if (-not (Test-Path $exportcheck)) {
    Write-Host "NOT RUN: $exportcheck was not built"
    $notRun++
} elseif (-not (Assert-Fresh $exportcheck 'audio_exportcheck.exe' $exportcheckSkips)) {
    # It compiles akbind.cpp's symbol table in, so a stale one checks yesterday's name list.
    $fail++
} elseif (-not (Test-Path $exe)) {
    Write-Host "NOT RUN: echovr.exe is not at $exe, so the AK export names were not checked"
    $notRun++
} else {
    # ★ EXIT 2 IS "COULD NOT ASK", AND IT USED TO READ AS "ASKED AND FAILED".
    #
    # exportcheck returns 2 for three conditions that are not findings -- no path given, no
    # PE32+ export table readable, an export directory that did not parse -- and this arm
    # collapsed every non-zero code into one FAIL whose printed reason was 'an AK name
    # akbind.cpp calls is not exported'. That sentence is FALSE for all three: it names a
    # defect in akbind.cpp for a binary the check could not read. The Test-Path above only
    # covers the file being absent; a truncated, non-PE32+ or unparseable copy walked
    # straight into the wrong verdict.
    & $exportcheck $exe | ForEach-Object { Write-Host $_ }
    $rc = $LASTEXITCODE
    if ($rc -eq 1) { Write-Host 'FAIL: an AK name akbind.cpp calls is not exported'; $fail++ }
    elseif ($rc -ne 0) {
        Write-Host "NOT RUN: exportcheck exited $rc without reading the export table; the AK names were NOT checked"
        $notRun++
    }

    # Same split on the mutation. A planted name that could not be planted is not a control
    # that passed, and it is not a control that failed either.
    & $exportcheck $exe --mutate | ForEach-Object { Write-Host $_ }
    $rc = $LASTEXITCODE
    if ($rc -eq 1) {
        Write-Host 'FAIL: the export check did not notice a name that cannot exist'
        $fail++
    }
    elseif ($rc -ne 0) {
        Write-Host "NOT RUN: the export mutation exited $rc, so the control did not run and proves nothing"
        $notRun++
    }
}

# ---- 3 and 4. the ReceiveVoip header guard --------------------------------------------------------
$py = Get-Command python -ErrorAction SilentlyContinue
$guard = Join-Path $PSScriptRoot 'recvguard.py'
if (-not $py) {
    Write-Host 'NOT RUN: python is not on PATH, so the VOIP header guard did not run'
    $notRun++
} else {
    & python $guard --exe $exe | ForEach-Object { Write-Host $_ }
    $rc = $LASTEXITCODE
    if ($rc -eq 1) { Write-Host 'FAIL: recvguard'; $fail++ }
    elseif ($rc -eq 2) { $notRun++ }

    & python $guard --exe $exe --mutate | ForEach-Object { Write-Host $_ }
    $rc = $LASTEXITCODE
    if ($rc -eq 1) { Write-Host 'FAIL: a planted read at +8 did not fail the sweep'; $fail++ }
    elseif ($rc -eq 2) { $notRun++ }
}

# ---- 4b. the in-vivo binary the harness will install ---------------------------------------------
#
# probe_audio.dll is the one artifact here that no offline arm runs, and it is the one a sweep
# actually loads into the game. A stale DLL would answer a launch with code nobody in this tree
# wrote, and the harness records its size and mtime as DllStamp without being able to tell.
$probeDll = Join-Path $bin 'probe_audio.dll'
if (-not (Test-Path $probeDll)) {
    Write-Host "NOT RUN: $probeDll was not built, so a sweep would install nothing for this packet"
    $notRun++
} elseif (-not (Assert-Fresh $probeDll 'probe_audio.dll' $probeSkips)) {
    $fail++
}

# ---- 6. the check ledger, held against the artifact the probe actually writes --------------------
#
# ★ THIS IS THE ARM THAT WOULD HAVE CAUGHT LAUNCH 13. That run filed `verdict: FAIL, checked: 4` and
# stated nothing about what four was out of. Four of the eight scored checks -- the whole rate ladder
# -- sat inside `if (proven > 0)` and were never asked, and a counter that counts only what executed
# renders a truncated run and a short one identically. So the artifact now carries its own
# denominator and names every check it did not ask, WITH A REASON.
#
# ★★ THE DENOMINATOR IS NOT TAKEN FROM THE ARTIFACT, AND IT IS NOT PARSED OUT OF THE HEADER EITHER.
# A coverage figure supplied by the work being measured cannot fail -- launch 13 would have said
# "4 of 4". The earlier revision of this arm ran two regexes over checkledger.h and called their
# agreement "derived twice"; that was one source counted twice, and an audit shrank the enumeration
# by one and watched it stay green. Now the ceiling, the names and the reserved words are IMPORTED:
# audio_ledgerdump.exe is checkledger.h compiled, printing its own table, and the sample records it
# emits come from the same FormatRecord the probe writes with. What is held against the artifact is
# the writer.
#
# ★★★ AND THE CHECKER IS PROVEN AGAINST THE RECORDS IT WAS WRITTEN FOR. Four committed fixtures must
# each be caught, or the arm fails on that instead of passing:
#   launch13-done.json          the run with no denominator at all (a byte copy; runs\ is gitignored)
#   provisional-f870d62.json    the provisional record f870d62's probe wrote from inside its sink
#                               wait, compiled from that commit's checkledger.h: "verdict": "PASS"
#                               with seven checks rendered as forgotten. The packet's own gate went
#                               red on the packet's own designed artifact.
#   provisional-forgotten.json  a provisional record with one check genuinely forgotten -- the
#                               control that keeps "provisional" from becoming the new exemption.
#   final-pending-reason.json   a final record still calling a check "not yet reached": a probe
#                               that filed its pending reasons as its answer.
function Get-LedgerViolations($obj, $table) {
    $v = New-Object System.Collections.ArrayList
    $names = @($obj.PSObject.Properties.Name)
    $ceiling = [int]$table.checkCeiling
    if ($names -notcontains 'checkCeiling') {
        [void]$v.Add('states no denominator (no checkCeiling), so "checked" is a count of what ran and cannot be read as coverage')
    } elseif ([int]$obj.checkCeiling -ne $ceiling) {
        [void]$v.Add("declares checkCeiling $([int]$obj.checkCeiling) but the ledger scores $ceiling")
    }
    if ($names -notcontains 'notAsked') {
        [void]$v.Add('does not say how many checks were not asked, so a truncated run and a short one render identically')
        return $v
    }
    $checked = if ($names -contains 'checked') { [int]$obj.checked } else { -1 }
    $notAsked = [int]$obj.notAsked
    if ($checked -lt 0) { [void]$v.Add('has notAsked but no checked') }
    elseif (($checked + $notAsked) -ne $ceiling) {
        [void]$v.Add("checked ($checked) + notAsked ($notAsked) = $($checked + $notAsked), not $ceiling")
    }
    $unasked = @()
    if ($names -contains 'unasked') { $unasked = @($obj.unasked) }
    if ($unasked.Count -ne $notAsked) {
        [void]$v.Add("says notAsked=$notAsked but names $($unasked.Count) of them")
    }
    $seen = @{}
    foreach ($u in $unasked) {
        $check = [string]$u.check
        if (@($table.checks) -notcontains $check) { [void]$v.Add("names an unasked check '$check' that the ledger does not score") }
        if ($seen.ContainsKey($check)) { [void]$v.Add("names '$check' as unasked twice") }
        $seen[$check] = $true
    }

    # Absent is not "final": the harness reads a record with no `provisional` as undeclared and lets
    # the launch run to its deadline, which is a different outcome from either value.
    if ($names -notcontains 'provisional') {
        [void]$v.Add('does not declare whether it is provisional, so neither the harness nor a reader can tell a verdict from a snapshot')
        return $v
    }
    $verdict = [string]$obj.verdict
    if ([bool]$obj.provisional) {
        if ($verdict -ne [string]$table.inProgressVerdict) {
            [void]$v.Add("is provisional and carries verdict '$verdict'; a record written mid-run is not a verdict and must say '$([string]$table.inProgressVerdict)'")
        }
        foreach ($u in $unasked) {
            $reason = [string]$u.reason
            if (-not $reason -or $reason -eq [string]$table.forgottenReason) {
                [void]$v.Add("check '$([string]$u.check)' was not asked and no reason was recorded: forgotten, not excluded, on a record that should have called it pending")
            }
        }
        return $v
    }
    if ($verdict -eq [string]$table.inProgressVerdict) {
        [void]$v.Add("is final and still carries the in-progress word")
    } elseif ($table.finalWords -notcontains $verdict) {
        [void]$v.Add("is final and carries verdict '$verdict', which is not a word the harness tallies (" + ($table.finalWords -join ', ') + ")")
    }
    foreach ($u in $unasked) {
        $reason = [string]$u.reason
        if (-not $reason -or $reason -eq [string]$table.forgottenReason) {
            [void]$v.Add("check '$([string]$u.check)' was not asked and no reason was recorded: forgotten, not excluded")
        } elseif ($reason -eq [string]$table.pendingReason) {
            [void]$v.Add("check '$([string]$u.check)' is still called '$reason' on a final record: the probe filed its pending state as its answer")
        }
    }
    if ($verdict -eq 'PASS' -and $notAsked -ne 0) {
        [void]$v.Add("says PASS with $notAsked check(s) unasked; PASS means every check was asked")
    }
    if ($names -notcontains 'completionReached' -or $names -notcontains 'completesAt') {
        [void]$v.Add('does not say whether it reached its declared completion point, so a menu-time reading and a match-time one render identically')
    } else {
        if ([string]$obj.completesAt -ne [string]$table.completesAt) {
            [void]$v.Add("declares completesAt '$([string]$obj.completesAt)' but the ledger declares '$([string]$table.completesAt)'")
        }
        if ($verdict -eq 'PASS' -and -not [bool]$obj.completionReached) {
            [void]$v.Add('says PASS without reaching its declared completion point: whatever it measured, it did not measure it where it said it would')
        }
    }
    return $v
}

function Read-JsonText([string]$text, [string]$what) {
    try { return ($text | ConvertFrom-Json) }
    catch { Write-Host "FAIL: $what is not parseable JSON: $($_.Exception.Message)"; return $null }
}

$ledgerdump = Join-Path $bin 'audio_ledgerdump.exe'
$probeSrc = Join-Path $PSScriptRoot 'probe_audio.cpp'
$ledgerSrc = Join-Path $PSScriptRoot 'checkledger.h'
$fixtureDir = Join-Path $PSScriptRoot 'fixtures'
$controls = @(
    @{ Name = 'launch13-done.json';         Why = "launch 13's own done.json, which reported checked=4 against a ceiling of 8 with four checks never asked" },
    @{ Name = 'provisional-f870d62.json';   Why = "f870d62's provisional record: PASS with seven checks forgotten" },
    @{ Name = 'provisional-forgotten.json'; Why = 'a provisional record with one check genuinely forgotten' },
    @{ Name = 'final-pending-reason.json';  Why = 'a final record still calling a check pending' }
)
$table = $null
$scratch = Join-Path $build 'audio_ledgercheck'

if (-not (Test-Path $probeSrc) -or -not (Test-Path $ledgerSrc)) {
    Write-Host "FAIL: the ledger arm needs probe_audio.cpp and checkledger.h; one of them is missing"
    $fail++
} elseif (@($controls | Where-Object { -not (Test-Path (Join-Path $fixtureDir $_.Name)) }).Count -gt 0) {
    Write-Host ("FAIL: the ledger arm's negative controls are missing from tests\audio\fixtures: " +
                (@($controls | Where-Object { -not (Test-Path (Join-Path $fixtureDir $_.Name)) } | ForEach-Object { $_.Name }) -join ', '))
    $fail++
} elseif (-not (Test-Path $ledgerdump)) {
    Write-Host "NOT RUN: $ledgerdump was not built, so the ledger could not be imported and no artifact was checked against it"
    $notRun++
} elseif (-not (Assert-Fresh $ledgerdump 'audio_ledgerdump.exe' -OnlyThese $ledgerdumpOnly)) {
    $fail++
} else {
    # A ledger the probe does not write through is a ledger the artifact will not carry: unit-green
    # cannot see wiring, so the caller is named here as well as the definition.
    $probeUses = Get-Content $probeSrc -Raw
    if (($probeUses -notmatch '#include\s+"checkledger\.h"') -or ($probeUses -notmatch 'ledger::FormatRecord\s*\(')) {
        Write-Host "FAIL: probe_audio.cpp does not include checkledger.h and write its record through ledger::FormatRecord, so the ledger this arm imports is not the one the artifact is written from"
        $fail++
    }

    $tableText = (& $ledgerdump | Out-String)
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: audio_ledgerdump exited $LASTEXITCODE"; $fail++ }
    else { $table = Read-JsonText $tableText 'audio_ledgerdump''s table' }
    if ($null -eq $table) { $fail++ }
    elseif ([int]$table.checkCeiling -le 0 -or @($table.checks).Count -ne [int]$table.checkCeiling -or
            @($table.finalWords).Count -ne 3 -or
            -not [string]$table.inProgressVerdict -or -not [string]$table.pendingReason -or
            -not [string]$table.forgottenReason -or -not [string]$table.completesAt) {
        Write-Host ("FAIL: audio_ledgerdump's table is incomplete (ceiling " + [int]$table.checkCeiling + ", " +
                    @($table.checks).Count + " names); a ledger check with no denominator of its own would accept whatever the artifact claims")
        $fail++
        $table = $null
    } elseif (@($table.checks | Sort-Object -Unique).Count -ne @($table.checks).Count) {
        Write-Host "FAIL: audio_ledgerdump names the same check twice, so an unasked entry could not be attributed"
        $fail++
        $table = $null
    }
}

# The three final words are the ones Verdict() itself returns, printed by the dump; arm 7 holds
# them against the words the harness tallies.
if ($table) {
    Write-Host ("ok: the scored-check ceiling is " + [int]$table.checkCeiling + ", imported from audio_ledgerdump.exe: " + (@($table.checks) -join ', '))

    # The declaration the harness reads and the constant the probe writes must be one value.
    if ($probe) {
        if ([string]$probe.completesAt -ne [string]$table.completesAt) {
            Write-Host ("FAIL: W2-H.probe.json declares completesAt '" + [string]$probe.completesAt + "' but checkledger.h writes '" + [string]$table.completesAt + "'; the harness would judge truncation against a point the probe does not wait for")
            $fail++
        } else {
            Write-Host ("ok: completesAt '" + [string]$table.completesAt + "' is declared to the harness and written by the probe alike")
        }
    }

    # The negative controls, and they run first: a checker that passes the records it was written
    # for is not a checker.
    foreach ($c in $controls) {
        $path = Join-Path $fixtureDir $c.Name
        $old = Read-JsonText (Get-Content $path -Raw) $c.Name
        if ($null -eq $old) { $fail++; continue }
        $caught = @(Get-LedgerViolations $old $table)
        if ($caught.Count -eq 0) {
            Write-Host ("FAIL: the ledger check passes fixtures\" + $c.Name + " (" + $c.Why + "). A check that cannot fail on the record it was written for is vacuous.")
            $fail++
        } else {
            Write-Host ("ok: the negative control fires - " + $c.Name + " is caught, " + $caught.Count + " violation(s): " + $caught[0])
        }
    }

    # The positive controls come from the writer itself, live, not from a fixture somebody typed.
    New-Item -ItemType Directory -Force -Path $scratch | Out-Null
    $samples = @{}
    foreach ($s in @('provisional', 'final-pass', 'final-truncated')) {
        $text = (& $ledgerdump --sample $s | Out-String)
        if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: audio_ledgerdump --sample $s exited $LASTEXITCODE"; $fail++; continue }
        $obj = Read-JsonText $text "audio_ledgerdump --sample $s"
        if ($null -eq $obj) { $fail++; continue }
        $samplePath = Join-Path $scratch "$s.json"
        [IO.File]::WriteAllText($samplePath, $text, (New-Object Text.UTF8Encoding $false))
        $samples[$s] = @{ Obj = $obj; Path = $samplePath }
        $bad = @(Get-LedgerViolations $obj $table)
        if ($bad.Count -gt 0) {
            Write-Host ("FAIL: the record FormatRecord writes for '" + $s + "' violates the ledger: " + ($bad -join '; '))
            $fail++
        }
    }
    if ($samples.ContainsKey('provisional')) {
        $p = $samples['provisional'].Obj
        $reasons = @($p.unasked | ForEach-Object { [string]$_.reason } | Sort-Object -Unique)
        Write-Host ("ok: the provisional record passes - verdict '" + [string]$p.verdict + "', " + [int]$p.notAsked +
                    " unreached check(s) each saying '" + ($reasons -join "' / '") + "'")
    }
    if ($samples.ContainsKey('final-truncated') -and [string]$samples['final-truncated'].Obj.verdict -ne 'NOT-RUN') {
        Write-Host ("FAIL: eight checks held with no match reached is '" + [string]$samples['final-truncated'].Obj.verdict + "', not NOT-RUN; a menu-time reading would file as the match-time one")
        $fail++
    }
    if ($samples.ContainsKey('final-pass') -and [string]$samples['final-pass'].Obj.verdict -ne 'PASS') {
        Write-Host ("FAIL: eight checks held at a live match is '" + [string]$samples['final-pass'].Obj.verdict + "', not PASS; the rule refuses the one state it exists to accept")
        $fail++
    }
    if ($samples.Count -eq 3) { Write-Host "ok: the writer's own final-pass and final-truncated records are PASS and NOT-RUN respectively" }

    # Live artifacts on this machine. A record that predates the ledger is history and is
    # reported rather than failed; one written by a binary that HAS the ledger and still breaks
    # the invariant is a defect now.
    $live = @()
    $runsDir = Join-Path $repo 'runs'
    if (Test-Path $runsDir) {
        $live = @(Get-ChildItem -Path $runsDir -Recurse -Filter 'done.json' -ErrorAction SilentlyContinue |
                  Where-Object { $_.DirectoryName -like '*probe_audio' })
    }
    $stale = 0
    foreach ($f in $live) {
        $o = $null
        try { $o = Get-Content $f.FullName -Raw | ConvertFrom-Json } catch { }
        if (-not $o) { continue }
        if (@($o.PSObject.Properties.Name) -notcontains 'checkCeiling') { $stale++; continue }
        $bad = @(Get-LedgerViolations $o $table)
        if ($bad.Count -gt 0) {
            Write-Host ("FAIL: " + $f.FullName + " carries the ledger and still violates it: " + ($bad -join '; '))
            $fail++
        }
    }
    Write-Host ("ok: " + $live.Count + " probe_audio done.json on this machine, " + $stale +
                " predating the ledger (reported, not failed - they are history), " +
                ($live.Count - $stale) + " checked against it")
}

Write-Host ''
Write-Host "audio: $fail failure(s), $notRun arm(s) not run"
if ($fail -ne 0) { exit 1 }
exit 0
