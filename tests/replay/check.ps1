# P3's gate check. One question, asked of the build system rather than of a comment:
#
#   does probe_replay.dll contain the product's sources, all of them, and exactly one file more?
#
# The design notes derive probe_replay from echoreplay's own SOURCES property precisely so the two
# binaries cannot drift. A derivation nothing checks is a derivation that survives being replaced by a
# hand-typed list, which is what F11 found in the plan's first draft - so the claim is asserted here
# against CMake's own record of the link graph (C5's target_manifest.json), not against CMake text.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

$failed = $false
$checked = 0
function Fail([string]$why) { Write-Host "  FAIL $why"; $script:failed = $true }
function Pass([string]$what) { Write-Host "  ok   $what"; $script:checked++ }

# â˜…â˜… BY CONTENT, NOT BY MTIME. Eleven agents share this tree and any checkout rewrites a source
# byte-for-byte with a fresh timestamp, so an mtime guard reds on a no-op and gets muted.
# tools\build.ps1 records the SHA-256 of every tracked source AFTER a successful build, from the
# pre-build content, so it is a statement about what went into this binary.
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
# certify anything.
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

# The two files this check speaks for: the observer, and the list that derives the target. A stale
# either way and the manifest below describes a build nobody asked for.
foreach ($subject in @((Join-Path $repo 'tests\replay\probe_replay.cpp'), (Join-Path $repo 'tests/replay/observer_rows.h'), (Join-Path $repo 'tests/replay/test_observer.cpp'), (Join-Path $repo 'CMakeLists.txt'))) {
    if (-not (Test-Path -LiteralPath $subject)) { Fail "missing: $subject"; continue }
    $why = Get-StaleReason $subject
    if ($why) { Fail "staleness: $why" } else { Pass "current: $(Split-Path -Leaf $subject)" }
}

# --- C5's manifest: CMake's own record of what each SHARED target contains.
$manifestPath = Join-Path $build 'target_manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) {
    Write-Host "  FAIL no $manifestPath - configure did not write the target manifest, so nothing here can be asked"
    
Write-Host "GATE-RESULT checked=$checked subject=probe_replay-object-list-vs-echoreplay+staleness+export-pin+descriptor-denominator+carries-bootstrap+sc7-read-only"
    exit 1
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$rows = @{}
foreach ($row in $manifest.targets) { $rows[[string]$row.target] = $row }

$rowsMissing = $false
foreach ($name in @('echoreplay', 'probe_replay')) {
    if (-not $rows.ContainsKey($name)) { Fail "the manifest has no row for $name"; $rowsMissing = $true }
}
if ($rowsMissing) {
    Write-Host "GATE-RESULT checked=$checked subject=probe_replay-object-list-vs-echoreplay+staleness+export-pin+descriptor-denominator+carries-bootstrap+sc7-read-only"
    exit 1
}

$prodSources = @($rows['echoreplay'].sources | ForEach-Object { ([string]$_).ToLowerInvariant() })
$probeSources = @($rows['probe_replay'].sources | ForEach-Object { ([string]$_).ToLowerInvariant() })
$observer = 'tests/replay/probe_replay.cpp'

# â˜… THE CLAIM, BOTH WAYS. "The probe has one extra file" is half the property: a probe MISSING one of
# the product's files would also be a divergence, and a count alone cannot tell the two apart.
$missing = @($prodSources | Where-Object { $probeSources -notcontains $_ })
$extra = @($probeSources | Where-Object { $prodSources -notcontains $_ })

if ($prodSources.Count -lt 10) {
    Fail ("echoreplay's manifest row lists only $($prodSources.Count) source(s); the product is populated " +
          'from twelve subdirectories, so this manifest is not describing the product')
} else {
    Pass "echoreplay carries $($prodSources.Count) sources"
}
if ($missing.Count -gt 0) {
    Fail ("probe_replay is missing $($missing.Count) of the product's own sources, so it is not the product " +
          'plus an observer: ' + (($missing | Select-Object -First 5) -join ', '))
} else {
    Pass 'probe_replay carries every source echoreplay carries'
}
if ($extra.Count -eq 1 -and $extra[0] -eq $observer) {
    Pass "probe_replay's only extra translation unit is $observer"
} else {
    Fail ("probe_replay's extra sources are [" + ($extra -join ', ') + "]; exactly one was expected, $observer. " +
          'A second file here means the observer has grown a private copy of something the product owns')
}

$bootstrapAndTick = $true
foreach ($name in @('echoreplay', 'probe_replay')) {
    if (-not $rows[$name].carriesBootstrap) { Fail "$name does not carry src/bootstrap/bootstrap.cpp"; $bootstrapAndTick = $false }
    if (-not $rows[$name].carriesTick) { Fail "$name does not carry the tick"; $bootstrapAndTick = $false }
}
# Keyed on THIS arm's own result, not on the global $failed: a red anywhere earlier in the file
# would otherwise suppress a Pass about a question that was asked and answered.
if ($bootstrapAndTick) { Pass 'both targets carry bootstrap and the tick, which is why only one may be installed' }

# --- the binary itself
$dll = Join-Path (Join-Path $build 'bin') 'probe_replay.dll'
if (Test-Path -LiteralPath $dll) { Pass "probe_replay.dll is built ($dll)" } else { Fail "no $dll" }

# --- the export pin. tests/exports owns the surface; this asserts it was EXTENDED to the new target
# rather than left pinning echoreplay alone, because an unpinned DLL whose symbols the engine can
# GetProcAddress is the hazard WINDOWS_EXPORT_ALL_SYMBOLS OFF exists for.
$exportsCheck = Join-Path $repo 'tests\exports\check.ps1'
if (-not (Test-Path -LiteralPath $exportsCheck)) {
    Fail "no $exportsCheck"
} elseif ((Get-Content -LiteralPath $exportsCheck -Raw) -match 'probe_replay\.dll') {
    Pass 'tests/exports pins probe_replay.dll as well as echoreplay.dll'
} else {
    Fail 'tests/exports/check.ps1 does not name probe_replay.dll, so the new DLL has no pinned export surface'
}


