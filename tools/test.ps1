<#
.SYNOPSIS
    Runs every test program and check script of a build.

.DESCRIPTION
    Reads build\test_targets.txt (written by CMake) and runs each entry: native doctest programs directly,
    check scripts in a child PowerShell with REPLAY_REPO_ROOT and REPLAY_BUILD_DIR set. Nothing is built;
    run tools\build.ps1 first. Checks that need the real game report SKIP lines and pass without it.
    Full output of each entry goes to build\test-logs\<name>.log.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\test.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\test.ps1 -Only audio,import
#>
[CmdletBinding()]
param(
    [string]$BuildDir,
    [string[]]$Only,
    [int]$TimeoutSec = 900
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build' }
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path

$manifest = Join-Path $BuildDir 'test_targets.txt'
if (-not (Test-Path -LiteralPath $manifest)) {
    Write-Host "no $manifest - run tools\build.ps1 first" -ForegroundColor Red
    exit 1
}

$entries = @(Get-Content -LiteralPath $manifest | Where-Object { $_.Trim() } | ForEach-Object {
        $kind, $name, $path = $_ -split '\|', 3
        [pscustomobject]@{ Kind = $kind; Name = $name; Path = $path }
    })
# -File passes "a,b" as one string, so split it here.
if ($Only) { $Only = @($Only | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
if ($Only) { $entries = @($entries | Where-Object { $Only -contains $_.Name }) }
if ($entries.Count -eq 0) {
    Write-Host 'nothing to run' -ForegroundColor Red
    exit 1
}

$logDir = Join-Path $BuildDir 'test-logs'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null

$env:REPLAY_REPO_ROOT = $repoRoot
$env:REPLAY_BUILD_DIR = $BuildDir

$failed = New-Object System.Collections.ArrayList
foreach ($entry in $entries) {
    $label = "$($entry.Kind) $($entry.Name)"
    $log = Join-Path $logDir ("{0}-{1}.log" -f $entry.Kind, $entry.Name)
    if (-not (Test-Path -LiteralPath $entry.Path)) {
        Write-Host ("  MISSING  {0}: {1}" -f $label, $entry.Path) -ForegroundColor Red
        [void]$failed.Add($label)
        continue
    }

    if ($entry.Kind -eq 'native') {
        $file = $entry.Path
        $arguments = ''
    }
    else {
        # Invoked with & rather than -File so a script that ends without `exit` reports its last native
        # exit code, reset to 0 first so nothing earlier leaks in.
        $quoted = $entry.Path -replace "'", "''"
        $file = 'powershell.exe'
        # Start-Process joins an argument array without quoting, so the command line is built whole.
        $arguments = "-NoProfile -ExecutionPolicy Bypass -Command `"`$global:LASTEXITCODE = 0; & '$quoted'; exit `$LASTEXITCODE`""
    }

    $started = Get-Date
    $errLog = "$log.stderr"
    $startArgs = @{
        FilePath               = $file
        WorkingDirectory       = (Split-Path -Parent $entry.Path)
        RedirectStandardOutput = $log
        RedirectStandardError  = $errLog
        NoNewWindow            = $true
        PassThru               = $true
    }
    if ($arguments) { $startArgs.ArgumentList = $arguments }
    $proc = Start-Process @startArgs
    # Windows PowerShell only reports ExitCode for a process whose handle was opened while it ran.
    $null = $proc.Handle
    $finished = $proc.WaitForExit($TimeoutSec * 1000)
    if (-not $finished) {
        $proc.Kill()
        Write-Host ("  TIMEOUT  {0} after {1}s" -f $label, $TimeoutSec) -ForegroundColor Red
        [void]$failed.Add($label)
        continue
    }
    $proc.WaitForExit()
    $exitCode = $proc.ExitCode
    if ((Test-Path -LiteralPath $errLog) -and (Get-Item -LiteralPath $errLog).Length -gt 0) {
        Add-Content -LiteralPath $log -Value (Get-Content -LiteralPath $errLog)
    }
    Remove-Item -LiteralPath $errLog -ErrorAction SilentlyContinue

    $seconds = [int]((Get-Date) - $started).TotalSeconds
    $skips = @(Select-String -LiteralPath $log -Pattern '^\s*SKIP' -ErrorAction SilentlyContinue).Count
    $note = ''
    if ($skips -gt 0) { $note = " ($skips SKIP)" }
    if ($exitCode -eq 0) {
        Write-Host ("  PASS     {0} [{1}s]{2}" -f $label, $seconds, $note)
    }
    else {
        Write-Host ("  FAIL     {0} [{1}s] exit {2}, see {3}" -f $label, $seconds, $exitCode, $log) -ForegroundColor Red
        [void]$failed.Add($label)
    }
}

Write-Host ''
if ($failed.Count -gt 0) {
    Write-Host ("{0} of {1} failed: {2}" -f $failed.Count, $entries.Count, ($failed -join ', ')) -ForegroundColor Red
    exit 1
}
Write-Host ("all {0} passed" -f $entries.Count) -ForegroundColor Green
exit 0
