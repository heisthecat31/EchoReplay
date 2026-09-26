# W1-M's gate check. Two instruments, one verdict:
#
#   sitecheck.py    the byte windows probe_uidag.cpp carries, against the real echovr.exe, and the
#                   offsets it decodes out of them
#   uidag_probehost the offline reproduction of the condition that has silently eaten probe verdicts on
#                   this project: another module claims the coexist instance first. The real probe must
#                   still file, and must file a PROVISIONAL record before it measures anything; the
#                   MUTANT -- the same source with the filing moved back into a bootstrap module's
#                   Init -- must not file at all.
#
# The reported count is the sum of what the two actually asserted, not a constant.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

$failed = $false
$checked = 0
function Fail([string]$why) { Write-Host "  FAIL $why"; $script:failed = $true }

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

# ---------------------------------------------------------------------------------------------------
# 1. The site tables against the shipped binary.
# ---------------------------------------------------------------------------------------------------
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { Write-Host 'python is not on PATH; sitecheck.py cannot run'; exit 1 }

$siteOut = & $py.Source (Join-Path $PSScriptRoot 'sitecheck.py') 2>&1
$siteExit = $LASTEXITCODE
$siteOut | ForEach-Object { Write-Host "  $_" }
if ($siteExit -ne 0) { $failed = $true }

$siteLine = $siteOut | Where-Object { $_ -match '^SITECHECK-CHECKED\s+(\d+)' } | Select-Object -First 1
if (-not $siteLine -or $siteLine -notmatch '^SITECHECK-CHECKED\s+(\d+)') {
    Write-Host 'sitecheck.py did not report what it checked; a check whose count cannot be read is not a check'
    exit 1
}
$siteChecked = [int]$matches[1]
$siteSkipped = @($siteOut | Where-Object { "$_" -match '^SKIP ' }).Count -gt 0
if ($siteChecked -eq 0 -and -not $siteSkipped) { Write-Host 'sitecheck.py asserted nothing'; exit 1 }
$checked += $siteChecked
Write-Host "  sitecheck asserted $siteChecked"

# ---------------------------------------------------------------------------------------------------
# 2. Staleness. A probe DLL older than its source is the wrong probe, and it fails in the most
#    expensive place there is - on a launch. Refuse here instead.
# ---------------------------------------------------------------------------------------------------
$bin = Join-Path $build 'bin'
$host_ = Join-Path $bin 'uidag_probehost.exe'
$probe = Join-Path $bin 'probe_uidag.dll'
$mutant = Join-Path $bin 'probe_uidag_mutant.dll'
$claimer = Join-Path $bin 'uidag_claimer.dll'
foreach ($needed in @($host_, $probe, $mutant, $claimer)) {
    if (-not (Test-Path -LiteralPath $needed)) { Write-Host "no $needed - build first"; exit 1 }
}
$src = Get-Item -LiteralPath (Join-Path $repo 'tests\uidag\probe_uidag.cpp')
$srcStale = Get-StaleReason $src.FullName
foreach ($built in @($probe, $mutant)) {
    $checked += 1
    if ($srcStale) {
        Fail ("{0} was not built from the probe_uidag.cpp in this tree: {1} - rebuild before this can mean anything" -f (Split-Path -Leaf $built), $srcStale)
    }
}

