<#
.SYNOPSIS
  Opens an Echo VR replay (.echodemo, or a legacy .echoreplay / .tape) in the game.

.DESCRIPTION
  With no arguments it lists the captures in Documents\EchoReplay and the legacy replays in Spark's
  folder (Documents\Spark\replays), newest first, and asks which one to watch. A legacy replay is
  converted by the plugin when the game opens it, and the copy is kept for next time.
  It starts echovr.exe offline with the replay plugin told which file to open. rlay.dll is not needed:
  the plugin applies the offline patch itself in a replay session, and skips it where rlay already did.
  It sets ECHOREPLAY_OPEN for the game process only, touches no file, and uses no network.

  The game is found through -GameDir, REPLAY_ECHOVR_EXE, ECHOVR_DIR, or the Oculus libraries registered
  on this PC, in that order. Captures are listed from -CaptureDir, else the plugin's default folder
  %USERPROFILE%\Documents\EchoReplay.

.EXAMPLE
  watch_replay.bat                 pick from a list
  watch_replay.bat -Latest         the newest capture
  watch_replay.bat -Path C:\some\capture.echodemo
  watch_replay.bat -Path C:\some\match.echoreplay
  watch_replay.bat -Latest -DryRun show what would be started, start nothing
#>
[CmdletBinding()]
param(
    [string]$Path,
    [switch]$Latest,
    [int]$Pick = 0,
    [int]$Count = 15,
    [string]$GameDir,
    [string]$CaptureDir,
    # Where legacy replays are listed from as well; Spark's own default folder. The plugin's
    # `import.dirs` config row is the same list for the in-game browser.
    [string]$ImportDir = (Join-Path $env:USERPROFILE 'Documents\Spark\replays'),
    [switch]$DryRun,
    # Forwarded to the game as -syncinterval <n> when given. Unset means the game keeps whatever
    # its own config says, which is how the owner runs it; 0 uncaps the frame rate for a
    # measurement run and changes nothing else.
    [int]$SyncInterval = -1,
    # The process whose presence means "the game is already running". Only tests change it.
    [string]$GameProcessName = 'echovr'
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'echovr_path.ps1')
$gameExe = Find-EchoVrExe $GameDir

# The plugin's default capture folder (src/bootstrap/paths.cpp ResolveCaptureDir). A `capture.dir`
# set in the plugin's config is not read here: pass that folder with -CaptureDir.
if (-not $CaptureDir) { $CaptureDir = Join-Path $env:USERPROFILE 'Documents\EchoReplay' }

$CaptureExtension = '.echodemo'
$LegacyExtensions = @('.echoreplay', '.tape')

function Test-Legacy([string]$file) { return $LegacyExtensions -contains [IO.Path]::GetExtension($file).ToLowerInvariant() }

# The argument form LAUNCH-SPAWNED-SEED / LAUNCH-BIND-FIX rendered recorded players with.
$ProvenLevel = 'mpl_arena_a'
$ProvenGametype = 'echo_arena'

# Level hash (CSymbol64 of the name, as the capture records it) -> the -level / -gametype to start in.
$KnownLevels = @{
    '576ED3F8428EBC4B' = @{ Level = 'mpl_arena_a';           Gametype = 'echo_arena' }
    'DF5CA7B7DFA383D4' = @{ Level = 'mpl_combat_fission';    Gametype = 'echo_combat' }
    '43E2DA7A0C623A19' = @{ Level = 'mpl_combat_gauss';      Gametype = 'echo_combat' }
    'DF4AABA6CEA689C9' = @{ Level = 'mpl_combat_pebbles';    Gametype = 'echo_combat' }
    '42670F2BED45703C' = @{ Level = 'mpl_combat_combustion'; Gametype = 'echo_combat' }
    '43E2DA7914642604' = @{ Level = 'mpl_combat_dyson';      Gametype = 'echo_combat' }
}

function Say([string]$text) { Write-Host $text }

function Stop-WithReason([string]$text) {
    Write-Host "STOPPED: $text" -ForegroundColor Red
    exit 1
}

function Format-Size([long]$bytes) {
    if ($bytes -ge 1MB) { return ('{0:N1} MB' -f ($bytes / 1MB)) }
    return ('{0:N0} KB' -f [Math]::Ceiling($bytes / 1KB))
}

function Format-Duration([double]$seconds) {
    $span = [TimeSpan]::FromSeconds([Math]::Round($seconds))
    return ('{0}:{1:00}' -f [int][Math]::Floor($span.TotalMinutes), $span.Seconds)
}

