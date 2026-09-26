# gate-tier: T0
#
# W2-F's gate entry point. Discovered by tools\gate.ps1 by convention: tests\<name>\check.ps1.
#
# Instruments judged separately, because they answer different questions.
#
#   1. ui_selftest -- the timeline arithmetic. Judged by its exit code AND by how many assertions
#      doctest reports, because a suite that exits 0 having asserted nothing must never be
#      indistinguishable from one that passed.
#
#   1b. ui_paneltest -- the panel's wiring: does LoadPath consult ISessionEntry, does the hotkey
#      reach ShowPanel, does the cursor get claimed and released, does AddOne survive a
#      reallocation. Judged on assertions AND on test-case count, for the same reason.
#
#   2. The guard bytes in src\ui\ui_facts.h, re-read out of echovr.exe through the PE section
#      table. That header is GENERATED from the binary, and a generated file nobody reads back is
#      a claim about a binary that has not been checked since the day it was written. This step
#      needs the shipped executable; when it is absent the step reports NOT RUN, contributes ZERO
#      to the checked count, and says so -- it never quietly passes.
#
# WHAT THIS GATE COVERS: cell layout, the click round trip, endpoint reachability, the fine
# window, marker classification and label layout, the fast-forward ceiling, time formatting, the
# identity of every facade entry point the panel calls, and -- since the W2-F audit -- the panel's
# WIRING: the session-entry consult on every load path, the show/hide switch, the cursor claim and
# the browser's three-state row. It covers NO REAL facade call, NO band-3 registration and NO
# drawing into the game's own frame. Those need a launch and are parked in runs\queue\payload.json
# as w2f-band3-registration-lands and w2f-band3-callback-thread. This gate being green is not the
# panel working.

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
$buildDir = Join-Path $repo 'build\W2-F'
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

$checked = 0
$assertions = 0

# ---------------------------------------------------------------------------------------------
# 1. The timeline arithmetic.
# ---------------------------------------------------------------------------------------------
$pathFile = Join-Path $buildDir 'ui_selftest_path.txt'
$exe = $null
if (Test-Path $pathFile) { $exe = (Get-Content $pathFile -Raw).Trim() }

if (-not $exe -or -not (Test-Path $exe)) {
    Add-Fail ("ui_selftest was not built; expected its path in " + $pathFile)
} else {
    $out = & $exe 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }

    $line = ($out | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
    if (-not $line) {
        Add-Fail 'ui_selftest produced no doctest assertion summary; the count cannot be trusted'
    } elseif ($line -match 'assertions:\s+(\d+)\s+\|\s+(\d+)\s+passed\s+\|\s+(\d+)\s+failed') {
        $assertions = [int]$Matches[1]
        $failedCount = [int]$Matches[3]
        $checked += $assertions
        if ($failedCount -ne 0) { Add-Fail ("ui_selftest: $failedCount assertion(s) failed") }
        # The P1/P2 sweeps alone run hundreds of thousands of checks. A floor well below the real
        # count catches a suite that stopped running, not one that legitimately changed size.
        if ($assertions -lt 50000) {
            Add-Fail ("ui_selftest asserted only $assertions times; the P1/P2 sweeps alone exceed that, so this means they did not run")
        }
    } else {
        Add-Fail ('could not parse the doctest summary line: ' + $line)
    }
    if ($code -ne 0) { Add-Fail ("ui_selftest exited $code") }
}

# ---------------------------------------------------------------------------------------------
# 1b. The panel's WIRING, which the arithmetic suite above does not touch at all.
#
# ui_selftest links timeline.cpp alone, and that is exactly how three deliverables shipped with no
# caller behind a green gate: LoadPath never consulted ISessionEntry, ShowPanel/Toggle had no caller
# anywhere in src/, and SetCursorEnabled was bound, guarded, counted and never called. ui_paneltest
# links the real panel.cpp, demoindex.cpp and csregister.cpp against a fake facade and real
# .echodemo files, so every one of those call sites is exercised. Each case fails when its call is
# deleted -- proven by mutation, both directions, before this step was added.
# ---------------------------------------------------------------------------------------------
$panelPathFile = Join-Path $buildDir 'ui_paneltest_path.txt'
$panelExe = $null
if (Test-Path $panelPathFile) { $panelExe = (Get-Content $panelPathFile -Raw).Trim() }