# ---------------------------------------------------------------------------------------------
# THE DENOMINATOR COMES FROM THE DESCRIPTOR, NOT FROM THE OBSERVER'S OWN TABLE.
#
# A coverage denominator supplied by the work being measured cannot
# fail. The observer's arm tables live in observer_rows.h; the rows a launch MUST ask live in
# W2-R.probe.json's answers[]. This asserts the two are the SAME SET per arm, in both directions:
#
#   - a descriptor row the arm does not produce would render never-asked on every launch;
#   - an arm row no descriptor lists would be an answer nobody asked for, and would let the
#     observer's table quietly grow its own denominator.
# ---------------------------------------------------------------------------------------------
$spec = $null
$rowsHeader = Join-Path $repo 'tests/replay/observer_rows.h'
$descriptorPath = Join-Path $repo 'tests/replay/W2-R.probe.json'
if (-not (Test-Path -LiteralPath $rowsHeader)) { Fail 'observer_rows.h is missing' }
elseif (-not (Test-Path -LiteralPath $descriptorPath)) { Fail 'W2-R.probe.json is missing' }
else {
    $armOf = [ordered]@{ 'kArmCapture' = 'CAPTURE'; 'kArmPlayback' = 'PLAYBACK'; 'kArmBrowser' = 'BROWSER'; 'kArmIdle' = 'PLAYBACK-IDLE' }
    $tableRows = @{}
    foreach ($word in $armOf.Values) { $tableRows[$word] = New-Object System.Collections.Generic.List[string] }
    $headerText = Get-Content -LiteralPath $rowsHeader -Raw
    $tableBody = [regex]::Match($headerText, 'kRows\[\] = \{(?<body>.*?)\n    \};', 'Singleline')
    if (-not $tableBody.Success) { Fail 'observer_rows.h: the kRows table could not be parsed, so the arms have no readable row list' }
    else {
        $entries = [regex]::Matches($tableBody.Groups['body'].Value, '\{\s*"(?<id>[^"]+)",(?<arms>[^}]*?),\s*"(?<by>[^"]+)"\s*\}')
        foreach ($entry in $entries) {
            foreach ($bit in $armOf.Keys) {
                if ($entry.Groups['arms'].Value -match [regex]::Escape($bit)) { $tableRows[$armOf[$bit]].Add($entry.Groups['id'].Value) }
            }
        }
        $pairs = 0
        foreach ($word in $armOf.Values) { $pairs += $tableRows[$word].Count }
        if ($entries.Count -eq 0 -or $pairs -eq 0) { Fail 'observer_rows.h: no rows parsed out of kRows, so this check would pass vacuously' }
        else { Pass "observer_rows.h publishes $($entries.Count) rows and $pairs arm/row pairs" }

        $script:spec = Get-Content -LiteralPath $descriptorPath -Raw | ConvertFrom-Json
        foreach ($probe in $spec.probes) {
            # The arm a descriptor row selects, by the same two rules the module uses: the profile
            # supplies the ECHOREPLAY_OPEN shape, and an id ending in -idle declares the attended arm.
            $arm = 'PLAYBACK'
            if ($probe.id -match '-browser$') { $arm = 'BROWSER' }
            elseif ($probe.id -match '-capture$') { $arm = 'CAPTURE' }
            elseif ($probe.id -match '-idle$') { $arm = 'PLAYBACK-IDLE' }
            $listed = @($probe.answers)
            $produced = @($tableRows[$arm])
            if ($listed.Count -eq 0) { Fail "$($probe.id): answers[] is empty, so its launch has no denominator and every row would render never-asked"; continue }
            $missing = @($listed | Where-Object { $produced -notcontains $_ })
            $unasked = @($produced | Where-Object { $listed -notcontains $_ })
            if ($missing.Count -gt 0) { Fail "$($probe.id) ($arm): the descriptor lists $($missing -join ', '), which the arm does not produce - they would render NEVER-ASKED on every launch" }
            elseif ($unasked.Count -gt 0) { Fail "$($probe.id) ($arm): the arm produces $($unasked -join ', '), which no descriptor row asks for" }
            else { Pass "$($probe.id): the $arm arm produces exactly the $($listed.Count) row(s) the descriptor lists" }
            # ARCH-11 S2.2 / DESIGN S12.8.3: observer rows are declared in rows[], read ids in
            # answers[]. During the beside-phase the two must carry the SAME set, or a descriptor
            # that grew a row in one key and not the other would quietly ask a different question of
            # each reader.
            $rowsKey = @($probe.rows)
            if ($rowsKey.Count -eq 0) { Fail "$($probe.id): rows[] is missing or empty; observer row ids are declared there (ARCH-11 S2.2)" }
            else {
                $rowsOnly = @($rowsKey | Where-Object { $listed -notcontains $_ })
                $ansOnly = @($listed | Where-Object { $rowsKey -notcontains $_ })
                if ($rowsOnly.Count -gt 0 -or $ansOnly.Count -gt 0) { Fail "$($probe.id): rows[] and answers[] disagree - only in rows[]: $($rowsOnly -join ', '); only in answers[]: $($ansOnly -join ', ')" }
                else { Pass "$($probe.id): rows[] carries the same $($rowsKey.Count) observer row id(s) as answers[]" }
            }
        }
    }
}