# Duration, player count and level out of `echodemo info`, when the tool has been built.
function Get-CaptureInfo([string]$file) {
    $info = @{ Duration = $null; Players = $null; LevelHex = $null }
    # A legacy replay has no header to read until the plugin converts it.
    if (Test-Legacy $file) { return $info }
    $tool = Join-Path $PSScriptRoot '..\build\bin\echodemo.exe'
    if (Test-Path $tool) {
        try {
            foreach ($line in (& $tool info $file 2>$null)) {
                if ($line -match '^\s*duration\s*:\s*([\d.]+)\s*s') { $info.Duration = [double]$Matches[1] }
                elseif ($line -match '^\s*players\s*:\s*(\d+)') { $info.Players = [int]$Matches[1] }
                elseif ($line -match '^\s*level / gametype\s*:\s*(\d+)\s*/') {
                    $info.LevelHex = ('{0:X16}' -f [uint64]$Matches[1])
                }
            }
        } catch {
            # A capture the tool cannot read still gets listed, just without the extras.
        }
    }
    if (-not $info.LevelHex) {
        # The recorder names every file <date>_<time>_<level hex>_<gametype hex>.echodemo.
        $name = [IO.Path]::GetFileNameWithoutExtension($file)
        if ($name -match '_([0-9A-Fa-f]{16})_[0-9A-Fa-f]{16}$') { $info.LevelHex = $Matches[1].ToUpper() }
    }
    return $info
}

function Get-Captures {
    $found = @()
    if (Test-Path -LiteralPath $CaptureDir) {
        $found += @(Get-ChildItem -LiteralPath $CaptureDir -File |
                    Where-Object { $_.Extension -eq $CaptureExtension -or (Test-Legacy $_.Name) })
    }
    if ($ImportDir -and (Test-Path -LiteralPath $ImportDir)) {
        $found += @(Get-ChildItem -LiteralPath $ImportDir -File | Where-Object { Test-Legacy $_.Name })
    }
    # The capture folder named again as the import folder is listed once.
    return @($found | Sort-Object FullName -Unique | Sort-Object LastWriteTime -Descending)
}

# ---- which capture -------------------------------------------------------------------------------

$chosen = $null
if ($Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Stop-WithReason "There is no file at $Path." }
    $chosen = (Resolve-Path -LiteralPath $Path).ProviderPath
} else {
    $captures = Get-Captures
    if ($captures.Count -eq 0) {
        Stop-WithReason "There are no replays in $CaptureDir or $ImportDir. Play or spectate a match with the plugin installed and one will appear there."
    }
    if ($Latest) {
        $chosen = $captures[0].FullName
    } else {
        $shown = @($captures | Select-Object -First $Count)
        Say "Replays in $CaptureDir and $ImportDir (newest first):"
        Say ''
        for ($i = 0; $i -lt $shown.Count; ++$i) {
            $c = $shown[$i]
            $info = Get-CaptureInfo $c.FullName
            $extra = ''
            if ($null -ne $info.Duration) { $extra += '  ' + (Format-Duration $info.Duration) }
            if ($null -ne $info.Players) { $extra += ('  {0} player{1}' -f $info.Players, $(if ($info.Players -eq 1) { '' } else { 's' })) }
            if (Test-Legacy $c.Name) { $extra += '  LEGACY - converts on open' }
            Say ('  [{0,2}]  {1}  {2,9}{3}  {4}' -f ($i + 1), $c.LastWriteTime.ToString('yyyy-MM-dd HH:mm'),
                 (Format-Size $c.Length), $extra, $c.Name)
        }
        if ($captures.Count -gt $shown.Count) {
            Say ''
            Say ("  ({0} older replays are not shown; -Count {1} lists them all.)" -f
                 ($captures.Count - $shown.Count), $captures.Count)
        }
        Say ''
        $number = $Pick
        if ($number -eq 0) {
            $answer = Read-Host "Which one? Type a number from 1 to $($shown.Count), or press Enter to cancel"
            if (-not $answer) { Say 'Nothing was started.'; exit 0 }
            if (-not [int]::TryParse($answer, [ref]$number)) { Stop-WithReason "'$answer' is not a number." }
        }
        if ($number -lt 1 -or $number -gt $shown.Count) {
            Stop-WithReason "$number is not on the list; it goes from 1 to $($shown.Count)."
        }
        $chosen = $shown[$number - 1].FullName
    }
}
Say "Replay: $chosen"

# ---- the game ------------------------------------------------------------------------------------