# ---------------------------------------------------------------------------------------------------
# 3. The filing-site proof, run three ways.
#
#    A  claimer first, then the REAL probe   -> must load, must file a PROVISIONAL record before it
#                                               measures anything, and must leave a final record
#    B  claimer first, then the MUTANT       -> must file NOTHING, and the reason must be that its
#                                               initialisation declined (err=1114), not that the DLL
#                                               was missing. That distinction is the whole guarantee:
#                                               "it filed nothing" is only evidence about the probe if
#                                               the probe was there and its DllMain ran.
#    C  the mutant ALONE                     -> recorded, not required. It is what says how much of B
#                                               is attributable to the instance claim rather than to
#                                               the bootstrap declining to run in a host process.
# ---------------------------------------------------------------------------------------------------
function Invoke-Host([string]$label, [string[]]$dlls) {
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("w1m_uidag_" + [System.Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    $out = & $host_ @($tmp) @dlls 2>&1
    $code = $LASTEXITCODE
    Write-Host "  [$label] host exit=$code"
    $out | ForEach-Object { Write-Host "    $_" }
    return [pscustomobject]@{ Root = $tmp; Exit = $code; Out = @($out | ForEach-Object { [string]$_ }) }
}

function Get-Verdict([string]$root, [string]$stem) {
    $p = Join-Path (Join-Path $root $stem) 'done.json'
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    try { return Get-Content -Raw -LiteralPath $p | ConvertFrom-Json } catch { return 'unparseable' }
}

# The host prints one `snapshot <stem> <state>` line per DLL taken the instant LoadLibraryA returned,
# and one `after-settle snapshot ...` line per DLL taken after the sampler has had time to run.
function Get-Snapshot([object]$run, [string]$stem, [switch]$AfterSettle) {
    $prefix = if ($AfterSettle) { 'after-settle snapshot' } else { 'snapshot' }
    $line = $run.Out | Where-Object { $_ -match ("^" + [regex]::Escape($prefix) + "\s+" + [regex]::Escape($stem) + "\s+(.+)$") } |
        Select-Object -First 1
    if (-not $line) { return $null }
    if ($line -match ("^" + [regex]::Escape($prefix) + "\s+" + [regex]::Escape($stem) + "\s+(.+)$")) { return $matches[1].Trim() }
    return $null
}

function Get-LoadResult([object]$run, [string]$stem) {
    $line = $run.Out | Where-Object { $_ -match ("^load\s+" + [regex]::Escape($stem) + "\s+(.+)$") } | Select-Object -First 1
    if ($line -match ("^load\s+" + [regex]::Escape($stem) + "\s+(.+)$")) { return $matches[1].Trim() }
    return $null
}

$runA = $null
$runB = $null
$runC = $null
try {
    # -------------------------------------------------------------------- A
    $runA = Invoke-Host 'A: claimer then real probe' @($claimer, $probe)
    $checked += 1
    if ($runA.Exit -ne 0) { Fail 'run A: a DLL was missing from disk, so nothing it filed or did not file means anything' }

    $checked += 1
    if ((Get-LoadResult $runA 'probe_uidag') -ne 'ok') {
        Fail 'run A: the real probe did not load, so this run says nothing about where it files from'
    }

    # ★ The mechanism itself: at the instant LoadLibraryA returned, a PROVISIONAL record was already on
    #   disk. Only a static initialiser can have written it - DllMain had just returned and the sampler
    #   thread was still blocked on its own DLL_THREAD_ATTACH.
    $snapA = Get-Snapshot $runA 'probe_uidag'
    Write-Host "    snapshot at load time: $snapA"
    $checked += 1
    if ($snapA -notmatch '^provisional') {
        Fail "run A: at load time the record was '$snapA', not provisional - a killed measurement would then be indistinguishable from a finished one"
    }
    $checked += 1
    if ($snapA -notmatch 'says-static-initialiser') {
        Fail 'run A: the record filed at load time does not say where it was written from'
    }

    $vA = Get-Verdict $runA.Root 'probe_uidag'
    $checked += 1
    if (-not $vA) {
        Fail 'run A: the probe left no done.json behind a claimer - this is exactly the defect the static initialiser exists to prevent'
    } else {
        Write-Host ("    final record: probe={0} verdict={1} checked={2} undetermined={3} provisional={4}" -f `
                $vA.probe, $vA.verdict, $vA.checked, $vA.undetermined, $vA.provisional)
        $checked += 1
        if ($vA.probe -ne 'W1-M-uidag-offline') {
            Fail "run A: probe id came back '$($vA.probe)', not the one REPLAY_PROBE_IDS named"
        }
        # In a host process there is no echovr.exe mapped, so the site windows cannot match and every
        # engine read must come back COULD-NOT-DETERMINE. A probe that reported a confident answer here
        # would be reporting one it could not have.
        $checked += 1
        if ($vA.detail -notmatch 'COULD-NOT-DETERMINE') {
            Fail 'run A: with no game image mapped, the probe reported no undetermined check - it is answering questions it cannot see'
        }
        $checked += 1
        if ([int]$vA.undetermined -le 0) {
            Fail 'run A: undetermined=0 in a host process, where every engine read is unreachable'
        }
        $checked += 1
        if ($vA.detail -notmatch 'DOES NOT ESTABLISH') {
            Fail 'run A: the verdict does not state what it fails to establish'
        }
    }

    # -------------------------------------------------------------------- B
    $runB = Invoke-Host 'B: claimer then MUTANT' @($claimer, $mutant)
    $checked += 1
    if ($runB.Exit -ne 0) { Fail 'run B: a DLL was missing from disk, so the mutant filing nothing proves nothing' }

    $loadB = Get-LoadResult $runB 'probe_uidag_mutant'
    Write-Host "    mutant load result: $loadB"
    $checked += 1
    # ok, or FAILED err=1114 (ERROR_DLL_INIT_FAILED) - both mean the DLL was found and its DllMain ran.
    # Any other failure means it was never there, and then it filed nothing for an unrelated reason.
    if (-not ($loadB -eq 'ok' -or $loadB -eq 'FAILED err=1114')) {
        Fail "run B: the mutant's load result was '$loadB'; the check needs it either loaded or refused BY ITS OWN INITIALISATION, otherwise 'it filed nothing' is about the build"
    }

    $vB = Get-Verdict $runB.Root 'probe_uidag_mutant'
    $checked += 1
    if ($vB) {
        Fail 'run B: the mutant filed a verdict behind a claimer, so this check no longer reproduces the defect it exists to reproduce'
    } else {
        Write-Host '    the mutant filed nothing behind a claimer, as the old broken shape does'
    }

    # ★ The discriminating comparison, stated as its own assertion so it cannot be lost in the noise:
    #   same source, same host, same claimer ahead of it, opposite outcomes, and the only difference is
    #   where the verdict is filed from.
    $checked += 1
    if (-not ($vA -and -not $vB)) {
        Fail 'the two builds did not come out different: the mutation must fail exactly where the real probe succeeds, or it is not testing the fix'
    } else {
        Write-Host '    DISCRIMINATED: static-initialiser filing survives the instance claim; module-Init filing does not'
    }

    # -------------------------------------------------------------------- C
    $runC = Invoke-Host 'C: mutant alone (control, recorded not required)' @($mutant)
    $loadC = Get-LoadResult $runC 'probe_uidag_mutant'
    $vC = Get-Verdict $runC.Root 'probe_uidag_mutant'
    if ($vC) {
        Write-Host '    the mutant DOES file when it loads first, so run B is attributable to the instance claim itself'
    } elseif ($loadC -eq 'ok') {
        Write-Host '    the mutant loads alone but still files nothing inside the settle window: in a host process the bootstrap has no game to guard, so its modules do not reach Init. Run B then shows the weaker but sufficient property - a verdict written from a module Init does not survive a bootstrap that does not run to Init - and the real probe files under both'
    } else {
        Write-Host "    the mutant did not load even alone ($loadC); run B's attribution is correspondingly weaker and is recorded as such"
    }
} finally {
    foreach ($r in @($runA, $runB, $runC)) {
        if ($r) { Remove-Item -LiteralPath $r.Root -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

Write-Host "GATE-RESULT checked=$checked subject=probe-site-windows-vs-echovr+decoded-offsets+staleness+provisional-filed-at-load-time+verdict-survives-instance-refusal"
if ($failed) { exit 1 }
exit 0
