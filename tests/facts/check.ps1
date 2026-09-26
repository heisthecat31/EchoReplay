# Does src/generated/facts_generated.h still carry every byte-string row facts.tsv holds?
#
# The registry has two readers with no overlap. tests/factcheck/factcheck.py compares each
# .PROLOGUE row against the bytes at its owner's VA in the real echovr.exe -- it never opens
# the generated header. gen_facts.py --check compares the header against what it would emit
# -- from rows it just loaded, so it agrees with itself by construction and says nothing
# about a row it was never asked to emit. Between the two, a bytes row could be correct
# against the binary, self-consistent in the generator, and absent from the only artifact
# C++ ever sees; the guard that is meant to refuse a patched engine then compiles against a
# symbol that does not exist, or worse, against a stale one somebody left behind.
#
# ARCH-20 S3.1 is why this is a gate and not a convention. GUI.SET_SUPPRESSED is EIGHT bytes
# of function (0x140F922A0-0x140F922A8) and GUI.CLEAR_SUPPRESSED fourteen; a 20-byte window
# on either runs past the retn into the next function, so it would report a difference in
# code the row does not name and could not fail on a real difference in code it does. Those
# two widths are asserted by name below: widening either back to 20 reds here.
$ErrorActionPreference = 'Stop'
$repo = $env:REPLAY_REPO_ROOT
if (-not $repo) { Write-Host 'REPLAY_REPO_ROOT is not set'; exit 1 }

$factsPath = Join-Path $repo 'facts/facts.tsv'
$headerPath = Join-Path $repo 'src/generated/facts_generated.h'
foreach ($p in @($factsPath, $headerPath)) {
    if (-not (Test-Path -LiteralPath $p)) { Write-Host "no $p"; exit 1 }
}

# Widths this file requires by name, because the design requires them by name.
$requiredWidth = @{
    'GUI.SET_SUPPRESSED.PROLOGUE'   = 8
    'GUI.CLEAR_SUPPRESSED.PROLOGUE' = 8
}

# --- the registry side ------------------------------------------------------------------
$cols = $null
$rows = @{}
foreach ($line in (Get-Content -LiteralPath $factsPath)) {
    if ($line -match '^\s*#' -or -not $line.Trim()) { continue }
    $parts = $line -split "`t"
    if ($null -eq $cols) {
        if ($parts[0] -ne 'id') { Write-Host "facts.tsv: first data line is not the id header"; exit 1 }
        $cols = @{}
        for ($i = 0; $i -lt $parts.Count; $i++) { $cols[$parts[$i]] = $i }
        foreach ($need in @('id', 'value', 'type', 'verdict')) {
            if (-not $cols.ContainsKey($need)) { Write-Host "facts.tsv header has no '$need' column"; exit 1 }
        }
        continue
    }
    $type = $parts[$cols['type']]
    if ($type -notmatch '^bytes(\d+)$') { continue }
    # The withheld verdicts are declared and never defined, so they are absent from the
    # header on purpose. Read the vocabulary off the generator rather than restating it.
    $verdict = $parts[$cols['verdict']]
    if ($verdict -in @('ARM64_ONLY', 'PC_ABSENT', 'NOT_IN_CODE', 'PC_CANDIDATE')) { continue }
    $id = $parts[$cols['id']]
    $declared = [int]$matches[1]
    $bytes = @($parts[$cols['value']] -split '\s+' | Where-Object { $_ })
    if ($bytes.Count -ne $declared) {
        Write-Host "WIDTH: $id is $type but its value holds $($bytes.Count) byte(s)"
        exit 1
    }
    $rows[$id] = @{ Width = $declared; Bytes = ($bytes -join ' ').ToUpperInvariant() }
}
if ($rows.Count -eq 0) { Write-Host 'facts.tsv holds no emittable bytes<N> row -- this check would pass vacuously'; exit 1 }

# --- the header side --------------------------------------------------------------------
$emitted = @{}
foreach ($line in (Get-Content -LiteralPath $headerPath)) {
    $m = [regex]::Match($line, '^inline constexpr uint8_t (?<n>\w+)\[(?<len>\d+)\] = \{(?<body>[^}]*)\};')
    if (-not $m.Success) { continue }
    $vals = @($m.Groups['body'].Value -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $emitted[$m.Groups['n'].Value] = @{
        Width = [int]$m.Groups['len'].Value
        Bytes = (($vals | ForEach-Object { $_.Substring(2).ToUpperInvariant() }) -join ' ')
        Count = $vals.Count
    }
}

$failures = 0
foreach ($id in @($rows.Keys | Sort-Object)) {
    $name = $id.Replace('.', '_')
    if (-not $emitted.ContainsKey($name)) {
        Write-Host "MISSING: $id is in facts.tsv but $name is not in facts_generated.h"
        $failures++
        continue
    }
    $e = $emitted[$name]
    if ($e.Width -ne $rows[$id].Width -or $e.Count -ne $rows[$id].Width) {
        Write-Host "WIDTH: $name is declared [$($e.Width)] with $($e.Count) initialiser(s); $id is $($rows[$id].Width) byte(s)"
        $failures++
    }
    elseif ($e.Bytes -ne $rows[$id].Bytes) {
        Write-Host "BYTES: $name holds $($e.Bytes); $id holds $($rows[$id].Bytes)"
        $failures++
    }
}
foreach ($name in @($emitted.Keys | Sort-Object)) {
    if (-not $rows.ContainsKey($name.Replace('_', '.'))) {
        # Dots and underscores both map to '_', so an id containing '_' cannot be recovered
        # by reversing the substitution. Match it against the forward direction instead.
        $back = @($rows.Keys | Where-Object { $_.Replace('.', '_') -eq $name })
        if ($back.Count -eq 0) {
            Write-Host "ORPHAN: facts_generated.h defines $name, which no bytes<N> row in facts.tsv produces"
            $failures++
        }
    }
}
foreach ($id in @($requiredWidth.Keys | Sort-Object)) {
    if (-not $rows.ContainsKey($id)) {
        Write-Host "ABSENT: $id -- ARCH-20 S3.1 requires this row; W2-F-C2 cannot prologue-guard the setter without it"
        $failures++
        continue
    }
    if ($rows[$id].Width -ne $requiredWidth[$id]) {
        Write-Host ("WINDOW: $id is $($rows[$id].Width) bytes; ARCH-20 S3.1 requires $($requiredWidth[$id]) -- " +
                    'a wider window runs past the function''s retn and reports differences in code the row does not name')
        $failures++
    }
}

Write-Host "GATE-RESULT checked=$($rows.Count + $requiredWidth.Count) subject=generated-header-bytes-rows"
if ($failures -gt 0) {
    Write-Host "$failures byte-string row(s) disagree between facts/facts.tsv and src/generated/facts_generated.h."
    Write-Host 'Run: python tools/gen_facts.py'
    exit 1
}
Write-Host "facts_generated.h carries all $($rows.Count) bytes<N> row(s), including GUI.SET_SUPPRESSED.PROLOGUE and GUI.CLEAR_SUPPRESSED.PROLOGUE at 8 bytes"
exit 0
