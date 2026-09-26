# Drives tools\watch_replay.ps1 in -DryRun against a throwaway capture folder and a throwaway "game"
# folder. Nothing is started and the real install is never looked at.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\launcher\check.ps1
#   ... -Script <copy>   aims the same assertions at a mutated copy
param([string]$Script = (Join-Path $PSScriptRoot '..\..\tools\watch_replay.ps1'))

$ErrorActionPreference = 'Stop'
$failures = 0
$checks = 0

function Check([string]$what, [bool]$ok) {
    $script:checks++
    if ($ok) { Write-Host "  ok    $what" } else { $script:failures++; Write-Host "  FAIL  $what" -ForegroundColor Red }
}

$root = Join-Path $env:TEMP ("watch_replay_check_" + $PID)
$captures = Join-Path $root 'captures'
$empty = Join-Path $root 'empty'
$game = Join-Path $root 'game'
$noPlugin = Join-Path $root 'game_no_plugin'
$noExe = Join-Path $root 'game_no_exe'
$noLoader = Join-Path $root 'game_no_loader'
foreach ($d in $captures, $empty, "$game\bin\win10\plugins", "$noPlugin\bin\win10", "$noExe\bin\win10\plugins", "$noLoader\bin\win10\plugins") {
    New-Item -ItemType Directory -Force $d | Out-Null
}
Set-Content "$game\bin\win10\echovr.exe" 'not a game'
Set-Content "$game\bin\win10\dbgcore.dll" 'not a loader'
Set-Content "$game\bin\win10\plugins\EchoReplay.dll" 'not a plugin'
Set-Content "$noPlugin\bin\win10\echovr.exe" 'not a game'
Set-Content "$noPlugin\bin\win10\dbgcore.dll" 'not a loader'
Set-Content "$noLoader\bin\win10\echovr.exe" 'not a game'
Set-Content "$noLoader\bin\win10\plugins\EchoReplay.dll" 'not a plugin'
Set-Content "$noExe\bin\win10\plugins\EchoReplay.dll" 'not a plugin'

# Oldest to newest: arena, an unknown level, combat (dyson).
$arena = '2026-01-01_10-00-00_576ED3F8428EBC4B_000000007E1CAF73.echodemo'
$unknown = '2026-01-02_10-00-00_1111111111111111_00000000FFFFFFFF.echodemo'
$combat = '2026-01-03_10-00-00_43E2DA7914642604_3D5C3976578A321A.echodemo'
$age = 3
foreach ($name in $arena, $unknown, $combat) {
    $file = Join-Path $captures $name
    Set-Content $file ('x' * 2048)
    (Get-Item $file).LastWriteTime = (Get-Date).AddDays(-$age)
    $age--
}

# A process name nothing on this machine has, so a real game running elsewhere cannot red this.
$nobody = 'watch_replay_check_no_such_process'

function Run([string[]]$arguments) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $Script @arguments 2>&1 | Out-String
    return @{ Text = $out; Exit = $LASTEXITCODE }
}

$common = @('-CaptureDir', $captures, '-ImportDir', $empty, '-GameDir', $game, '-GameProcessName', $nobody, '-DryRun')
$tail = '-lobbyteam 2 -loginhost 127.0.0.1 -confighost 127.0.0.1 -radserverdbhost 127.0.0.1 -servermanagerhost 127.0.0.1'

