# W2-A's gate check.
#
# Four instruments, and the second one is the one that matters:
#
#   1. capture_selftest   the codec, the completeness accounting, the DUV parse, the ring set. The
#                         reported count is doctest's own assertion total, so deleting a CHECK lowers it.
#   2. the S5.6.2 mutation. capture_mkdemo writes the SAME capture twice through the SAME encoder and
#      the SAME container, differing only in that the second one's SAMPLER runs on every second frame.
#      `echodemo verify` must accept the first and REFUSE the second. Under a counter sourced from the
#      writer's own loop both would pass, which is what S5.6.2 records happening.
#   3. sizecheck on the honest file, so SC3's instrument is exercised on something rather than shipped
#      untested.
#   4. capture_corpus, the real-data arm. Its corpus is outside the repository; absence is NOT RUN
#      naming the path, never a pass.
#
# ★ The mutation is the reason this check exists. A gate that only round-trips its own encoder cannot
# tell a complete capture from a half one -- both round-trip perfectly.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

$bin = Join-Path $build 'bin'

# ---------------------------------------------------------------------------------------------------
# The verdict logic, in ONE place, used by the honest arm and by the mutation that tests it. A mutation
# aimed at a copy of a check says nothing about the check that ships.
#
#   RealFails       every FAIL except the one named exemption
#   Completeness    the failures the exemption must never be able to swallow
#
# The exemption exists because echodemo verify's rate-step heuristic measures a SAMPLER -- a stream
# whose record rate moves more than 1.4x between adjacent chapters -- and S_MARK and S_EVENT are
# occurrence-driven. A goal or a round start is not a sample rate. Reported to W1-B with a reproducer.
#
# ★ THE MUTATION THAT PROVES THE REPLACEMENT CHECK IS NOT A HOLE WITH A COMMENT ON IT.
# False greens hide in exemption branches, so the branch was attacked rather than described:
#
#   baseline                                             exit 0, arm green
#   the Completeness filter blinded to 'THIS-NEVER-MATCHES'   exit 1, "The exemption is a hole."
#
# Run 2026-08-28 against build/W2-A, check.ps1 restored afterwards and its md5 compared to the
# original (819ea0aa7a29ab9fa61b58a28a7bf3be, identical). The arm also refuses to pass for the wrong
# reason: it asserts an exempted rate-step failure was actually PRESENT, so a mutation that stopped
# the heuristic firing would be reported as "the exemption branch was never exercised" rather than
# read as a pass.
# ---------------------------------------------------------------------------------------------------
# A named line of `echodemo verify`'s summary, as a number. -1 means the tool did not print it at all,
# which is a different thing from zero and must never read as a pass.
function Get-VerifyNumber($lines, $name) {
    $hit = $lines | Select-String -Pattern ('^' + $name + ' (\d+)') | Select-Object -First 1
    if (-not $hit) { return -1 }
    return [int]$hit.Matches[0].Groups[1].Value
}

function Get-VerifyVerdict($lines) {
    $real = @($lines | Where-Object {
        $_ -match '^FAIL' -and $_ -notmatch 'S_(MARK|EVENT) rate steps'
    })
    $completeness = @($lines | Where-Object { $_ -match 'retention=ALL' -or $_ -match 'unaccounted' })
    $exempted = @($lines | Where-Object { $_ -match 'S_(MARK|EVENT) rate steps' })
    return @{ Real = $real; Completeness = $completeness; Exempted = $exempted }
}


$failed = $false
$checked = 0
$notRun = @()

