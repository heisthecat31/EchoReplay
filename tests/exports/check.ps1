# Pins echoreplay.dll's export table.
#
# The plugin must export exactly Update and RadPluginShutdown. The engine GetProcAddress-es provider
# method names against our HMODULE uncached, every frame, so an export named Users or ProviderID makes us
# answer for an ABI we do not implement - and a no-op returning 0 is worse than a missing export, because
# it is indistinguishable from a valid provider id.
$ErrorActionPreference = 'Stop'
$repo = $env:REPLAY_REPO_ROOT
$build = $env:REPLAY_BUILD_DIR
if (-not $repo -or -not $build) { Write-Host 'REPLAY_REPO_ROOT / REPLAY_BUILD_DIR are not set'; exit 1 }

$toolchain = Join-Path $build 'toolchain.txt'
if (-not (Test-Path -LiteralPath $toolchain)) {
    Write-Host "no $toolchain - run tools\build.ps1 first so the toolchain is resolved once, not guessed here"
    exit 1
}
$dumpbinLine = Get-Content -LiteralPath $toolchain | Where-Object { $_ -like 'dumpbin=*' } | Select-Object -First 1
if (-not $dumpbinLine) { Write-Host "no dumpbin= line in $toolchain"; exit 1 }
$dumpbin = $dumpbinLine -replace '^dumpbin=', ''
if (-not (Test-Path -LiteralPath $dumpbin)) { Write-Host "dumpbin not found: $dumpbin"; exit 1 }

# ★ BOTH BINARIES, and the second is not a copy of the first's rule. probe_replay.dll is the product
# plus one observer translation unit; it carries the same provider-name hazard,
# because the engine GetProcAddress-es method names against whichever of our modules is loaded, and it
# is the only one loaded on a wave-2 launch. A pin that named echoreplay alone would leave the DLL
# every wave-2 claim is made from unpinned.
$script:gateChecked = 0
$script:gateExports = 0
$script:gateRules = 0

function Test-ExportSurface([string]$dllName) {
    $dll = Join-Path (Join-Path $build 'bin') $dllName
    if (-not (Test-Path -LiteralPath $dll)) { Write-Host "no $dll"; return $false }

    # --- the allowed set: what this packet declares may appear
    $allowed = @(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'allowed.txt') |
        ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') })
    if ($allowed.Count -eq 0) { Write-Host 'allowed.txt lists no names; a list this check cannot read is not a check'; return $false }

    # The CNSProvider method names the engine resolves on our module.
    $forbidden = @('AllowGuests', 'Destroy', 'InitGlobals', 'MicDestroy', 'MicStart', 'MicStop', 'ProviderID',
        'RadPluginShutdown', 'Social', 'Update', 'UserProviderID', 'Users', 'VoipDestroyDecoder', 'VoipDestroyEncoder')
    $forbidden = @($forbidden | Where-Object { $_ -ne 'Update' -and $_ -ne 'RadPluginShutdown' })

    # A name in both lists is a contradiction, and it is how the deny-list was made inert: allow-list
    # membership used to suppress the deny-list check entirely.
    $contradiction = @($allowed | Where-Object { $forbidden -contains $_ })
    if ($contradiction.Count -gt 0) {
        Write-Host ("allowed.txt permits provider method name(s) that section 2 F8b forbids: " + ($contradiction -join ', '))
        return $false
    }

    # dumpbin's export rows are "ordinal hint RVA name". A DLL with no export table has no such section, which
    # is the correct state until W1-D lands the slot claim - told apart from a parse failure by the header.
    $actual = @()
    $sawTable = $false
    $inTable = $false
    foreach ($line in (& $dumpbin /nologo /exports $dll)) {
        if ($line -match '^\s*ordinal\s+hint\s+RVA\s+name') { $sawTable = $true; $inTable = $true; continue }
        if (-not $inTable) { continue }
        if ($line -match '^\s*Summary') { break }
        if ($line -match '^\s*(\d+)\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $actual += $matches[2] }
    }
    if ($sawTable -and $actual.Count -eq 0) { Write-Host 'dumpbin printed an export table this check could not parse'; return $false }

    if ($actual.Count -eq 0) { Write-Host ("{0}: exports found: (none - the DLL has no export table)" -f $dllName) }
    else { Write-Host ("{0}: exports found: {1}" -f $dllName, ($actual -join ', ')) }

    # The count is over the SUBJECT and the rules applied to it: every export the DLL actually publishes is
    # tested against both lists, and every list name is tested against the export table. It used to be
    # allowed.Count + forbidden.Count alone - a census of this check's own reference lists, in which adding a
    # surplus export to the DLL could not move the number by one.
    $script:gateChecked += ($actual.Count + $allowed.Count + $forbidden.Count)
    $script:gateExports += $actual.Count
    $script:gateRules += ($allowed.Count + $forbidden.Count)

    $bad = $false
    # The two rules are evaluated independently. Being on the allow-list does not exempt a name from the
    # deny-list, or a single edit to allowed.txt would disarm the control that matters most.
    foreach ($n in @($actual | Where-Object { $forbidden -contains $_ } | Sort-Object)) {
        Write-Host "FORBIDDEN EXPORT: $n - a CNSProvider method the ENGINE LOOKS UP against our module; exporting it makes us answer for an ABI we do not implement (section 2 F8b)"
        $bad = $true
    }
    foreach ($n in @($actual | Where-Object { $allowed -notcontains $_ -and $forbidden -notcontains $_ } | Sort-Object)) {
        Write-Host "UNEXPECTED EXPORT: $n - not in the allowed set (section 2 F8b: export exactly Update and RadPluginShutdown)"
        $bad = $true
    }
    if ($bad) {
        Write-Host 'Keep helpers static or in an anonymous namespace. Adding a name to allowed.txt needs the design to say so first.'
        return $false
    }

    $present = @($allowed | Where-Object { $actual -contains $_ } | Sort-Object)
    $pending = @($allowed | Where-Object { $actual -notcontains $_ } | Sort-Object)
    if ($present.Count -gt 0) { Write-Host ("allowed and present: " + ($present -join ', ')) }
    if ($pending.Count -gt 0) { Write-Host ("allowed but not yet exported (W1-D): " + ($pending -join ', ')) }
    Write-Host ("{3}: export surface clean: {0} export(s); none outside the allowed set; none of the {1} forbidden provider names ({2})" -f `
            $actual.Count, $forbidden.Count, ($forbidden -join ", "), $dllName)
    return $true
}

$subjects = @('echoreplay.dll', 'probe_replay.dll')
$clean = $true
foreach ($name in $subjects) {
    if (-not (Test-ExportSurface $name)) { $clean = $false }
}
Write-Host ("GATE-RESULT checked={0} subject=export-surface-of-{1} exports={2} rules={3}" -f `
    $script:gateChecked, ($subjects -join '+'), $script:gateExports, $script:gateRules)
if (-not $clean) { exit 1 }
exit 0