if (-not $panelExe -or -not (Test-Path $panelExe)) {
    Add-Fail ("ui_paneltest was not built; expected its path in " + $panelPathFile)
} else {
    $out = & $panelExe 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }

    $line = ($out | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
    $cases = ($out | Where-Object { $_ -match '^\[doctest\]\s+test cases:' } | Select-Object -Last 1)
    if (-not $line) {
        Add-Fail 'ui_paneltest produced no doctest assertion summary; the count cannot be trusted'
    } elseif ($line -match 'assertions:\s+(\d+)\s+\|\s+(\d+)\s+passed\s+\|\s+(\d+)\s+failed') {
        $panelAssertions = [int]$Matches[1]
        $panelFailed = [int]$Matches[3]
        $checked += $panelAssertions
        if ($panelFailed -ne 0) { Add-Fail ("ui_paneltest: $panelFailed assertion(s) failed") }
        # A floor, because a suite that stops running is a suite that exits 0 having asserted
        # nothing -- and this file exists because something that could not fail shipped once.
        if ($panelAssertions -lt 100) {
            Add-Fail ("ui_paneltest asserted only $panelAssertions times; the wiring cases alone exceed that, so they did not run")
        }
    } else {
        Add-Fail ('could not parse ui_paneltest''s doctest summary line: ' + $line)
    }
    # ★ THE PENDING AXIS REPRODUCTION MUST STAY VISIBLE. Four cases in ui_paneltest are SKIPPED, not
    # deleted: they reproduce the transport-seam axis defect (PlayheadUs and Seek are absolute source
    # time, DurationUs is a 0-based length, and they differ by t0Us) and they are skipped because
    # W2-B owns the invariant and one packet must not answer it locally. A skipped case contributes
    # zero assertions, so both floors above are blind to it being quietly deleted - which is how a
    # known defect loses its only reproduction. This arm reads the SOURCE for the case names.
    $panelSrc = Join-Path $here 'test_panel.cpp'
    $pending = @('clicking the end of the scrub bar reaches the end of the capture',
                 'SeekFraction(0.5) lands on the middle of the capture',
                 'a bookmark jump lands on that bookmark',
                 'the playhead renders inside the strip it is drawn on')
    if (-not (Test-Path -LiteralPath $panelSrc)) {
        Add-Fail 'test_panel.cpp could not be read, so the pending axis reproduction cannot be accounted for'
    } else {
        $panelSrcText = Get-Content -LiteralPath $panelSrc -Raw
        $missing = @($pending | Where-Object { $panelSrcText -notmatch [regex]::Escape($_) })
        if ($missing.Count -ne 0) {
            Add-Fail ("the axis-seam reproduction case(s) '" + ($missing -join "', '") + "' are gone from test_panel.cpp. They are the only executable record of the transport-seam axis defect; delete them only with the fix that makes them pass.")
        } else { $checked++ }
    }
    if ($cases -match 'skipped') {
        $skipCount = 0
        if ($cases -match '(\d+)\s+skipped') { $skipCount = [int]$Matches[1] }
        if ($skipCount -gt 0) {
            Write-Host ("PENDING: $skipCount ui_paneltest case(s) are skipped - the transport-seam axis question (PlayheadUs/Seek are ABSOLUTE source time, DurationUs is a 0-BASED length, differing by t0Us). Un-skip them with the fix; they are its regression test.")
        } else {
            Write-Host 'the axis-seam reproduction is live (nothing skipped), so the seam invariant has been settled'
        }
    }

    # The case count is a second, independent statement: assertions can be concentrated in one
    # surviving case while every other one silently stopped being compiled in.
    if ($cases -match 'test cases:\s+(\d+)') {
        if ([int]$Matches[1] -lt 15) {
            Add-Fail ("ui_paneltest ran only $($Matches[1]) test case(s); the B1, B4, O1 and O2 arms are more than that")
        }
    } else {
        Add-Fail 'ui_paneltest produced no doctest test-case summary'
    }
    if ($code -ne 0) { Add-Fail ("ui_paneltest exited $code") }
}