if (Get-Process -Name $GameProcessName -ErrorAction SilentlyContinue) {
    Stop-WithReason "Echo VR is already running. Close it first; the replay is opened when the game starts."
}
if (-not $gameExe) {
    if ($GameDir) { Stop-WithReason "echovr.exe is not at $(Join-Path $GameDir 'bin\win10\echovr.exe'). Pass the Echo VR install folder (the one holding bin\win10) with -GameDir." }
    Stop-WithReason "$script:EchoVrNotFound Or pass the folder with -GameDir."
}
$exe = $gameExe
$binDir = Split-Path -Parent $exe
$plugin = Join-Path $binDir 'plugins\probe_replay.dll'
$loader = Join-Path $binDir 'dbgcore.dll'
if (-not (Test-Path -LiteralPath $loader)) {
    Stop-WithReason "EchoLoader is not installed: $loader is missing. The replay plugin is loaded by EchoLoader's dbgcore.dll in bin\win10; install the loader first."
}
if (-not (Test-Path -LiteralPath $plugin)) {
    Write-Host "WARNING: the replay plugin is not installed ($plugin is missing). The game will start, but no replay will open. Copy build\bin\probe_replay.dll there." -ForegroundColor Yellow
} else {
    $built = Join-Path $PSScriptRoot '..\build\bin\probe_replay.dll'
    if ((Test-Path $built) -and
        (Get-FileHash -LiteralPath $built).Hash -ne (Get-FileHash -LiteralPath $plugin).Hash) {
        Write-Host "WARNING: the installed plugin is not the one built in this folder. To update it, copy $((Resolve-Path $built).ProviderPath) over $plugin." -ForegroundColor Yellow
    } else {
        Say "The replay plugin is installed."
    }
}

$level = $ProvenLevel
$gametype = $ProvenGametype
$levelHex = (Get-CaptureInfo $chosen).LevelHex
if (Test-Legacy $chosen) {
    Say "This is a legacy replay: the plugin converts it when the game opens it (the first time only), then plays it. Legacy replays are all $ProvenLevel, so the game starts there."
} elseif ($levelHex -and $KnownLevels.ContainsKey($levelHex)) {
    $level = $KnownLevels[$levelHex].Level
    $gametype = $KnownLevels[$levelHex].Gametype
    Say "The capture was recorded in $level, so the game starts there."
    if ($level -ne $ProvenLevel) {
        Say "Note: only $ProvenLevel has been launch-tested so far."
    }
} elseif ($levelHex) {
    Say "The capture's level (0x$levelHex) is not one this script knows by name, so the game starts in $ProvenLevel and the plugin moves it to the recorded level."
} else {
    Say "The capture's level could not be read here, so the game starts in $ProvenLevel and the plugin moves it to the recorded level."
}

# Stock switches only. -offline and -windowed are registered by rlay.dll, and without it one unknown
# switch makes the game discard the whole line, pins included. The plugin applies both itself in a
# replay session (ECHOREPLAY_OPEN below is the trigger), with or without rlay.
$gameArgs = @('-region', 'usc',
              '-level', $level, '-gametype', $gametype, '-lobbyteam', '2',
              '-loginhost', '127.0.0.1', '-confighost', '127.0.0.1',
              '-radserverdbhost', '127.0.0.1', '-servermanagerhost', '127.0.0.1')

if ($PSBoundParameters.ContainsKey('SyncInterval')) {
    if ($SyncInterval -lt 0) { Stop-WithReason "-SyncInterval must be 0 or greater." }
    $gameArgs += @('-syncinterval', "$SyncInterval")
}

$inherited = @(Get-ChildItem Env: | Where-Object { $_.Name -like 'ECHOREPLAY_*' -and $_.Name -ne 'ECHOREPLAY_OPEN' })
foreach ($v in $inherited) {
    Write-Host "WARNING: $($v.Name)=$($v.Value) is set in this window and the game will see it. None is needed; close this window and use a new one to run with the defaults." -ForegroundColor Yellow
}

if ($DryRun) {
    Say ''
    Say 'DRY RUN - nothing was started. This is what would run:'
    Say "  working directory : $binDir"
    Say "  command line      : `"$exe`" $($gameArgs -join ' ')"
    Say "  environment       : ECHOREPLAY_OPEN=$chosen  (for the game process only)"
    foreach ($v in $inherited) { Say "  environment       : $($v.Name)=$($v.Value)  (inherited from this window)" }
    exit 0
}

$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $exe
$start.WorkingDirectory = $binDir
$start.Arguments = $gameArgs -join ' '
$start.UseShellExecute = $false
$start.EnvironmentVariables['ECHOREPLAY_OPEN'] = $chosen
$game = [System.Diagnostics.Process]::Start($start)
Say "Echo VR is starting (process $($game.Id)). The replay opens once the arena has loaded; press F8 in the game for the replay panel."