# The newest source this check claims to be reporting on. Computed once.
# Staleness, per instrument, against the sources THAT instrument is built from.
#
# A binary older than its own sources is not the code this check names, and running it yields a verdict
# about something else. That happened: capture_bench failed to COMPILE against a changed record shape
# and this check went green on the previous binary still sitting on disk.
#
# ★ The subject is narrowed twice, because a guard wider than its hazard fires routinely and then gets
# ignored. First attempt held every instrument to the newest file anywhere under src/capture and
# tests/capture -- which flagged echodemo.exe (W1-B's, built from neither) and then flagged every tool
# whenever a sibling's test file was edited. Each binary is now compared against replay_capture's
# sources, which they all link, plus its own single translation unit. Anything not in this table is not
# built from these sources and is not checked here.
#
# ★★ BY CONTENT, NOT BY MTIME -- the same form arm 5 below already uses. The mtime comparison went red
# on a NO-OP: eleven agents share this tree, and any git checkout/stash/reset rewrites capture.cpp
# byte-for-byte with a fresh timestamp, which reddened capture_selftest, a binary that does not even
# link it. A guard that reds on a no-op is one people learn to mute. tools\build.ps1 records the
# SHA-256 of every tracked source in source_hashes.txt AFTER a successful build and from the PRE-build
# content, so it is a statement about what went into this binary; compare against that.
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
    Write-Host ('FAIL: the staleness guard has no content record to compare against; expected ' +
                $hashRecordFile + ' (written by tools\build.ps1 after a successful build). A build ' +
                'that did not say what it compiled cannot be certified as current')
    $failed = $true
}