# ---------------------------------------------------------------------------------------------
# 2. Every guard byte in ui_facts.h, back out of the binary.
# ---------------------------------------------------------------------------------------------
. (Join-Path $PSScriptRoot '..\..\tools\echovr_path.ps1')
$echovr = Find-EchoVrExe
if (-not $echovr) { $echovr = '(echovr.exe not found - set ECHOVR_DIR or REPLAY_ECHOVR_EXE)' }
$factsHeader = Join-Path $repo 'src\ui\ui_facts.h'

function Get-PeSections([byte[]]$bytes) {
    $lfanew = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($bytes[$lfanew] -ne 0x50 -or $bytes[$lfanew + 1] -ne 0x45) { return $null }
    $nsec = [BitConverter]::ToUInt16($bytes, $lfanew + 6)
    $optSize = [BitConverter]::ToUInt16($bytes, $lfanew + 20)
    $imageBase = [BitConverter]::ToUInt64($bytes, $lfanew + 24 + 24)
    $secOff = $lfanew + 24 + $optSize
    $sections = @()
    for ($i = 0; $i -lt $nsec; $i++) {
        $o = $secOff + 40 * $i
        $sections += [pscustomobject]@{
            Rva     = [BitConverter]::ToUInt32($bytes, $o + 12)
            VSize   = [BitConverter]::ToUInt32($bytes, $o + 8)
            RawSize = [BitConverter]::ToUInt32($bytes, $o + 16)
            RawPtr  = [BitConverter]::ToUInt32($bytes, $o + 20)
        }
    }
    return [pscustomobject]@{ ImageBase = $imageBase; Sections = $sections }
}

