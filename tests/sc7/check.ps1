# P12's JSON-escape check: the SC7 document, taken to a real parser.
#
# L5 (`20260902-081617Z`) wrote a `sc7_text_diff.json` that no parser would read - the module path went
# in raw, so `\P` sat at offset 2156. test_sc7.exe asserts the escaped bytes are in the document; that is
# a string search, and a string search cannot say the document PARSES. This does, with the same parser
# the rest of tests/ uses, and it reads the path back out to compare against the recorded input.
#
# The recorded input is not re-typed here: arm 3 recovers the provenance string from the broken L5
# artifact this directory carries, and holds the fresh document's parsed value against it.
$ErrorActionPreference = 'Stop'

$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

$failed = $false
$checked = 0
function Fail([string]$why) { Write-Host "  FAIL $why"; $script:failed = $true }
function Pass([string]$what) { Write-Host "  ok   $what"; $script:checked++ }

# '' when the parser accepted the text, otherwise the message it refused with.
function Get-ParseError([string]$text) {
    try {
        $null = ConvertFrom-Json $text
        return ''
    } catch {
        return $_.Exception.Message
    }
}

# --- 1. the binary was built from the sources this check speaks for.
#
# BY CONTENT, not by mtime: this tree is shared and a checkout rewrites a file byte-for-byte with a
# fresh timestamp. tools\build.ps1 records the SHA-256 of every tracked source after a successful build.
$hashRecordFile = Join-Path $build 'source_hashes.txt'
$recorded = $null
if (Test-Path -LiteralPath $hashRecordFile) {
    $recorded = @{}
    foreach ($line in (Get-Content -LiteralPath $hashRecordFile)) {
        $parts = $line -split ' ', 2
        if ($parts.Count -eq 2) { $recorded[$parts[1]] = $parts[0] }
    }
}
$subjects = @('src/bootstrap/jsonesc.h', 'src/bootstrap/sc7.cpp', 'tests/sc7/test_sc7.cpp')
if ($null -eq $recorded) {
    Fail "no content record in $hashRecordFile, so this run cannot tell a current binary from a stale one"
} else {
    foreach ($rel in $subjects) {
        $full = Join-Path $repo ($rel -replace '/', '\')
        if (-not $recorded.ContainsKey($rel)) {
            Fail "$rel is compiled in, but the build that produced this binary recorded no hash for it"
        } elseif ((Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash -ne $recorded[$rel]) {
            Fail "$rel has changed since the binary was built"
        } else {
            Pass "$rel is the content this binary was built from"
        }
    }
}

# --- 2. the two artifacts, written by this run and not left over from an earlier one.
$artifactRoot = Join-Path ([IO.Path]::GetTempPath()) 'replay_sc7_test'
$l5Dir = Join-Path $artifactRoot 'jsonesc-l5'
$hostileDir = Join-Path $artifactRoot 'jsonesc-hostile'
# AUDIT_L5FIX_R2 W1C-R2-1. The cap sweep in test_sc7.exe proves that every cap either returns THESE
# documents byte-for-byte or refuses; a string search cannot say either of them PARSES, so both ends of
# the sweep's claim come here. sweep-refusal is the COULD-NOT-ASK document itself, which until now no
# committed gate had ever taken to a parser.
$sweepDir = Join-Path $artifactRoot 'sweep-complete'
$sweepSmallDir = Join-Path $artifactRoot 'sweep-small'
$refusalDir = Join-Path $artifactRoot 'sweep-refusal'
foreach ($d in @($l5Dir, $hostileDir, $sweepDir, $sweepSmallDir, $refusalDir)) {
    $f = Join-Path $d 'sc7_text_diff.json'
    if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force }
}

$exe = Join-Path $build 'bin\test_sc7.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Fail "$exe is not built, so nothing here can be asked"
    Write-Host "CHECKS $checked"
    Write-Host 'GATE-RESULT checked=0 subject=sc7-json-escape+overflow-sweep'
    exit 1
}
$null = & $exe 2>&1
if ($LASTEXITCODE -ne 0) { Fail "test_sc7.exe exited $LASTEXITCODE - its own assertions come first" }
else { Pass 'test_sc7.exe is green' }