# ---------------------------------------------------------------------------------------------
# THE OBSERVER MUST NEVER WRITE sc7_text_diff.json (AUDIT_W1-C_SC7 #8, which has FIRED).
#
# src/bootstrap/sc7.cpp is compiled into this binary and writes that record itself. A second writer
# here would race the real one, and the loser's content decides a seven-row block: a stub landing
# last would render a reader that actually ran as NOT-RUN. The observer READS it back and scores it.
# ---------------------------------------------------------------------------------------------
$observerText = Get-Content -LiteralPath (Join-Path $repo $observer) -Raw
# The file's own header comment names the record it must never write, so the counting is done on
# the CODE: a rule that reds on its own explanation teaches people to delete the explanation.
$observerCode = [regex]::Replace($observerText, '(?m)//.*$', '')
$sc7Names = ([regex]::Matches($observerCode, 'sc7_text_diff\.json')).Count
if ($sc7Names -eq 0) { Fail 'the observer never names sc7_text_diff.json, so it is not reading the record back either' }
elseif ($sc7Names -ne 1) { Fail "the observer names sc7_text_diff.json $sc7Names times; the windowed read-mode test below can only speak for one of them" }
elseif ($observerText -match 'replay::sc7::WriteJson') { Fail 'the observer calls sc7::WriteJson, which would race the product writer' }
elseif ($observerText -match '(?s)sc7_text_diff\.json.{0,400}?fopen_s[^;]*"(w|a)') { Fail 'the observer opens sc7_text_diff.json in a write or append mode' }
elseif ($observerText -notmatch '(?s)sc7_text_diff\.json.{0,400}?fopen_s[^;]*"rb"') { Fail 'the observer names sc7_text_diff.json but never opens it for reading' }
else { Pass 'the observer only READS sc7_text_diff.json; src/bootstrap/sc7.cpp writes it' }

# The regex above is a 400-char window around ONE literal and knows only fopen_s. AUDIT_P3_ARMS 8.1
# recorded the gap and AUDIT_P3_ARMS_R2 7 FIRED it: a planted `fopen(path, "wb")` on the line above
# the real read, truncating L5's own artifact, passed the whole gate - the census knew only fopen_s
# and the object scan only ofstream/_wfopen/CreateFileW/WriteFile.
#
# So the census is over the whole stdio open family now, by API and by MODE, and it reports the name
# each call opens - the literal where the call site spells one, the variable text otherwise, which is
# what SampleSc7 does. An open whose mode literal this gate cannot read is a REFUSAL and not a shrug:
# a mode it cannot classify is exactly the write it would otherwise wave through.
function Get-FileOpens([string]$code) {
    $found = @()
    $rx = '(?<![A-Za-z0-9_])(?<api>_wfopen_s|_wfopen|freopen_s|freopen|fopen_s|fopen)\s*\((?<argtext>[^;{}]*?)\)'
    foreach ($m in [regex]::Matches($code, $rx)) {
        $argText = $m.Groups['argtext'].Value
        $strings = @([regex]::Matches($argText, '"((?:[^"\\]|\\.)*)"') | ForEach-Object { $_.Groups[1].Value })
        $modes = @($strings | Where-Object { $_ -match '^[rwa][btcnSRTD+]*$' })
        $mode = if ($modes.Count -gt 0) { $modes[-1] } else { $null }
        $target = ($argText -replace '\s+', ' ').Trim()
        $found += [pscustomobject]@{
            Api      = $m.Groups['api'].Value
            Mode     = $mode
            Target   = $target
            IsWrite  = ($null -ne $mode -and $mode -match '[wa+]')
            Unreadable = ($null -eq $mode)
        }
    }
    return @($found)
}

# The C++ and Win32 halves are a presence test, because this file has never used one of them and a
# single reference is the finding. std::filesystem's mutators are on the list for the same reason:
# a remove() or a rename() over the artifact needs no open at all.
# CreateFileA is NOT on the list: the observer's own log tailer opens the game log through it,
# read-only (probe_replay.cpp, GENERIC_READ | OPEN_EXISTING), so a presence test on the name would
# red on a legitimate reader. Its MODE is censused separately below, which is the same rule the
# stdio family gets. Matching is CASE-SENSITIVE and word-anchored: `-match '_open'` is
# case-insensitive in PowerShell and hits ECHOREPLAY_OPEN, which is a gate that reds on an env name.
$nonStdioWriters = @('ofstream', 'basic_ofstream', 'CreateFileW', 'WriteFile', 'GENERIC_WRITE',
    'FILE_APPEND_DATA', 'CREATE_ALWAYS', 'TRUNCATE_EXISTING', 'SetEndOfFile', 'std::filesystem',
    'filesystem::remove', 'filesystem::rename', 'filesystem::copy', 'filesystem::resize_file',
    '_open', '_wopen', 'SHFileOperation')