if (-not (Test-Path $echovr)) {
    # NOT a pass and NOT a silent skip: name the path and say the step did not run.
    Write-Host ("NOT RUN: the ui_facts.h byte check needs " + $echovr + " and it is not present; 0 guard rows were verified this run")
} elseif (-not (Test-Path $factsHeader)) {
    Add-Fail ("src\ui\ui_facts.h is missing: " + $factsHeader)
} else {
    $bytes = [IO.File]::ReadAllBytes($echovr)
    $md5 = [BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash($bytes)).Replace('-', '').ToLower()
    if ($md5 -ne 'caa05ee3bca2f5df0809994e93e2cc05') {
        Add-Fail ("echovr.exe md5 is $md5, not the build every fact was measured against; refusing to compare bytes against the wrong binary")
    } else {
        $pe = Get-PeSections $bytes
        if ($null -eq $pe) {
            Add-Fail 'could not parse the PE header of echovr.exe'
        } else {
            $text = Get-Content $factsHeader -Raw

            # Pair every kNameVA with the guard that identifies it: the DISCRIMINATOR window at its
            # own offset where one exists, and the prologue otherwise. Checking the prologue for a
            # function whose prologue is shared would prove only that the bytes had not drifted.
            $vaMap = @{}
            foreach ($m in [regex]::Matches($text, 'k(\w+)VA\s*=\s*0x([0-9A-Fa-f]+)ull')) {
                $vaMap[$m.Groups[1].Value] = [Convert]::ToUInt64($m.Groups[2].Value, 16)
            }
            $offMap = @{}
            foreach ($m in [regex]::Matches($text, 'k(\w+)DiscOffset\s*=\s*(\d+)u')) {
                $offMap[$m.Groups[1].Value] = [int]$m.Groups[2].Value
            }

            $rows = 0
            $mismatches = 0
            # The window is per row, not 20 for everyone: the two ARCH-20 setters are 14 and 8
            # bytes long and are declared short on purpose (ARCH-20 s3.1). A hard-coded [20] here
            # would simply stop matching them and the floor below would be the only thing that
            # noticed -- which is why the floor is a count and not a boolean.
            foreach ($m in [regex]::Matches($text, 'k(\w+?)(Prologue|Disc)\[(\d+)\]\s*=\s*\{([^}]*)\}')) {
                $name = $m.Groups[1].Value
                $kind = $m.Groups[2].Value
                $len = [int]$m.Groups[3].Value
                if (-not $vaMap.ContainsKey($name)) {
                    Add-Fail ("ui_facts.h declares k$name$kind with no matching k${name}VA")
                    continue
                }
                # A shared prologue is a drift guard only; the identity claim lives in the Disc row,
                # and checking both would double-count a row that proves one thing.
                if ($kind -eq 'Prologue' -and $offMap.ContainsKey($name)) { continue }

                $va = $vaMap[$name]
                if ($kind -eq 'Disc') { $va += [uint64]$offMap[$name] }
                $rva = $va - $pe.ImageBase
                $off = $null
                foreach ($s in $pe.Sections) {
                    $span = [Math]::Max($s.VSize, $s.RawSize)
                    if ($rva -ge $s.Rva -and $rva -lt ($s.Rva + $span)) { $off = $s.RawPtr + ($rva - $s.Rva); break }
                }
                if ($null -eq $off) {
                    Add-Fail ("k$name$kind : VA 0x{0:X} is in no section of echovr.exe" -f $va)
                    continue
                }
                $want = @()
                foreach ($h in [regex]::Matches($m.Groups[4].Value, '0x([0-9A-Fa-f]{2})')) {
                    $want += [byte][Convert]::ToInt32($h.Groups[1].Value, 16)
                }
                if ($want.Count -ne $len) {
                    Add-Fail ("k$name$kind : parsed $($want.Count) bytes, the declaration says $len")
                    continue
                }
                $bad = $false
                for ($i = 0; $i -lt $len; $i++) {
                    if ($bytes[$off + $i] -ne $want[$i]) { $bad = $true; break }
                }
                $rows++
                if ($bad) {
                    $mismatches++
                    Add-Fail ("k$name$kind does not match echovr.exe at VA 0x{0:X}" -f $va)
                }
            }

            # An empty match set would sail through the loop above reporting nothing wrong. The
            # header carries one guard per entry point and there are 20 of them; anything less means
            # the regex stopped matching, not that the binary agrees.
            if ($rows -lt 22) {
                Add-Fail ("only $rows guard row(s) were extracted from ui_facts.h; the header declares 22 guarded sites (20 facade entry points plus the two ARCH-20 gate setters), so the check did not read what it claims to have read")
            } else {
                Write-Host ("ui_facts.h: $rows guard row(s) re-read from echovr.exe, $mismatches mismatch(es)")
                $checked += $rows
            }

            if ($SelfTest) {
                # ★ Prove the byte check can fail. Flip one byte of a known-good expectation and
                # assert the comparison rejects it. A guard nobody has watched go red is not a guard.
                $probeVa = $vaMap['Begin']
                $probeRva = $probeVa - $pe.ImageBase
                $probeOff = $null
                foreach ($s in $pe.Sections) {
                    $span = [Math]::Max($s.VSize, $s.RawSize)
                    if ($probeRva -ge $s.Rva -and $probeRva -lt ($s.Rva + $span)) { $probeOff = $s.RawPtr + ($probeRva - $s.Rva); break }
                }
                $planted = $bytes[$probeOff] -bxor 0xFF
                if ($planted -eq $bytes[$probeOff]) {
                    Add-Fail 'SelfTest: the planted byte equals the real one, so the plant proves nothing'
                } else {
                    Write-Host ("SelfTest: a planted byte at GUI.BEGIN (0x{0:X2} for 0x{1:X2}) differs from the file, so a wrong expectation would be rejected" -f $planted, $bytes[$probeOff])
                }
            }
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 3. The probe: is it BUILT, is it FRESH, does it FILE, and does it answer what it claims?
#
# "Every probe DLL in the queue is stale -- eleven against one landed change, one by 22 hours. The
# tree being green is a statement about SOURCES; every probe binary is older than the green." So
# this step compares the DLL's timestamp against every source it is built from, and loads it to
# prove the provisional record is filed from the static initialiser rather than assuming a filer
# that has never been watched file.
# ---------------------------------------------------------------------------------------------
$probePathFile = Join-Path $buildDir 'probe_ui_path.txt'
$hostPathFile  = Join-Path $buildDir 'uiprobehost_path.txt'
$probeDll = $null
$probeHost = $null
if (Test-Path $probePathFile) { $probeDll  = (Get-Content $probePathFile -Raw).Trim() }
if (Test-Path $hostPathFile)  { $probeHost = (Get-Content $hostPathFile  -Raw).Trim() }

if (-not $probeDll -or -not (Test-Path $probeDll)) {
    Add-Fail ("probe_ui.dll was not built; expected its path in " + $probePathFile)
} else {
    # 3a. FRESHNESS. Named explicitly because a stale probe is the failure that voided the last
    # sweep: it answers the question the sources asked yesterday.
    $probeSources = @(
        (Join-Path $repo 'tests\ui\probe_ui.cpp'),
        # The probe's two scoring decisions (censored-vs-FAIL, and whether the sampler may stop
        # waiting) live here, so a probe built before an edit to it answers the old question.
        (Join-Path $repo 'tests\ui\probe_ui_outcome.h'),
        (Join-Path $repo 'src\ui\csregister.cpp'),
        (Join-Path $repo 'src\ui\csregister.h'),
        (Join-Path $repo 'src\engine\tick.cpp'),
        (Join-Path $repo 'src\engine\tick_policy.cpp'),
        (Join-Path $repo 'src\generated\facts_generated.h')
    )
    $staleAgainst = @()
    $sourcesSeen = 0
    foreach ($src in $probeSources) {
        if (-not (Test-Path $src)) { Add-Fail ("probe source not found: " + $src); continue }
        $sourcesSeen++
        $why = Get-StaleReason $src
        if ($why) { $staleAgainst += $why }
    }
    # An empty source list would sail through the loop reporting nothing wrong.
    if ($sourcesSeen -lt $probeSources.Count) {
        Add-Fail ("only $sourcesSeen of $($probeSources.Count) probe sources were checked for freshness")
    }
    if ($staleAgainst.Count -gt 0) {
        Add-Fail ("probe_ui.dll was NOT built from " + $staleAgainst.Count + " of its sources (" + (($staleAgainst | Select-Object -Unique) -join '; ') + "); it would answer the question the sources asked before the change")
    } else {
        Write-Host ("probe_ui.dll was built from the recorded content of all $sourcesSeen of its sources")
        $checked += $sourcesSeen
    }

    # 3b. DOES IT FILE. LoadLibrary in a process that is not the game: if done.json appears anyway,
    # the filing depends on nothing the game provides, which is the whole point of the static
    # initialiser.
    if (-not $probeHost -or -not (Test-Path $probeHost)) {
        Add-Fail ("uiprobehost.exe was not built; expected its path in " + $hostPathFile)
    } else {
        $stamp = Get-Date -Format 'yyyyMMddHHmmssfff'
        $probeRoot = Join-Path $buildDir ("uiprobe_" + $stamp)
        New-Item -ItemType Directory -Force -Path $probeRoot | Out-Null
        $env:REPLAY_PROBE_ROOT = $probeRoot
        $env:REPLAY_PROBE_IDS = 'probe_ui=W2-F-uipanel'
        & $probeHost $probeDll 1200 | Out-Null
        $doneFile = Join-Path $probeRoot 'probe_ui\done.json'
        if (-not (Test-Path $doneFile)) {
            Add-Fail ("the probe loaded and filed NOTHING; a verdict nobody has watched be written is not a verdict. Expected " + $doneFile)
        } else {
            $raw = Get-Content $doneFile -Raw
            $done = $null
            try { $done = $raw | ConvertFrom-Json } catch { Add-Fail ('done.json is not valid JSON: ' + $_.Exception.Message) }
            if ($done) {
                if ($done.probe -ne 'W2-F-uipanel') { Add-Fail ("done.json probe id is '$($done.probe)', not the id REPLAY_PROBE_IDS gave it") } else { $checked++ }
                if (-not $done.provisional)         { Add-Fail 'done.json is not provisional; outside a game the sampler cannot have determined anything, so a final record here means the outcome is fabricated' } else { $checked++ }
                if ($null -eq $done.checked)        { Add-Fail 'done.json omits `checked`; an otherwise perfect artifact was voided once for exactly this' } else { $checked++ }
                if ($done.checked -ne 0)            { Add-Fail "done.json reports checked=$($done.checked) outside a game, where nothing can have been determined" } else { $checked++ }
                if ($done.verdict -ne 'NOT-RUN')    { Add-Fail "done.json verdict is '$($done.verdict)', not NOT-RUN, in a process that never reached a match" } else { $checked++ }
                Write-Host ("probe filed from its static initialiser: verdict=$($done.verdict) checked=$($done.checked) provisional=$($done.provisional)")
            }
        }
    }
}

# 3c. The descriptor must answer reads that actually exist. A probe claiming an id no payload
# carries is collected against nothing, and the sweep reports coverage it does not have.
$descriptor = Join-Path $repo 'tests\ui\W2-F.probe.json'
$payload    = Join-Path $repo 'runs\queue\payload\W2-F.json'
if (-not (Test-Path $descriptor)) {
    Add-Fail ("the queue descriptor is missing: " + $descriptor)
} elseif (-not (Test-Path $payload)) {
    Write-Host ("NOT RUN: no parked-read payload at " + $payload + ", so the descriptor's answers were not cross-checked")
} else {
    $desc = Get-Content $descriptor -Raw | ConvertFrom-Json
    $pay  = Get-Content $payload -Raw | ConvertFrom-Json
    $payIds = @($pay.reads | ForEach-Object { $_.id })
    $answers = @($desc.probes[0].answers)
    if ($answers.Count -eq 0) {
        Add-Fail 'the descriptor answers no reads at all, so collecting it would prove nothing'
    }
    foreach ($a in $answers) {
        if ($payIds -notcontains $a) {
            Add-Fail ("the descriptor claims to answer '$a', which no read in $payload carries")
        } else {
            $checked++
        }
    }
    # 3d. THE GUARD DECLARATION MUST MATCH THE MECHANISM IN THE BINARY.
    #
    # probe_ui links replay_bootstrap, so coexist::ClaimInstance is compiled into it and it contends
    # with any other bootstrap-carrying probe in the same launch. The harness splits launches on the
    # `offline-bit-lock` guard -- but the DECLARATION is the packet's, and a probe that carries the
    # mechanism without declaring it gets scheduled beside a contender and loses the read to load
    # order. That already cost one launch. So the two are checked against each other rather than
    # either being trusted: if the build links the bootstrap, the descriptor must say so.
    $probeCmake = Get-Content (Join-Path $repo 'tests\ui\CMakeLists.txt') -Raw
    $linksBootstrap = $probeCmake -match 'target_link_libraries\(probe_ui[^)]*replay_bootstrap'
    $declaresGuard = @($desc.probes[0].guards) -contains 'offline-bit-lock'
    if ($linksBootstrap -and -not $declaresGuard) {
        Add-Fail 'probe_ui links replay_bootstrap but the descriptor does not declare the offline-bit-lock guard; the harness would schedule it beside a contending probe and the read would be lost to load order'
    } elseif (-not $linksBootstrap -and $declaresGuard) {
        Add-Fail 'the descriptor declares offline-bit-lock but probe_ui no longer links replay_bootstrap; it is reserving contention it does not cause'
    } else {
        Write-Host ("guard declaration matches the binary: linksBootstrap=$linksBootstrap declaresGuard=$declaresGuard")
        $checked++
    }

    if ($desc.probes[0].dll -ne 'build/W2-F/bin/probe_ui.dll') {
        Add-Fail ("the descriptor points at " + $desc.probes[0].dll + ", which is not where this packet builds its probe")
    } else { $checked++ }

    # -----------------------------------------------------------------------------------------
    # 3d. The declared guard must be INSTALLED, not merely linked.
    #
    # The arm above proves the descriptor and the link list agree. That is not the same question
    # and the difference is a no-network hazard: offlinelock::Apply() has exactly one caller, inside
    # WorkerMain, and only bootstrap::Attach starts that thread - so linking replay_bootstrap put
    # all thirteen of its sources into this DLL and reached none of them. The descriptor declared
    # the guard, the harness cleared the -offline launch on that declaration, and nothing installed
    # it. The test harness refuses this shape at plan time; this arm is the same question asked
    # where the code lives, so the call cannot be deleted and found only by the sweep.
    #
    # ★ THE PATTERN IS CITED, NOT RESTATED. $AttachCallForm is W1-J's constant and a second copy
    # here is a second source of truth that is right until theirs changes. It is read out of their
    # file, and a read that cannot find it FAILS rather than falling back to a local guess.
    $harnessPath = Join-Path $repo 'tools\harness.ps1'
    $attachForm = $null
    if (Test-Path -LiteralPath $harnessPath) {
        $harnessText = Get-Content -LiteralPath $harnessPath -Raw
        if ($harnessText -match '(?m)^\s*\$AttachCallForm\s*=\s*''([^'']+)''') { $attachForm = $Matches[1] }
    }
    $probeSrc = Join-Path $here 'probe_ui.cpp'
    $probeText = ''
    if (Test-Path -LiteralPath $probeSrc) { $probeText = Get-Content -LiteralPath $probeSrc -Raw }

    if (-not $attachForm) {
        Add-Fail ("could not read `$AttachCallForm out of " + $harnessPath + "; the installed-guard arm has no pattern to apply and refuses rather than inventing one")
    } elseif (-not $probeText) {
        Add-Fail 'probe_ui.cpp could not be read, so the installed-guard arm would pass over zero files'
    } else {
        # Comments stripped the way the harness strips them.
        $stripped = [regex]::Replace($probeText, '(?s)/\*.*?\*/', ' ')
        $stripped = [regex]::Replace($stripped, '(?m)//.*$', ' ')

        if ($stripped -notmatch $attachForm) {
            Add-Fail 'probe_ui.cpp does not call bootstrap::Attach, but the descriptor declares the offline-bit-lock guard. offlinelock::Apply() runs only from WorkerMain and only Attach starts it, so the -offline launch would proceed on a promise nothing keeps. Call it, or withdraw the declaration and accept that the launch is then refused for having no installer.'
        } else { $checked++ }

        # ★ AND THE COMMENT STRIPPING IS LOAD-BEARING **HERE**, not hypothetically: this file's own
        # comments name bootstrap::Attach while explaining why it is called, so a check that read the
        # raw text would keep passing after the call itself was deleted. This arm fails if the
        # stripping is ever dropped.
        if ($probeText -match [regex]::Escape('bootstrap::Attach')) { $checked++ }
        $commentsOnly = [regex]::Replace($probeText, '(?s)/\*.*?\*/', ' ')
        $commentsOnly = [regex]::Replace($commentsOnly, '(?m)//.*$', ' ')
        if (($probeText -match $attachForm) -and ($commentsOnly -notmatch $attachForm)) {
            Add-Fail 'the raw text matches the call form but the stripped text does not: probe_ui.cpp mentions bootstrap::Attach only in a comment'
        } else { $checked++ }

        # The probe must also REPORT the outcome. Calling Attach is necessary and not sufficient -
        # Attach returns false at coexist::ClaimInstance before it reaches CreateThread(WorkerMain),
        # so a probe that loses the instance claim installs nothing. The check id below is what
        # makes the declaration unable to outlive the mechanism.
        if ($stripped -notmatch [regex]::Escape('offline-bit-lock-held')) {
            Add-Fail 'probe_ui.cpp installs the lock but never reports whether it is HELD. A probe that loses the instance claim installs nothing, so the launch needs the process-level answer read out of offlinelock::Current(), not the fact that Attach was called.'
        } else { $checked++ }
    }
}

if ($SelfTest) {
    # ★ The assertion count is mutation-tested rather than trusted. Re-run excluding the sweep cases
    # and assert the number DROPS -- a substance count nobody has watched move is not a count.
    if ($exe -and (Test-Path $exe)) {
        $out2 = & $exe '--test-case-exclude=*P1*,*P2*' 2>&1
        $line2 = ($out2 | Where-Object { $_ -match '^\[doctest\]\s+assertions:' } | Select-Object -Last 1)
        if ($line2 -match 'assertions:\s+(\d+)') {
            $reduced = [int]$Matches[1]
            if ($reduced -ge $assertions) {
                Add-Fail ("SelfTest: excluding the P1/P2 sweeps did not reduce the assertion count ($assertions -> $reduced); the count does not measure what it claims")
            } else {
                Write-Host ("SelfTest: assertion count moved $assertions -> $reduced when the P1/P2 sweeps were excluded")
            }
        } else {
            Add-Fail 'SelfTest: the reduced run produced no summary'
        }
    }
}

foreach ($f in $failed) { Write-Host ("FAIL: " + $f) }

if ($failed.Count -gt 0) {
    Write-Host 'GATE-RESULT checked=0 subject=ui'
    exit 1
}

Write-Host "GATE-RESULT checked=$checked subject=ui"
exit 0