$docs = @{}
foreach ($pair in @(@('l5', $l5Dir), @('hostile', $hostileDir), @('sweep', $sweepDir),
                    @('sweep-small', $sweepSmallDir), @('refusal', $refusalDir))) {
    $f = Join-Path $pair[1] 'sc7_text_diff.json'
    if (-not (Test-Path -LiteralPath $f)) {
        Fail "$($pair[0]): $f was not written by this run"
    } else {
        $docs[$pair[0]] = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($f))
        Pass "$($pair[0]): the emitter wrote $($docs[$pair[0]].Length) chars"
    }
}

# --- 3. the retroactive proof. The broken artifact refuses; the new document, from the same recorded
# provenance string, parses and gives the string back unchanged.
$brokenFile = Join-Path $repo 'tests\sc7\L5-20260902-081617Z.sc7_text_diff.json'
$recoveredPath = $null
if (-not (Test-Path -LiteralPath $brokenFile)) {
    Fail 'the recorded L5 artifact is missing, so the retroactive arm cannot be asked'
} else {
    $broken = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($brokenFile))
    $err = Get-ParseError $broken
    # AUDIT_L5FIX W1C-1: this used to require the refusal at byte 2156. An offset is a property of the
    # file's whitespace, not of the defect - git's autocrlf turned the fixture into CRLF on every
    # checkout but the engineer's own, moving the defect to 2186 and making the packet's own T0 gate
    # red everywhere else. The defect is the invalid escape, so that is what is asserted: the first
    # backslash in the document that opens no legal JSON escape must be the `\P` of the raw module
    # path. The offset is reported, never required.
    $firstIllegal = -1
    $legal = '"\/bfnrtu'
    for ($i = 0; $i -lt $broken.Length; $i++) {
        if ($broken[$i] -ne '\') { continue }
        if ($i + 1 -ge $broken.Length -or $legal.IndexOf($broken[$i + 1]) -lt 0) { $firstIllegal = $i; break }
        $i++   # a legal escape consumes its second character
    }
    if (-not $err) {
        Fail 'the recorded L5 artifact PARSES - it is not the broken document this check was written for'
    } elseif ($err -notmatch 'Unrecognized escape sequence') {
        Fail "the recorded L5 artifact was refused, but not as an invalid escape: $err"
    } elseif ($firstIllegal -lt 0) {
        Fail 'the recorded L5 artifact holds no illegal escape, so its refusal is not the L5 defect'
    } elseif ($broken.Substring($firstIllegal, 2) -ne '\P') {
        Fail "the first illegal escape in the recorded L5 artifact is [$($broken.Substring($firstIllegal, 2))], not the [\P] of the raw module path"
    } else {
        $reported = if ($err -match '\((\d+)\)') { $Matches[1] } else { 'no offset' }
        Pass "the recorded L5 artifact is refused as an unrecognized escape - the first illegal escape is the [\P] at char $firstIllegal, parser reported offset $reported"
    }
    # The recorded input, taken out of the broken document by its delimiters rather than re-typed.
    if ($broken -match '"provenance": "(.*?)", "unmatched"') {
        $recoveredPath = $Matches[1]
        Pass "recorded provenance recovered from the broken artifact ($($recoveredPath.Length) chars)"
    } else {
        Fail 'the broken artifact holds no provenance field, so there is no recorded input to replay'
    }
}

if ($docs.ContainsKey('l5') -and $recoveredPath) {
    $err = Get-ParseError $docs['l5']
    if ($err) {
        Fail "the new document built from the L5 provenance still does not parse: $err"
    } else {
        Pass 'the new document built from the L5 provenance parses'
        $got = (ConvertFrom-Json $docs['l5']).third_party[0].provenance
        $a = [Text.Encoding]::UTF8.GetBytes($got)
        $b = [Text.Encoding]::UTF8.GetBytes($recoveredPath)
        if ((Compare-Object $a $b -SyncWindow 0) -or $a.Length -ne $b.Length) {
            Fail "the path read back differs from the recorded input: [$got] vs [$recoveredPath]"
        } else {
            Pass 'the path reads back byte-identical to the one L5 recorded'
        }
    }
}