# KNOWN-ANSWER CONTROLS, run before the census is believed: the auditor's own plant must red, and
# the read-only twin of it must not. A census that cannot fail certifies nothing, and this pair is
# the proof that this one can - it runs on every check, over the real file's own text.
$plantWrite = $observerCode + "`nstatic void ControlPlant() { FILE* wz = fopen(path, `"wb`"); fputs(`"{}`", wz); fclose(wz); }`n"
$plantRead = $observerCode + "`nstatic void ControlPlant() { FILE* rz = fopen(path, `"rb`"); fclose(rz); }`n"
$baseWrites = @(Get-FileOpens $observerCode | Where-Object { $_.IsWrite })
$plantWriteCount = @(Get-FileOpens $plantWrite | Where-Object { $_.IsWrite }).Count
$plantReadCount = @(Get-FileOpens $plantRead | Where-Object { $_.IsWrite }).Count
if ($plantWriteCount -ne ($baseWrites.Count + 1)) {
    Fail "the write-open census does not see a planted bare fopen(path, `"wb`") - it counted $plantWriteCount against a baseline of $($baseWrites.Count), so it cannot fail and certifies nothing"
} elseif ($plantReadCount -ne $baseWrites.Count) {
    Fail "the write-open census counted a planted READ-only fopen(path, `"rb`") as a writer ($plantReadCount against a baseline of $($baseWrites.Count)); a census that reds on the observer's own read is not usable"
} else {
    Pass "the write-open census reds on a planted fopen(path, `"wb`") and not on the read-only twin (baseline $($baseWrites.Count))"
}

$allOpens = @(Get-FileOpens $observerCode)
$unreadable = @($allOpens | Where-Object { $_.Unreadable })
if ($unreadable.Count -gt 0) {
    Fail ('the observer opens a file whose mode this gate cannot read: ' + (($unreadable | ForEach-Object { $_.Api + '(' + $_.Target + ')' }) -join '; ') + ' - an unclassifiable mode is refused, never assumed read-only')
} elseif ($baseWrites.Count -ne 1) {
    Fail ("the observer has $($baseWrites.Count) write-mode open(s) across the fopen/fopen_s/freopen/_wfopen family; exactly one is expected, the record writer, and a second one is a writer nobody declared: " + (($baseWrites | ForEach-Object { $_.Api + ' "' + $_.Mode + '" ' + $_.Target }) -join '; '))
} elseif ($baseWrites[0].Target -match 'sc7_text_diff') {
    Fail ('the observer opens sc7_text_diff.json for writing: ' + $baseWrites[0].Api + ' "' + $baseWrites[0].Mode + '" ' + $baseWrites[0].Target)
} elseif (-not @($allOpens | Where-Object { -not $_.IsWrite -and $_.Target -match 'sc7|Sc7' }).Count) {
    Pass ('the observer opens exactly one file for writing, its own record (' + $baseWrites[0].Api + ' "' + $baseWrites[0].Mode + '"); the sc7 record is opened through a path variable and the windowed read-mode test above covers it')
} else {
    Pass ('the observer opens exactly one file for writing, its own record (' + $baseWrites[0].Api + ' "' + $baseWrites[0].Mode + '"), and reads the sc7 record read-only')
}

$otherWriters = @($nonStdioWriters | Where-Object {
        $observerCode -cmatch ('(?<![A-Za-z0-9_])' + [regex]::Escape($_) + '(?![A-Za-z0-9_])') })
if ($otherWriters.Count -gt 0) {
    Fail ('the observer names ' + ($otherWriters -join ', ') + ' - a write through an API this file has never used is exactly what the windowed fopen_s test cannot see')
} else {
    Pass "the observer names none of the $($nonStdioWriters.Count) non-stdio write APIs (ofstream, CreateFileW, WriteFile, GENERIC_WRITE, the truncating dispositions, std::filesystem mutators, _open)"
}

# CreateFile's mode lives in its access mask, not in a mode string, so it is censused the same way
# the stdio opens are: every call site must ask for GENERIC_READ and nothing else. A call this gate
# cannot classify is a refusal, not a pass.
$createFileCalls = @([regex]::Matches($observerCode, '(?<![A-Za-z0-9_])CreateFile[AW]\s*\(([^;{}]*?)\)'))
$badMask = @($createFileCalls | Where-Object {
        $a = $_.Groups[1].Value
        ($a -cnotmatch 'GENERIC_READ') -or ($a -cmatch 'GENERIC_WRITE|GENERIC_ALL|FILE_APPEND_DATA|FILE_WRITE_DATA|CREATE_ALWAYS|TRUNCATE_EXISTING') })
if ($createFileCalls.Count -eq 0) {
    Pass 'the observer makes no CreateFile call, so there is no access mask to classify'
} elseif ($badMask.Count -gt 0) {
    Fail ("$($badMask.Count) of the observer's $($createFileCalls.Count) CreateFile call(s) do not ask for GENERIC_READ alone: " + (($badMask | ForEach-Object { ($_.Value -replace '\s+', ' ') }) -join '; '))
} else {
    Pass "all $($createFileCalls.Count) of the observer's CreateFile call(s) ask for GENERIC_READ and none of the write or truncating masks"
}

# Everything above censuses ways of OPENING the record. Destroying it needs no
# open at all: remove(), rename(), DeleteFile and MoveFile take a path and are done. The observer
# legitimately uses three of them on its OWN record (MoveFileExA/CopyFileA/DeleteFileA over the
# done.json temp), so this is censused by TARGET the way CreateFileA is censused by mask, and not
# by presence of the name.
function Get-PathMutators([string]$code) {
    $rx = '(?<![A-Za-z0-9_.:>])(?<api>_wremove|_wrename|remove|rename|DeleteFile[AW]?|MoveFileEx[AW]?|MoveFile[AW]?|ReplaceFile[AW]?|CopyFileEx[AW]?|CopyFile[AW]?)\s*\((?<argtext>[^;{}]*?)\)'
    $out = @()
    foreach ($m in [regex]::Matches($code, $rx)) {
        $out += [pscustomobject]@{
            Api    = $m.Groups['api'].Value
            Target = ($m.Groups['argtext'].Value -replace '\s+', ' ').Trim()
        }
    }
    return @($out)
}

# The sc7 path is built into a LOCAL named `path`, so naming the variable is not enough on its own;
# the windowed form is what catches a mutator applied to the freshly-built path, and the target form
# is what catches one applied to a variable that spells the record. Both, because either alone has a
# hole the other closes.
$sc7MutatorWindow = '(?s)sc7_text_diff\.json.{0,400}?(?<![A-Za-z0-9_.:>])(_wremove|_wrename|remove|rename|DeleteFile[AW]?|MoveFileEx[AW]?|MoveFile[AW]?|ReplaceFile[AW]?|CopyFileEx[AW]?|CopyFile[AW]?)\s*\('
$mutatorPlant = $observerCode -replace '(sc7_text_diff\.json[^;]*;)', '$1 remove(path);'
$mutatorNamed = $observerCode + "`nstatic void ControlPlant() { DeleteFileA(sc7DiffPath); }`n"
$plantWindowHit = [bool]([regex]::IsMatch($mutatorPlant, $sc7MutatorWindow))
$plantNamedHit = @(Get-PathMutators $mutatorNamed | Where-Object { $_.Target -match 'sc7' }).Count
if (-not $plantWindowHit) {
    Fail 'the sc7 mutator census does not see a planted remove(path) beside the sc7_text_diff.json literal, so it cannot fail and certifies nothing'
} elseif ($plantNamedHit -ne 1) {
    Fail "the sc7 mutator census counted $plantNamedHit planted DeleteFileA(sc7DiffPath) call(s) instead of 1, so a delete through a variable that spells the record would pass"
} else {
    Pass 'the sc7 mutator census reds on a planted remove(path) beside the literal and on a planted DeleteFileA over a variable naming the record'
}

$sc7Mutators = @(Get-PathMutators $observerCode | Where-Object { $_.Target -match 'sc7' })
if ($sc7Mutators.Count -gt 0) {
    Fail ('the observer deletes or renames the sc7 record: ' + (($sc7Mutators | ForEach-Object { $_.Api + '(' + $_.Target + ')' }) -join '; '))
} elseif ([regex]::IsMatch($observerCode, $sc7MutatorWindow)) {
    Fail 'the observer calls remove/rename/DeleteFile/MoveFile within 400 characters of the sc7_text_diff.json literal, which is the path it has just built'
} else {
    $ownRecord = @(Get-PathMutators $observerCode)
    Pass ("the observer names no remove/rename/DeleteFile/MoveFile over the sc7 record; its $($ownRecord.Count) path mutator call(s) are over its own done.json temp (" + (($ownRecord | ForEach-Object { $_.Api }) -join ', ') + ')')
}

# W1-J's egress record, WIRING. The composer is unit-proved in test_observer.cpp, and unit-green
# cannot see whether the probe ever calls it or whether it passes the product's own answer through:
# a literal state word would satisfy every test in that file. So the accessors are grepped in the
# non-test caller, and the composer must be reached from the record writer.
$egressReads = @('replay::egress::Current', 'replay::egress::ArmStateName', 'replay::egress::ArmTriggerName',
    'replay::egress::HookNamesJson', 'AppendEgressObject')
$missingReads = @($egressReads | Where-Object { $observerCode -cnotmatch [regex]::Escape($_) })
if ($missingReads.Count -gt 0) {
    Fail ('the observer does not read the egress module through ' + ($missingReads -join ', ') + " - the harness's only probe-side source for the last layer would be absent or re-derived")
} elseif ($observerCode -cmatch '"egress"\s*:') {
    Fail 'the observer spells the egress object by hand instead of composing it from the accessors'
} else {
    Pass "the observer files its egress record from the product's own accessors (Current/ArmStateName/ArmTriggerName/HookNamesJson) through AppendEgressObject"
}

# And the word that pass-through produces has to be one the harness reads. Resolve-EgressVerdict
# strips [^A-Za-z] and lowercases, so 'partially-armed' becomes 'partiallyarmed'; a product word
# outside that set prints red as not protecting, which is right for `failed` and wrong for an arm
# that actually armed. Taken from src/bootstrap/egress.cpp's own switch, not from a copy here.
$egressSrc = Join-Path $repo 'src/bootstrap/egress.cpp'
if (-not (Test-Path $egressSrc)) {
    Fail "src/bootstrap/egress.cpp is absent, so the state words the observer forwards cannot be read"
} else {
    $stateSwitch = [regex]::Match((Get-Content -Raw $egressSrc), '(?s)const char\* ArmStateName\(ArmState state\)\s*\{.*?\n\}')
    $words = @([regex]::Matches($stateSwitch.Value, 'return\s+"([^"]+)"') | ForEach-Object { ($_.Groups[1].Value -replace '[^A-Za-z]', '').ToLowerInvariant() } | Where-Object { $_ -ne '' })
    $armWords = @($words | Where-Object { $_ -eq 'armed' -or $_ -eq 'partiallyarmed' })
    if ($words.Count -lt 4) {
        Fail "ArmStateName in src/bootstrap/egress.cpp yielded $($words.Count) state word(s); the switch was not read, so this row certifies nothing"
    } elseif ($armWords.Count -ne 2) {
        Fail ("the product's arm-state words do not include both of the ones the harness recognises: " + ($words -join ', '))
    } else {
        Pass ("the $($words.Count) state words the observer forwards come from ArmStateName, and both arming words are ones Resolve-EgressVerdict reads (" + ($words -join ', ') + ')')
    }
}

# Binary side, and it is asked of the OBSERVER'S OWN OBJECT FILE rather than of the DLL: the DLL
# carries src/bootstrap/sc7.cpp, which legitimately writes that record, so a DLL-wide scan cannot
# tell the two writers apart. This object holds this translation unit alone. The sc7 literal is
# required to be IN it, which is the known-answer control that the scan is reading real content and
# not an empty or wrong file.
$observerObj = Join-Path $build (Join-Path 'CMakeFiles/probe_replay.dir' ($observer + '.obj'))
if (-not (Test-Path -LiteralPath $observerObj)) {
    Fail "no $observerObj, so nothing here can certify what file APIs the observer's own compiled code calls"
} else {
    $objBytes = [System.IO.File]::ReadAllBytes($observerObj)
    $objText = [System.Text.Encoding]::GetEncoding(28591).GetString($objBytes)
    # Two known-answer controls, because the previous version of this arm had only one. The sc7
    # literal proves the scan is reading THIS object's data; `fopen_s` proves it can also see the
    # object's imported CRT symbols, which is the half that the writer list depends on and the half
    # AUDIT_P3_ARMS_R2 D-2 recorded as having no control. Without the second, an empty writer list
    # and a scan that cannot see symbols at all are indistinguishable.
    $symbolProbe = 'fopen_s'
    if (-not $objText.Contains('sc7_text_diff.json')) {
        Fail "$observerObj does not carry the sc7_text_diff.json literal, so it is not the observer's object and this scan would certify nothing"
    } elseif ($objText -notmatch ('(?<![A-Za-z0-9])' + $symbolProbe + '(?![A-Za-z0-9_])')) {
        Fail "$observerObj does not reference $symbolProbe, the one file-open the observer's source does use, so this scan cannot see imported CRT symbols and its writer list would be silent for the wrong reason"
    } else {
        # Word-anchored: a bare `fopen` is the AUDIT_P3_ARMS_R2 7 plant, and a substring test for it
        # matches `fopen_s` and reds on the legitimate reader instead.
        # CreateFileA is off this list for the reason the source list gives, and it is not a hole:
        # the source census above proves every CreateFile call in this TU asks GENERIC_READ alone.
        $objWriterSymbols = @('fopen', 'freopen', 'freopen_s', '_wfopen', '_wfopen_s', 'ofstream',
            'basic_ofstream', 'CreateFileW', 'WriteFile', '_wopen')
        $objWriters = @($objWriterSymbols | Where-Object {
                $objText -match ('(?<![A-Za-z0-9])' + [regex]::Escape($_) + '(?![A-Za-z0-9_])') })
        if ($objWriters.Count -gt 0) {
            Fail ('the built observer object references ' + ($objWriters -join ', ') + ', so something the source scan cannot see is opening files for writing in this translation unit')
        } else {
            Pass "the built observer object ($($objBytes.Length) bytes) names the sc7 record, references $symbolProbe, and references none of the $($objWriterSymbols.Count) other open APIs"
        }
    }
}


# ---------------------------------------------------------------------------------------------
# THE DESCRIPTOR'S carriesBootstrap CLAIM, CHECKED AGAINST CMAKE'S RECORD (C5's second direction).
#
# A descriptor row asserting carriesBootstrap on a DLL whose manifest row says otherwise would send
# the sweep's Rule G over an install set it has mis-described: the guard would be evaluated against
# a plugin that claims an instance and a tick slot it does not take, or - worse - would let a second
# bootstrap-carrying DLL into the install because nobody said this one carries one.
# ---------------------------------------------------------------------------------------------
if ($null -ne $spec -and $rows.Count -gt 0) {
    foreach ($probe in $spec.probes) {
        $leaf = [System.IO.Path]::GetFileNameWithoutExtension([string]$probe.dll)
        if (-not $rows.ContainsKey($leaf)) {
            Fail "$($probe.id): the descriptor names $($probe.dll), which is not a SHARED target in the manifest"
            continue
        }
        $recorded = [bool]$rows[$leaf].carriesBootstrap
        $claimed = [bool]$probe.carriesBootstrap
        if ($claimed -ne $recorded) {
            Fail "$($probe.id): the descriptor says carriesBootstrap=$claimed for $leaf; CMake's manifest says $recorded"
        } else {
            Pass "$($probe.id): carriesBootstrap=$recorded agrees with the manifest's record for $leaf"
        }
    }
}

# ---------------------------------------------------------------------------------------------
# THE OBSERVER READS NO OBSERVER-PRIVATE ENVIRONMENT NAME.
#
# The child environment's allow-list is frozen at exactly two names.
# A third would be REFUSED at Test-LaunchWindows, so a launch that set it would
# not run and a launch that did not set it would silently take a fallback -- vacuous when present,
# invisible when absent. The window and the attendance come off the launch marker instead.
#
# Asked of the BUILT BINARY as well as of the source, because a header pulled into this DLL could
# reintroduce the read without the observer's own file naming it. A missing DLL is a FAIL: a binary
# that cannot be read cannot certify an absence.
# ---------------------------------------------------------------------------------------------
$envToken = 'REPLAY_OBSERVER'
if ($observerText -match $envToken) {
    Fail "the observer names $envToken in its source: the launch allow-list carries exactly two environment names and would refuse a third, so this read is either a refused launch or a silent fallback"
} else {
    Pass "the observer source names no $envToken* environment variable"
}

$probeDll = Join-Path $build 'bin/probe_replay.dll'
if (-not (Test-Path -LiteralPath $probeDll)) {
    Fail "probe_replay.dll is not at $probeDll, so nothing here can certify what environment names the observer BINARY reads"
} else {
    $dllBytes = [System.IO.File]::ReadAllBytes($probeDll)
    $dllText = [System.Text.Encoding]::GetEncoding(28591).GetString($dllBytes)
    $wideToken = ($envToken.ToCharArray() -join "`0")
    if ($dllText.Contains($envToken)) { Fail "the built probe_replay.dll carries the literal $envToken, so something linked into it reads that environment name" }
    elseif ($dllText.Contains($wideToken)) { Fail "the built probe_replay.dll carries the wide literal $envToken, so something linked into it reads that environment name" }
    else { Pass "the built probe_replay.dll ($($dllBytes.Length) bytes) carries no $envToken literal, narrow or wide" }
}

# The replacement has to actually be there, and it has to use the product's own directory
# resolution rather than a second one of our own.
if ($observerText -notmatch 'launch-marker\.json') { Fail 'the observer reads no launch marker, so a windowless arm has no declared window at all' }
elseif ($observerText -notmatch 'replay::StateDir\(\)') { Fail 'the observer does not resolve StateDir(), so it is looking for the marker somewhere the capture module does not watch' }
elseif ($observerText -notmatch 'no-launch-marker') { Fail 'the observer never names no-launch-marker, so an absent marker cannot render as could-not-ask' }
else { Pass 'the observer reads launch-marker.json through the product''s own StateDir() resolution, and names no-launch-marker when it is absent' }

# ---------------------------------------------------------------------------------------------
# THE QUEUE COPY OF THE DESCRIPTOR (docs/HARNESS.md S8: "only the `runs\queue\` copy is what a
# sweep reads. Re-sync it in the working tree; there is nothing to commit").
#
# That settles the direction: the TRACKED copy under tests\replay\ is the source -- it is the one a
# checkout restores and the one every gate here reads -- and runs\queue\ is a working-tree MIRROR of
# it. The danger is exactly one-sided: the tracked copy can pass every check in this file while a
# launch reads the other file. AUDIT_P3_ARMS_R3 recorded the two as md5-identical; by ENG-W2R-C1
# they had drifted past the `rows` key, and nothing noticed.
#
# An ABSENT queue copy is NOT a divergence -- a machine that has queued nothing has nothing to
# disagree with -- and it is reported in those words rather than passed as if it had been compared.
# ---------------------------------------------------------------------------------------------
$trackedDesc = Join-Path $repo 'tests/replay/W2-R.probe.json'
$queueDesc = Join-Path $repo 'runs/queue/W2-R.probe.json'
if (-not (Test-Path -LiteralPath $trackedDesc)) {
    Fail "the tracked descriptor is not at $trackedDesc, so the queue copy has no source to mirror"
} elseif (-not (Test-Path -LiteralPath $queueDesc)) {
    Pass 'no runs/queue/W2-R.probe.json on this machine, so there is no mirror to have diverged (the queue is machine state and is never committed)'
} else {
    $tHash = (Get-FileHash -LiteralPath $trackedDesc -Algorithm SHA256).Hash
    $qHash = (Get-FileHash -LiteralPath $queueDesc -Algorithm SHA256).Hash
    if ($tHash -eq $qHash) {
        Pass "runs/queue/W2-R.probe.json is byte-identical to the tracked tests/replay copy (sha256 $($tHash.Substring(0,12)))"
    } else {
        # The hash decides; the walk below only says WHAT to re-sync, so the message is actionable
        # rather than a bare inequality.
        $detail = New-Object System.Collections.ArrayList
        $tDoc = Get-Content -LiteralPath $trackedDesc -Raw | ConvertFrom-Json
        $qDoc = Get-Content -LiteralPath $queueDesc -Raw | ConvertFrom-Json
        foreach ($p in @($tDoc.probes)) {
            $q = @($qDoc.probes) | Where-Object { $_.id -eq $p.id } | Select-Object -First 1
            if ($null -eq $q) { [void]$detail.Add("$($p.id) is absent from the queue copy"); continue }
            if ($null -eq $q.rows) { [void]$detail.Add("$($p.id) has no rows[] key in the queue copy") }
            $missing = @(@($p.answers) | Where-Object { @($q.answers) -notcontains $_ })
            if ($missing.Count -gt 0) { [void]$detail.Add("$($p.id) answers[] is missing $($missing -join ', ')") }
        }
        foreach ($q in @($qDoc.probes)) {
            if (@($tDoc.probes) | Where-Object { $_.id -eq $q.id } | Select-Object -First 1) { continue }
            [void]$detail.Add("$($q.id) is in the queue copy ONLY, so re-syncing forward would drop it - resolve it by hand")
        }
        if ($detail.Count -eq 0) { [void]$detail.Add('the two parse to the same probes, ids and answers, so they differ in formatting or in a key this walk does not enumerate') }
        Fail "runs/queue/W2-R.probe.json has DIVERGED from the tracked tests/replay/W2-R.probe.json, and the queue copy is the one a sweep reads: $($detail -join '; '). docs/HARNESS.md S8: re-sync the queue copy FROM the tracked one in the working tree; there is nothing to commit"
    }
}

# ---------------------------------------------------------------------------------------------
# CLEAN(L5)'s FIXTURES (ARCH-14 / ARCH-15, the design notes). One synthetic record set per clause,
# driving each of the three answers the four-state vocabulary can give: clean, not-clean, and
# could-not-ask. Every fixture declares its own expected verdict beside itself in expect.json and
# this gate RUNS the resolver over it -- a fixture nothing executes is not a fixture.
#
# The set must cover all four clauses AND all three verdict words, asserted below: a fixture set
# that only holds clean records is a suite that cannot fail, which is the defect these exist to
# catch in the first place.
# ---------------------------------------------------------------------------------------------
$cleanRoot = Join-Path $repo 'tests/replay/fixtures/cleanl5'
$resolver = Join-Path $repo 'tools/compare/Resolve-CleanL5.ps1'
if (-not (Test-Path -LiteralPath $resolver)) {
    Fail "Resolve-CleanL5.ps1 is not at $resolver, so CLEAN(L5)'s fixtures have no reader and cannot be scored"
} elseif (-not (Test-Path -LiteralPath $cleanRoot)) {
    Fail "no $cleanRoot, so CLEAN(L5) has no fixture at all and its four clauses are exercised by nothing"
} else {
    $fixtures = @(Get-ChildItem -LiteralPath $cleanRoot -Directory | Sort-Object Name)
    if ($fixtures.Count -eq 0) {
        Fail "$cleanRoot holds no fixture directory, so the walk below would pass over an empty set"
    } else {
        $seenClauses = New-Object System.Collections.Generic.List[int]
        $seenVerdicts = New-Object System.Collections.Generic.List[string]
        $badFixture = $false
        foreach ($fx in $fixtures) {
            $expectPath = Join-Path $fx.FullName 'expect.json'
            if (-not (Test-Path -LiteralPath $expectPath)) {
                Fail "$($fx.Name): no expect.json, so nothing states what this fixture is supposed to prove"
                $badFixture = $true
                continue
            }
            $expect = Get-Content -LiteralPath $expectPath -Raw | ConvertFrom-Json
            $out = @(& powershell.exe -NoProfile -File $resolver -LaunchDir $fx.FullName 2>&1)
            $line = @($out | Where-Object { "$_" -like 'CLEAN-L5: *' }) | Select-Object -First 1
            if ($null -eq $line) {
                Fail "$($fx.Name): the resolver printed no CLEAN-L5 line, so it did not reach a verdict on this fixture"
                $badFixture = $true
                continue
            }
            $got = ("$line" -replace '^CLEAN-L5: ', '').Trim()
            if ($got -ne [string]$expect.verdict) {
                Fail "$($fx.Name): CLEAN(L5) resolved '$got' where the fixture declares '$($expect.verdict)' -- $($expect.why)"
                $badFixture = $true
                continue
            }
            if ([int]$expect.clause -gt 0) {
                $clauseLine = @($out | Where-Object { "$_" -like "clause $([int]$expect.clause): *" }) | Select-Object -First 1
                if ($null -eq $clauseLine -or "$clauseLine" -notlike "clause $([int]$expect.clause): $($expect.verdict)*") {
                    Fail "$($fx.Name): the whole-record verdict is '$got' but clause $([int]$expect.clause) did not carry it, so this fixture moved a DIFFERENT clause than the one it isolates: $clauseLine"
                    $badFixture = $true
                    continue
                }
                $seenClauses.Add([int]$expect.clause)
            }
            $seenVerdicts.Add([string]$expect.verdict)
        }
        if (-not $badFixture) { Pass "CLEAN(L5): $($fixtures.Count) fixture(s) each resolved to the verdict they declare, on the clause they name" }
        $missingClauses = @(1, 2, 3, 4) | Where-Object { -not $seenClauses.Contains($_) }
        if ($missingClauses.Count -gt 0) { Fail "CLEAN(L5) clause(s) $($missingClauses -join ', ') are driven by no fixture, so nothing here would notice if they stopped deciding anything" }
        else { Pass 'CLEAN(L5): every one of the four clauses is driven by a fixture that decides the verdict' }
        $missingWords = @('clean', 'not-clean', 'could-not-ask') | Where-Object { -not $seenVerdicts.Contains($_) }
        if ($missingWords.Count -gt 0) { Fail "no CLEAN(L5) fixture produces '$($missingWords -join "', '")', so the set cannot tell that verdict apart from the ones it does produce" }
        else { Pass 'CLEAN(L5): the fixture set produces all three of clean, not-clean and could-not-ask' }
    }
}

# ---------------------------------------------------------------------------------------------
# THE F-SYNTH FIXTURE'S DEFAULT LENGTH.
#
# The launch runbook regenerates F-SYNTH with `capture_mkdemo <out>` and no --frames (L3-RUN.md
# s.1 row 9), so capture_mkdemo's DEFAULT is the playback fixture's length. At 900 frames that was
# 9.999 s: shorter than the boot the file has to outlive, and far shorter than the scripted 60 s
# replay the design make the playback acceptance. A default nothing asserts goes back
# to 900 the next time someone tunes a unit test, silently, and the next L3 plays out before the
# level arrives.
#
# Read from mkdemo.cpp's source because that IS the artefact a runner runs, and asserted for SHAPE
# first: exactly one initialiser of each name. Unreadable is not zero, and a regex that matched
# nothing would otherwise report a floor nobody measured.
# ---------------------------------------------------------------------------------------------
$mkdemoSrc = Join-Path $repo 'tests/capture/mkdemo.cpp'
if (-not (Test-Path -LiteralPath $mkdemoSrc)) {
    Fail 'tests/capture/mkdemo.cpp is missing, so the F-SYNTH default length cannot be read'
} else {
    $mkdemoText = Get-Content -LiteralPath $mkdemoSrc -Raw
    $frameHits = [regex]::Matches($mkdemoText, '(?m)^\s*uint32_t\s+frames\s*=\s*(\d+)\s*;')
    $ivalHits  = [regex]::Matches($mkdemoText, '(?m)^\s*uint32_t\s+frameIntervalUs\s*=\s*(\d+)\s*;')
    if ($frameHits.Count -ne 1 -or $ivalHits.Count -ne 1) {
        Fail ("mkdemo.cpp declares $($frameHits.Count) 'uint32_t frames =' and $($ivalHits.Count) " +
              "'uint32_t frameIntervalUs =' initialiser(s); this check needs exactly one of each to " +
              'know which number is the default, and cannot report a length it did not read')
    } else {
        $defaultFrames = [long]$frameHits[0].Groups[1].Value
        $intervalUs = [long]$ivalHits[0].Groups[1].Value
        $defaultSec = ($defaultFrames * $intervalUs) / 1000000.0
        $floorSec = 60.0
        if ($defaultSec -lt $floorSec) {
            Fail ("capture_mkdemo's DEFAULT fixture is $defaultFrames frames x $intervalUs us = " +
                  "$([math]::Round($defaultSec, 3)) s, under the $floorSec s floor. The launch " +
                  'runbook regenerates F-SYNTH with no --frames, so this default is what an L3 ' +
                  'plays: a file this short runs out before the level it is meant to be played into')
        } else {
            Pass ("F-SYNTH default: $defaultFrames frames x $intervalUs us = " +
                  "$([math]::Round($defaultSec, 3)) s, at or above the $floorSec s floor")
        }
    }
}

# Test_replay links no product code, so the header helpers it proves are joined
# to the probe by this file alone. Assert the join here, where the source IS the subject: the probe
# must reach camera_control through the checked helpers and must never dereference the accessor.
$probeText = Get-Content -Raw -LiteralPath (Join-Path $repo 'tests\replay\probe_replay.cpp')
$bareDerefs = [regex]::Matches($probeText, 'cc::Get\(\)\s*->')
if ($bareDerefs.Count -ne 0) {
    Fail ("probe_replay.cpp dereferences camera_control::Get() directly $($bareDerefs.Count) time(s). " +
          'That is the L3-RUN-28 access violation verbatim: Get() answers null the moment ' +
          'camera_module unbinds. Read it through probe_replay::ReadObservedSelector, which makes ' +
          'null a named result')
} else {
    Pass 'probe_replay.cpp has no bare cc::Get()-> dereference'
}
foreach ($wiring in @(
        @{ pattern = 'ReadObservedSelector\(cc::Get\(\)'; what = 'the selector read-back re-asks for the rig' },
        @{ pattern = 'RigAcrossStep\('; what = 'the cam look-at step records what the rig was across it' },
        @{ pattern = 'StopRequested\(\); \}\)'; what = 'the bounded cam waits are stop-aware' },
        @{ pattern = 'g_status\.workerJoined\s*='; what = 'Shutdown publishes whether the worker joined' })) {
    $hits = [regex]::Matches($probeText, $wiring.pattern)
    if ($hits.Count -lt 1) {
        Fail ("probe_replay.cpp does not match /$($wiring.pattern)/, so " + $wiring.what +
              ' is not wired. The header-side unit tests pass either way -- unit-green cannot see wiring')
    } else {
        Pass ($wiring.what + " (wired, $($hits.Count) site(s))")
    }
}
Write-Host "GATE-RESULT checked=$checked subject=probe_replay-object-list-vs-echoreplay+staleness+export-pin+descriptor-denominator+carries-bootstrap+sc7-read-only+no-observer-env+launch-marker+answers-id-namespace+observer-object-writers+queue-copy-in-sync+cleanl5-fixtures+fixture-default-length+probe-camera-wiring"
if ($failed) { exit 1 }
exit 0