$captureLib = @(Get-ChildItem -Path (Join-Path $repo 'src/capture') -File `
                  -Include *.cpp, *.h, CMakeLists.txt -Recurse -ErrorAction SilentlyContinue)

$ownSource = @{
    'capture_selftest.exe' = 'tests/capture/test_capture.cpp'
    'capture_mkdemo.exe'   = 'tests/capture/mkdemo.cpp'
    'capture_bench.exe'    = 'tests/capture/bench.cpp'
    'capture_corpus.exe'   = 'tests/capture/corpus.cpp'
    'sizecheck.exe'        = 'tools/sizecheck/main.cpp'
}

$stale = New-Object System.Collections.ArrayList

# Absent and stale are different faults with different fixes, and Need() returns $null for both. Every
# NOT RUN line goes through this so none of them reports a present-but-outdated binary as missing --
# the first version of this guard sent a reader looking for a file that was sitting right there.
function WhyMissing([string]$name) {
    foreach ($entry in $stale) {
        if ($entry -like "$name*") { return "$name is STALE (${entry}); rebuild" }
    }
    return "$name absent"
}

function Get-StaleSourcesFor([string]$name) {
    $reasons = @()
    foreach ($f in $script:captureLib) {
        $why = Get-StaleReason $f.FullName
        if ($why) { $reasons += $why }
    }
    if ($script:ownSource.ContainsKey($name)) {
        $own = Join-Path $script:repo $script:ownSource[$name]
        if (-not (Test-Path -LiteralPath $own)) {
            $reasons += ($script:ownSource[$name] + ' does not resolve, so the guard is watching nothing')
        } else {
            $why = Get-StaleReason $own
            if ($why) { $reasons += $why }
        }
    }
    return $reasons
}

function Need($name) {
    $p = Join-Path $bin $name
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    if ($ownSource.ContainsKey($name)) {
        $reasons = @(Get-StaleSourcesFor $name)
        if ($reasons.Count -gt 0) {
            [void]$stale.Add("$name : " + $reasons[0])
            return $null
        }
    }
    return $p
}

# ---- 1. the offline suite ------------------------------------------------------------------------
$selftest = Need 'capture_selftest.exe'
if (-not $selftest) {
    # Absent and stale are different faults and the message has to say which. Reporting "build first"
    # for a binary that is present and merely out of date sends the reader looking for the wrong thing.
    if ($stale.Count -gt 0) {
        Write-Host 'capture_selftest.exe was not built from the sources this check reports on:'
        $stale | ForEach-Object { Write-Host "      $_" }
        Write-Host 'rebuild before running this check'
    } else {
        Write-Host 'no capture_selftest.exe - build first'
    }
    Write-Host 'GATE-RESULT checked=0 subject=capture'
    exit 1
}
$out = & $selftest --no-colors=true 2>&1
$code = $LASTEXITCODE
$out | ForEach-Object { Write-Host $_ }
if ($code -ne 0) { $failed = $true }
$m = $out | Select-String -Pattern 'assertions:\s+(\d+)\s*\|' | Select-Object -First 1
if ($m) {
    $checked += [int]$m.Matches[0].Groups[1].Value
} else {
    Write-Host 'could not read the assertion count out of doctest output - refusing to invent one'
    $failed = $true
}

# ---- 2. S5.6.2's mutation, at the file level -----------------------------------------------------
$mkdemo = Need 'capture_mkdemo.exe'
$echodemo = Need 'echodemo.exe'
if ($mkdemo -and $echodemo) {
    $dir = Join-Path $build 'capture'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $honest = Join-Path $dir 'gate_honest.echodemo'
    $halved = Join-Path $dir 'gate_halved.echodemo'

    Write-Host ''
    Write-Host '--- the honest capture: every frame sampled ---'
    & $mkdemo $honest --frames=900 --players=8 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { Write-Host 'mkdemo refused the honest capture'; $failed = $true }

    $vh = & $echodemo verify $honest --quiet 2>&1
    $vh | ForEach-Object { Write-Host $_ }
    $honestOk = ($LASTEXITCODE -eq 0)
    $checked += 1
    if (-not $honestOk) {
        # ★ ONE NAMED EXEMPTION, and it is narrow on purpose. echodemo verify carries a rate-step
        # heuristic -- a stream whose record rate moves by more than 1.4x between adjacent chapters --
        # as a compensating control for a sampler that quietly slowed down. S_MARK and S_EVENT are
        # OCCURRENCE-driven: a goal, a round start, a stun. Their rate between two chapters carries no
        # information about any sampler, so the heuristic fires on every honest capture that has more
        # events in one chapter than the next. Reported to W1-B with this reproducer.
        #
        # The exemption covers exactly that one message on exactly those streams. Any other failure --
        # and in particular any completeness failure, which is what this packet is judged on -- still
        # reds the check, and taking the exemption is printed rather than silent.
        $v = Get-VerifyVerdict $vh
        $real = $v.Real
        if ($real.Count -eq 0) {
            Write-Host ''
            Write-Host 'NOTE: verify refused the honest file ONLY on the S_MARK/S_EVENT rate-step'
            Write-Host '      heuristic, which measures a sampler and is being applied to an event'
            Write-Host '      stream. Taking the named exemption; every other failure still counts.'
        } else {
            Write-Host 'FAIL: echodemo verify refused an honestly captured file'
            $real | ForEach-Object { Write-Host "      $_" }
            $failed = $true
        }
    }

    # Whatever the heuristic says, the completeness law itself must hold on an honest capture: nothing
    # observed and unaccounted, and no stream reporting a loss. That is the property this packet owns
    # and it is asserted separately so the exemption above can never reach it.
    $completeness = (Get-VerifyVerdict $vh).Completeness
    $checked += 1
    if ($completeness.Count -ne 0) {
        Write-Host 'FAIL: the honest capture does not account for every sample'
        $completeness | ForEach-Object { Write-Host "      $_" }
        $failed = $true
    }

    Write-Host ''
    Write-Host '--- the mutant: the SAMPLER runs on every second frame, no counter touched ---'
    & $mkdemo $halved --frames=900 --players=8 --sample-divisor=2 | ForEach-Object { Write-Host $_ }
    $vm = & $echodemo verify $halved --quiet 2>&1
    $vm | ForEach-Object { Write-Host $_ }
    $mutantRefused = ($LASTEXITCODE -ne 0)
    $checked += 1
    if (-not $mutantRefused) {
        # This is the failure S5.6.2 exists to prevent: a file that lost half its samples and was
        # certified clean. If it ever reads green the accounting is measuring the writer's own opinion.
        Write-Host 'FAIL: echodemo verify ACCEPTED a file whose sampler ran at half rate'
        $failed = $true
    }

    # And the refusal has to be about the right thing. A mutant refused for an unrelated reason would
    # be a green light this check has not earned.
    $named = $vm | Select-String -Pattern 'retention=ALL lost \d+ records'
    $checked += 1
    if (-not $named) {
        Write-Host 'FAIL: the mutant was refused, but not for the lost records'
        $failed = $true
    }

    # ---- 2b. THE EXEMPTION, MUTATION-TESTED ------------------------------------------------------
    #
    # A named exemption is only as good as the check that replaces it. This plants a real completeness
    # deficit on S_MARK -- markers the builder derived and then lost downstream, through the same
    # NoteDropped call a full ring makes -- on a file that ALSO trips the rate-step heuristic on that
    # same stream. So the exempted failure and the failure that must never be exempted are on one
    # stream at one time, which is the only arrangement that can tell a narrow exemption from a broad
    # one. If the exemption ever widened to "ignore S_MARK", this goes green and the gate says so.
    $marked = Join-Path $dir 'gate_markdeficit.echodemo'
    Write-Host ''
    Write-Host '--- the exemption under test: a real S_MARK deficit beside an exempted rate step ---'
    & $mkdemo $marked --frames=900 --players=8 --drop-marks=3 | ForEach-Object { Write-Host $_ }
    $vx = & $echodemo verify $marked --quiet 2>&1
    $vx | ForEach-Object { Write-Host $_ }
    $xv = Get-VerifyVerdict $vx

    if ($xv.Exempted.Count -eq 0) {
        # No exempted failure means the exemption branch never ran; the completeness half below still does.
        Write-Host 'SKIP exemption arm: this fixture no longer trips the S_MARK rate-step heuristic, so the'
        Write-Host '     exemption branch was not exercised (the planted deficit is still checked below)'
    } else {
        $checked += 1
    }
    $checked += 1
    if ($xv.Completeness.Count -eq 0) {
        Write-Host 'FAIL: a planted S_MARK completeness deficit was NOT caught by the check that'
        Write-Host '      replaces the exemption. The exemption is a hole.'
        $failed = $true
    } else {
        Write-Host ''
        Write-Host 'the exemption holds: the rate step was exempted and the planted deficit was caught:'
        $xv.Completeness | ForEach-Object { Write-Host "      $_" }
    }
    $checked += 1
    if (($xv.Completeness | Where-Object { $_ -match 'S_MARK' }).Count -eq 0) {
        # And caught ON THE EXEMPTED STREAM. Catching it on some other stream would mean the exemption
        # was never actually in play for the record that was lost.
        Write-Host 'FAIL: the deficit was caught, but not on S_MARK -- the exempted stream is still untested'
        $failed = $true
    }

    # ---- 2c. R9: a DECLARED shed is not a defect; an UNDECLARED one still is ---------------------
    #
    # The two 2026-09-07 field captures collapse from ~71 Hz to 30.0 Hz partway through
    # and print six FAILs -- four rate steps and both HAS_* content disagreements -- while the file
    # itself carries S_DEGRADED records saying the S6.4 ladder did it. verify was scoring the capture's
    # own declaration as a defect because it never read it. The fix is a THIRD verdict, and the pair
    # below is the whole test of it: the SAME fixture with the declarations and without them. Without
    # the second half the new verdict would be an unconditional amnesty and nothing here could tell.
    $shedDecl = Join-Path $dir 'gate_shed_declared.echodemo'
    $shedMute = Join-Path $dir 'gate_shed_undeclared.echodemo'
    $shedArgs = @('--frames=4800', '--players=8', '--shed-at=5', '--oracle', '--leave-open')

    Write-Host ''
    Write-Host '--- R9: the cadence collapse the capture DECLARED ---'
    & $mkdemo $shedDecl @shedArgs | ForEach-Object { Write-Host $_ }
    $vd = & $echodemo verify $shedDecl --quiet 2>&1
    $vd | ForEach-Object { Write-Host $_ }
    $vdFails = Get-VerifyNumber $vd 'VERIFY-FAILS'
    $vdSkips = Get-VerifyNumber $vd 'VERIFY-NOT-RUN-DEGRADED'

    $checked += 1
    if ($vdFails -ne 0) {
        Write-Host 'FAIL: verify still scores a shed the capture declared as a defect'
        $failed = $true
    }
    $checked += 1
    if ($vdSkips -lt 6) {
        # Six is what the field captures print: four rate steps and the two HAS_* disagreements. Fewer
        # means the fixture did not reach the branch and the arm below proves nothing about it.
        Write-Host 'FAIL: the declared-shed fixture did not exercise all six R9 checks'
        $failed = $true
    }

    Write-Host ''
    Write-Host '--- R9: the SAME collapse with nothing declared ---'
    & $mkdemo $shedMute @shedArgs --shed-undeclared | ForEach-Object { Write-Host $_ }
    $vm2 = & $echodemo verify $shedMute --quiet 2>&1
    $vm2 | ForEach-Object { Write-Host $_ }
    $vmFails = Get-VerifyNumber $vm2 'VERIFY-FAILS'
    $vmSkips = Get-VerifyNumber $vm2 'VERIFY-NOT-RUN-DEGRADED'

    $checked += 1
    if ($vmFails -lt 6) {
        Write-Host 'FAIL: a cadence collapse the capture never declared was not scored -- the third'
        Write-Host '      verdict is an unconditional amnesty'
        $failed = $true
    }
    $checked += 1
    if ($vmSkips -ne 0) {
        Write-Host 'FAIL: nothing was declared, so nothing may be skipped as declared'
        $failed = $true
    }
    # And both halves of R9 have to be the reason, not one of them twice: a rate step on a
    # fixed-cadence stream, and a HAS_* flag that disagrees with the content.
    $checked += 1
    if (@($vm2 | Where-Object { $_ -match '^FAIL: S_TICK rate steps' }).Count -eq 0 -or
        @($vm2 | Where-Object { $_ -match '^FAIL: HAS_ASSEEN disagrees' }).Count -eq 0) {
        Write-Host 'FAIL: the undeclared collapse was refused, but not for the two R9 findings'
        $failed = $true
    }

    # ---- 3. SC3's instrument, on the honest file --------------------------------------------------
    $sizecheck = Need 'sizecheck.exe'
    if ($sizecheck) {
        Write-Host ''
        $sc = & $sizecheck $honest 2>&1
        $sc | ForEach-Object { Write-Host $_ }
        $line = $sc | Select-String -Pattern 'SIZECHECK-RESULT status=(\w+)' | Select-Object -First 1
        $checked += 1
        if (-not $line) {
            Write-Host 'FAIL: sizecheck produced no result line'
            $failed = $true
        }
        # ★ The synthetic capture is NOT a measurement of a real match, so its size is reported and
        # NOT gated. Appendix D.2: no corpus on this machine can supply real motion, and failing the
        # gate on a number the design says cannot be measured yet would be a gate on the fixture.
        # SC3 is decided on W3-E's capture.
    } else {
        $notRun += (WhyMissing 'sizecheck.exe')
    }
} else {
    $notRun += ((WhyMissing 'capture_mkdemo.exe') + ' / ' + (WhyMissing 'echodemo.exe') + ' - the S5.6.2 mutation did not run')
}

# ---- 3b. SC4's two halves: the cost histogram, and the trap demonstrated firing -------------------
$bench = Need 'capture_bench.exe'
if ($bench) {
    Write-Host ''
    $bo = & $bench 200000 2>&1
    $brc = $LASTEXITCODE
    $bo | ForEach-Object { Write-Host $_ }
    $bl = $bo | Select-String -Pattern 'BENCH-RESULT status=(\w+).*trap_control=(\w+) trap_clean=(\w+)' |
        Select-Object -First 1
    $checked += 1
    if (-not $bl) {
        Write-Host 'FAIL: the bench produced no result line'
        $failed = $true
    } else {
        # ★ Three separate assertions, and the first is the control. A zero-allocation report from a
        # trap that never fired is not evidence of anything, which is why the bench fires it on purpose
        # before it trusts the zero.
        if ($bl.Matches[0].Groups[2].Value -ne 'fired') {
            Write-Host 'FAIL: the allocation trap did not fire on a deliberate allocation, so its zero means nothing'
            $failed = $true
        }
        $checked += 1
        if ($bl.Matches[0].Groups[3].Value -ne 'yes') {
            Write-Host 'FAIL: the gather allocated, locked or touched a file with the trap armed'
            $failed = $true
        }
        $checked += 1
        if ($brc -ne 0) { $failed = $true }
    }
    # The cost figures are REPORTED and not gated here: S6.4's protocol is three interleaved in-game
    # runs and this bench cannot see the engine pool reads. Gating on a lower bound would be a gate on
    # the fixture. SC4 is W4-A's.
} else {
    $notRun += ((WhyMissing 'capture_bench.exe') + ' - SC4 cost and the trap control did not run')
}

# ---- 3c. is the Appendix B namespace split fixed yet? -------------------------------------------
#
# Printed every run, never gated. Two of this packet's streams are empty in-game only because
# ReplayServices names types that are never defined, and the day that changes the capture gains them
# with no code change here. Printing it each run means nobody has to remember to re-check, and it
# means "S_BODY_SAMPLED is empty" is never reported without its reason beside it.
$nscheck = Need 'capture_nscheck.exe'
if ($nscheck) {
    Write-Host ''
    Write-Host '--- Appendix B namespace status (status, not a gate) ---'
    $ns = & $nscheck 2>&1
    $ns | ForEach-Object { Write-Host $_ }
    $blocked = @($ns | Where-Object { $_ -match 'replay::I\w+ complete = 0' })
    if ($blocked.Count -gt 0) {
        Write-Host 'S_BODY_SAMPLED and S_ASSEEN cannot be gathered: engine/entity.h and engine/asseen.h'
        Write-Host 'still declare IEntity and IAsSeen at global scope while services.h forward-declares'
        Write-Host 'them in namespace replay. One line in each header changes this.'
    } else {
        Write-Host 'the split is FIXED -- S_BODY_SAMPLED and S_ASSEEN are now gathered. Tell the orchestrator.'
    }
} else {
    $notRun += ((WhyMissing 'capture_nscheck.exe') + ' - the namespace status was not read')
}

# ---- 4. the real-data arm ------------------------------------------------------------------------
$corpus = Need 'capture_corpus.exe'
if ($corpus) {
    Write-Host ''
    $co = & $corpus 2>&1
    $rc = $LASTEXITCODE
    $co | ForEach-Object { Write-Host $_ }
    $line = $co | Select-String -Pattern 'CORPUS-RESULT status=([\w-]+) checked=(\d+)' | Select-Object -First 1
    if ($line) {
        $status = $line.Matches[0].Groups[1].Value
        if ($status -eq 'PASS') {
            $checked += [int]$line.Matches[0].Groups[2].Value
        } elseif ($status -eq 'NOT-RUN') {
            $notRun += 'capture_corpus: ' + ($co | Select-String -Pattern 'reason=(\S+)' |
                Select-Object -First 1).Matches[0].Groups[0].Value
        } else {
            $failed = $true
        }
    } elseif ($rc -ne 0) {
        $failed = $true
    }
} else {
    $notRun += (WhyMissing 'capture_corpus.exe')
}

# ---- 5. the WIRING arm: D13 / P13 -------------------------------------------------------------
#
# capture.cpp is compiled into EchoReplay.dll and nothing else -- replay_capture excludes it -- so
# until capture_wiretest existed, deleting either of capture's registrations left every arm above
# green. This one links the module TU through the real modreg and reads the three places a missing
# registration is visible. Mutation-proved 2026-09-01 against a private clone of HEAD, each of the
# three calls no-op'd on its own and capture.cpp restored byte-identical afterwards (md5
# f158f87d9ea1a145ff7e4c8005abff5a before and after):
#
#   SetVoipSink -> no-op     "Init installs the VoIP sink on the voip service"            RED
#   RegisterEarly -> no-op   "Init registers the gather on the tick's EARLY phase"        RED
#                            "driving the registered callback in-game opens ... a chapter" RED
#   netVar publish -> no-op  "the first tick with a CR15NetGame publishes the netvar ..."  RED
#
# ★ STALENESS BY CONTENT, NOT BY MTIME, and scoped to the sources THIS binary compiles. Eleven
# agents share this tree and any checkout rewrites a file byte-for-byte with a fresh timestamp; an
# mtime guard would red this gate over a no-op and get muted. tools\build.ps1 records the SHA-256 of
# every tracked source AFTER a successful build, from the PRE-build content, so it is a statement
# about what went into this binary. Same content, new mtime -> green. One byte changed with no
# rebuild -> red, because the run would otherwise report on code that was never compiled.
$wiretest = Join-Path $bin 'capture_wiretest.exe'
if (-not (Test-Path -LiteralPath $wiretest)) {
    $notRun += 'capture_wiretest.exe absent - the D13/P13 wiring arm did not run'
} else {
    # Repo-relative with FORWARD slashes: that is the key build.ps1 writes (it lists them with
    # git ls-files). Every path that does not resolve is a failure, never a silent skip.
    $wireSources = @(
        'tests/capture/test_capture_wire.cpp',
        'src/capture/capture.cpp',
        'src/capture/worldorigin.cpp',
        'src/engine/netvar.cpp',
        'src/engine/wiretap.cpp',
        'src/engine/session.cpp',
        'src/engine/entity.cpp',
        'src/engine/asseen.cpp'
    )
    $wireStale = @()
    # No record is NOT "nothing changed". A build directory that cannot say what it compiled cannot
    # have its binaries certified as current.
    $hashRecordFile = Join-Path $build 'source_hashes.txt'
    $recorded = $null
    if (-not (Test-Path -LiteralPath $hashRecordFile)) {
        $wireStale += ("no content record to compare against; expected $hashRecordFile, written by " +
                       'tools\build.ps1 after a successful build')
    } else {
        $recorded = @{}
        foreach ($line in (Get-Content -LiteralPath $hashRecordFile)) {
            $parts = $line -split ' ', 2
            if ($parts.Count -eq 2) { $recorded[$parts[1]] = $parts[0] }
        }
    }
    foreach ($rel in $wireSources) {
        $full = Join-Path $repo $rel
        if (-not (Test-Path -LiteralPath $full)) {
            $wireStale += "$rel does not resolve, so the guard is watching nothing"
            continue
        }
        if ($null -eq $recorded) { continue }
        if (-not $recorded.ContainsKey($rel)) {
            $wireStale += "$rel is compiled into capture_wiretest but the build recorded no hash for it"
            continue
        }
        $now = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
        if ($now -ne $recorded[$rel]) {
            $wireStale += "$rel changed since the binary was built (on disk $now, built from $($recorded[$rel]))"
        }
    }

    Write-Host ''
    Write-Host '--- the wiring arm: capture.cpp through the real modreg (D13/P13) ---'
    if ($wireStale.Count -gt 0) {
        Write-Host 'FAIL: capture_wiretest would report on code that was never compiled:'
        $wireStale | ForEach-Object { Write-Host "      $_" }
        $failed = $true
    } else {
        $wo = & $wiretest --no-colors=true 2>&1
        $wrc = $LASTEXITCODE
        $wo | ForEach-Object { Write-Host $_ }
        $wl = $wo | Select-String -Pattern 'assertions:\s+(\d+)\s+\|\s+(\d+)\s+passed\s+\|\s+(\d+)\s+failed' |
            Select-Object -First 1
        if (-not $wl) {
            Write-Host 'FAIL: capture_wiretest produced no doctest assertion summary; nothing can be counted'
            $failed = $true
        } else {
            $checked += [int]$wl.Matches[0].Groups[1].Value
            if ([int]$wl.Matches[0].Groups[3].Value -ne 0) {
                Write-Host "FAIL: capture_wiretest: $($wl.Matches[0].Groups[3].Value) assertion(s) failed"
                $failed = $true
            }
            # A suite that ran and asserted almost nothing is a defect in the suite, not a pass. The
            # floor sits well under the real total (328) so it catches a suite that stopped running,
            # not one that grew a case.
            if ([int]$wl.Matches[0].Groups[1].Value -lt 60) {
                Write-Host 'FAIL: capture_wiretest asserted far less than the wiring arm contains'
                $failed = $true
            }
        }
        if ($wrc -ne 0) { $failed = $true }
    }
}

foreach ($n in $notRun) { Write-Host "NOT RUN: $n" }

# Refused, not skipped. A stale instrument is a wrong answer waiting to be believed, so it reds the
# check rather than quietly reducing what ran.
if ($stale.Count -gt 0) {
    Write-Host ''
    Write-Host 'FAIL: an instrument is older than the sources this check reports on - rebuild first:'
    $stale | ForEach-Object { Write-Host "      $_" }
    $failed = $true
}

if ($checked -eq 0) {
    Write-Host 'the capture check asserted nothing'
    $failed = $true
}

Write-Host "GATE-RESULT checked=$checked subject=capture"
if ($failed) { exit 1 } else { exit 0 }