# --- 4. every hostile class, through the same parser. The literal is this check's own, because there is
# no recording of a path holding a quote, a tab and a control byte - only the L5 path is a recorded input.
$hostile = 'C:\Program Files\ready-at-dawn\bin\say "hi"' + [char]9 + 'a' + [char]1 + 'b caf' + [char]0xE9
if ($docs.ContainsKey('hostile')) {
    $err = Get-ParseError $docs['hostile']
    if ($err) {
        Fail "the hostile document does not parse: $err"
    } else {
        Pass 'the hostile document parses'
        $doc = ConvertFrom-Json $docs['hostile']
        $got = $doc.third_party[0].provenance
        $a = [Text.Encoding]::UTF8.GetBytes($got)
        $b = [Text.Encoding]::UTF8.GetBytes($hostile)
        if ((Compare-Object $a $b -SyncWindow 0) -or $a.Length -ne $b.Length) {
            Fail "the hostile path read back differs: [$got]"
        } else {
            Pass 'backslash, quote, tab, 0x01 and a UTF-8 e-acute all read back byte-identical'
        }
        $reason = 'quote " tab ' + [char]9 + ' bell ' + [char]7 + ' done'
        if ($doc.reason -ne $reason) { Fail "the reason field read back as [$($doc.reason)]" }
        else { Pass 'the free-text reason field reads back byte-identical too' }
    }
}

# --- 5. the oracle can say no. Arm 3 and 4 are only worth their green if the parser refuses the
# unescaped form of the very same document, so it is produced here and handed to the same parser.
if ($docs.ContainsKey('l5')) {
    $bypassed = $docs['l5'] -replace '\\\\', '\'
    $err = Get-ParseError $bypassed
    if (-not $err) {
        Fail 'MUTANT: the same document with its backslash escapes undone still parses - this check cannot fail'
    } elseif ($err -notmatch 'Unrecognized escape sequence') {
        Fail "MUTANT: the unescaped document was refused for another reason: $err"
    } else {
        Pass 'MUTANT: the same document with the escaping bypassed is refused, as L5 was'
    }
}

# --- 6. the documents the cap sweep pins everything else to, through the real parser.
foreach ($name in @('sweep', 'sweep-small')) {
    if ($docs.ContainsKey($name)) {
        $err = Get-ParseError $docs[$name]
        if ($err) {
            Fail "the $name reference document does not parse: $err"
        } else {
            $doc = ConvertFrom-Json $docs[$name]
            if ($doc.refused) { Fail "the $name reference document is a refusal, so the sweep's complete arm is vacuous" }
            elseif (-not $doc.verdict) { Fail "the $name reference document carries no verdict" }
            else { Pass "the $name reference document parses and is a report, not a refusal ($($docs[$name].Length) chars)" }
        }
    }
}

if ($docs.ContainsKey('refusal')) {
    $err = Get-ParseError $docs['refusal']
    if ($err) {
        Fail "the COULD-NOT-ASK document does not parse: $err"
    } else {
        $doc = ConvertFrom-Json $docs['refusal']
        $keys = ($doc.PSObject.Properties.Name | Sort-Object) -join ','
        if ($keys -ne 'pass,refused,verdict,writer') {
            Fail "the COULD-NOT-ASK document has keys [$keys]"
        } elseif ($doc.verdict -ne 'COULD-NOT-ASK') {
            Fail "the COULD-NOT-ASK document says verdict [$($doc.verdict)]"
        } elseif (-not $doc.refused) {
            Fail 'the COULD-NOT-ASK document does not set refused, which is what the harness reads first'
        } elseif ($doc.PSObject.Properties.Name -contains 'expected_rows') {
            Fail 'the COULD-NOT-ASK document manufactures an expected_rows denominator'
        } else {
            Pass 'the COULD-NOT-ASK document parses, refuses by name, and declares no denominator'
        }
    }
}

Write-Host "CHECKS $checked"
# Tools/gate.ps1 reads this line and nothing else, so without it a green run of
# this check renders NOT-RUN ("exited 0 but asserted nothing") - which is the right verdict for a
# script that asserted nothing and the wrong one for this. The count is this script's own checks;
# test_sc7.exe's assertions are its own gate row, and one number must not stand for two runs.
Write-Host "GATE-RESULT checked=$checked subject=sc7-json-escape+overflow-sweep"
if ($failed) { exit 1 }
exit 0