Write-Host 'latest'
$r = Run ($common + '-Latest')
Check 'exits 0' ($r.Exit -eq 0)
Check 'opens the newest capture' ($r.Text.Contains("ECHOREPLAY_OPEN=$(Join-Path $captures $combat)"))
Check 'derives the combat level from the capture' ($r.Text.Contains("echovr.exe`" -region usc -level mpl_combat_dyson -gametype echo_combat $tail"))
Check 'no rlay-only switch on the line' (-not ($r.Text -match 'echovr\.exe"[^\r\n]* -(offline|windowed)(\s|$)'))
Check 'says combat is not launch-tested' ($r.Text.Contains('only mpl_arena_a has been launch-tested'))
Check 'runs from bin\win10' ($r.Text.Contains("working directory : $game\bin\win10"))
Check 'says nothing was started' ($r.Text.Contains('DRY RUN - nothing was started'))

Write-Host 'list and pick'
$r = Run ($common + @('-Pick', '3'))
Check 'exits 0' ($r.Exit -eq 0)
$iCombat = $r.Text.IndexOf("[ 1]"); $iArena = $r.Text.IndexOf("[ 3]")
Check 'lists newest first' ($r.Text -match "\[ 1\].*$([regex]::Escape($combat))" -and $r.Text -match "\[ 3\].*$([regex]::Escape($arena))" -and $iCombat -lt $iArena)
Check 'shows size and date' ($r.Text -match '\[ 1\]\s+\d{4}-\d{2}-\d{2} \d{2}:\d{2}\s+\d+ KB')
Check 'pick 3 opens the oldest, the arena one' ($r.Text.Contains("ECHOREPLAY_OPEN=$(Join-Path $captures $arena)"))
Check 'the proven arena command line, verbatim' ($r.Text.Contains("echovr.exe`" -region usc -level mpl_arena_a -gametype echo_arena $tail"))

$r = Run ($common + @('-Pick', '2'))
Check 'an unknown level starts in the proven arena and says so' ($r.Text.Contains('-level mpl_arena_a -gametype echo_arena') -and $r.Text.Contains('is not one this script knows by name'))

$r = Run ($common + @('-Pick', '4'))
Check 'a number off the list is refused' ($r.Exit -eq 1 -and $r.Text.Contains('4 is not on the list') -and -not $r.Text.Contains('DRY RUN'))

Write-Host 'path'
$r = Run (@('-Path', (Join-Path $captures $arena)) + $common)
Check '-Path opens that file' ($r.Exit -eq 0 -and $r.Text.Contains("ECHOREPLAY_OPEN=$(Join-Path $captures $arena)"))
$r = Run (@('-Path', (Join-Path $captures 'absent.echodemo')) + $common)
Check 'a missing file is refused' ($r.Exit -eq 1 -and $r.Text.Contains('There is no file at') -and -not $r.Text.Contains('DRY RUN'))

Write-Host 'legacy replays (FORMATS-P6)'
# A Spark folder with one legacy replay newer than every capture, and one older, plus a file that
# is neither; the capture folder gets a .tape of its own.
$spark = Join-Path $root 'spark'
New-Item -ItemType Directory -Force $spark | Out-Null
$sparkNew = Join-Path $spark 'rec_2026-01-04_10-00-00.echoreplay'
$sparkOld = Join-Path $spark 'rec_2025-12-01_10-00-00.echoreplay'
$tape = Join-Path $captures 'sample.tape'
foreach ($f in $sparkNew, $sparkOld, $tape, (Join-Path $spark 'notes.txt')) { Set-Content $f ('z' * 4096) }
(Get-Item $sparkNew).LastWriteTime = (Get-Date).AddHours(-1)
(Get-Item $sparkOld).LastWriteTime = (Get-Date).AddDays(-30)
(Get-Item $tape).LastWriteTime = (Get-Date).AddDays(-20)
$legacyCommon = @('-CaptureDir', $captures, '-ImportDir', $spark, '-GameDir', $game, '-GameProcessName', $nobody, '-DryRun')
$r = Run ($legacyCommon + @('-Pick', '1'))
Check 'lists the Spark folder beside the captures, newest first' ($r.Exit -eq 0 -and $r.Text -match "\[ 1\].*rec_2026-01-04_10-00-00\.echoreplay" -and $r.Text -match "\[ 6\].*rec_2025-12-01_10-00-00\.echoreplay")
Check 'lists a .tape in the capture folder' ($r.Text -match "\[ 5\].*sample\.tape")
Check 'badges a legacy row' ($r.Text -match "\[ 1\].*LEGACY - converts on open")
Check 'does not list other files' (-not $r.Text.Contains('notes.txt'))
Check 'a picked legacy replay is handed to the plugin as-is' ($r.Text.Contains("ECHOREPLAY_OPEN=$sparkNew"))
Check 'and starts in the arena, saying it converts on open' ($r.Text.Contains('-level mpl_arena_a -gametype echo_arena') -and $r.Text.Contains('legacy replay: the plugin converts it'))
$r = Run (@('-Path', $tape) + $legacyCommon)
Check '-Path takes a .tape' ($r.Exit -eq 0 -and $r.Text.Contains("ECHOREPLAY_OPEN=$tape"))
$r = Run @('-CaptureDir', $captures, '-ImportDir', $captures, '-GameDir', $game, '-GameProcessName', $nobody, '-DryRun', '-Pick', '1')
Check 'the capture folder named as the import folder is listed once' (([regex]::Matches($r.Text, 'sample\.tape')).Count -eq 1)
$r = Run @('-CaptureDir', $empty, '-ImportDir', $spark, '-GameDir', $game, '-GameProcessName', $nobody, '-DryRun', '-Latest')
Check 'the Spark folder alone is enough to pick from' ($r.Exit -eq 0 -and $r.Text.Contains("ECHOREPLAY_OPEN=$sparkNew"))
Remove-Item $tape

Write-Host 'refusals'
$r = Run @('-CaptureDir', $empty, '-ImportDir', $empty, '-GameDir', $game, '-GameProcessName', $nobody, '-DryRun', '-Latest')
Check 'an empty capture folder is refused' ($r.Exit -eq 1 -and $r.Text.Contains('There are no replays in'))
# This check is itself a powershell process, so "powershell" is a game that is certainly running.
$r = Run @('-CaptureDir', $captures, '-ImportDir', $empty, '-GameDir', $game, '-GameProcessName', 'powershell', '-DryRun', '-Latest')
Check 'a running game is refused, before anything else is decided' ($r.Exit -eq 1 -and $r.Text.Contains('already running') -and -not $r.Text.Contains('DRY RUN'))
$r = Run @('-CaptureDir', $captures, '-ImportDir', $empty, '-GameDir', $noExe, '-GameProcessName', $nobody, '-DryRun', '-Latest')
Check 'a game folder with no echovr.exe is refused' ($r.Exit -eq 1 -and $r.Text.Contains('echovr.exe is not at'))
$r = Run @('-CaptureDir', $captures, '-ImportDir', $empty, '-GameDir', $noLoader, '-GameProcessName', $nobody, '-DryRun', '-Latest')
Check 'a game without EchoLoader is refused' ($r.Exit -eq 1 -and $r.Text.Contains('EchoLoader is not installed'))
$r = Run @('-CaptureDir', $captures, '-ImportDir', $empty, '-GameDir', $noPlugin, '-GameProcessName', $nobody, '-DryRun', '-Latest')
Check 'a missing plugin is a warning, not a copy and not a refusal' ($r.Exit -eq 0 -and $r.Text.Contains('WARNING: the replay plugin is not installed') -and -not (Test-Path "$noPlugin\bin\win10\plugins"))

Write-Host 'old plugin name'
$legacyGame = Join-Path $root 'game_legacy'
$legacyOnly = Join-Path $root 'game_legacy_only'
foreach ($g in $legacyGame, $legacyOnly) {
    New-Item -ItemType Directory -Force "$g\bin\win10\plugins" | Out-Null
    Set-Content "$g\bin\win10\echovr.exe" 'not a game'
    Set-Content "$g\bin\win10\dbgcore.dll" 'not a loader'
    Set-Content "$g\bin\win10\plugins\probe_replay.dll" 'old plugin'
}
Set-Content "$legacyGame\bin\win10\plugins\EchoReplay.dll" 'not a plugin'
$legacy = "$legacyGame\bin\win10\plugins\probe_replay.dll"
$disabled = "$legacyGame\bin\win10\plugins_disabled\probe_replay.dll"

$r = Run @('-CaptureDir', $captures, '-ImportDir', $empty, '-GameDir', $legacyOnly, '-GameProcessName', $nobody, '-DryRun', '-Latest')
Check 'an old plugin alone is a warning and is left in place' ($r.Exit -eq 0 -and $r.Text.Contains('earlier version of this plugin under its old name') -and (Test-Path "$legacyOnly\bin\win10\plugins\probe_replay.dll"))
$r = Run @('-CaptureDir', $captures, '-ImportDir', $empty, '-GameDir', $legacyGame, '-GameProcessName', $nobody, '-DryRun', '-Latest')
Check 'a dry run only says it would move the old plugin' ($r.Exit -eq 0 -and $r.Text.Contains('would be moved') -and (Test-Path $legacy) -and -not (Test-Path $disabled))
# Not a dry run: the move happens before the launch, and the launch of this text-file echovr.exe fails.
$ErrorActionPreference = 'Continue'   # the failed launch writes to stderr
$r = Run @('-CaptureDir', $captures, '-ImportDir', $empty, '-GameDir', $legacyGame, '-GameProcessName', $nobody, '-Latest')
$ErrorActionPreference = 'Stop'
Check 'beside EchoReplay.dll the old plugin is moved out of plugins' ($r.Text.Contains('Moved the old plugin') -and -not (Test-Path $legacy) -and (Test-Path $disabled) -and (Test-Path "$legacyGame\bin\win10\plugins\EchoReplay.dll"))

Write-Host 'source'
$source = Get-Content -Raw $Script
Check 'never names config.json' (-not ($source -match 'config\.json'))
Check 'no network cmdlet or class' (-not ($source -match 'Invoke-WebRequest|Invoke-RestMethod|Net\.WebClient|Net\.Http|Start-BitsTransfer'))
Check 'ECHOREPLAY_OPEN is set on the child, never on this process' ($source -match "EnvironmentVariables\['ECHOREPLAY_OPEN'\]" -and -not ($source -match '\$env:ECHOREPLAY_OPEN\s*='))

Remove-Item -Recurse -Force $root
Write-Host ''
Write-Host "$checks checks, $failures failed"
exit $(if ($failures -eq 0) { 0 } else { 1 })
