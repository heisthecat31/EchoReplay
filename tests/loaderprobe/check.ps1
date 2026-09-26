# gate-tier: T1
#
# The offline half of W1-C's acceptance. It reproduces, without echovr.exe, the one condition no unit test
# reaches: our DLL_PROCESS_ATTACH running under the Windows loader lock, entered from another DLL's
# DllMain, with a second plugin sharing the folder.
#
#   probe_host.exe  ->  LoadLibrary(probe_loader.dll)
#                           -> its DllMain scans <cwd>\plugins and LoadLibrarys every *.dll, discarding
#                              the return value, exactly as dbgcore.dll does at WinMain statement 2
#                                  -> echoreplay.dll's DllMain runs with the loader lock held
#
# What this proves, and what it cannot:
#
#   PROVED HERE   the attach line is written before anything else; no deadlock under a nested DllMain;
#                 the CWD-relative plugins scan; the wrong-build refusal end to end, including that the
#                 md5 in the log is the real md5 of the host (cross-checked against Get-FileHash, a
#                 second implementation this project did not write); that a refused build initialises no
#                 module; SC9 with rlay.dll present in BOTH load orders and absent; and that the only
#                 files created anywhere are echoreplay.log and echoreplay\config.json.
#   NOT PROVED    the ACCEPT path. Every host here is the wrong build by construction, so the guard is
#                 only ever seen refusing. Acceptance, the slot claim and coexistence with the real
#                 rlay.dll are V-I11 and go through W1-J's sweep (runs\queue\W1-C.probe.json).
#
# ★ THE GATE-RESULT COUNT, AND HOW TO SEE IT MOVE (docs\INSTALL.md requires the demonstration):
#   -Only <scenario>  runs one of the three scenarios instead of all three; `checked` drops by the
#                     per-scenario assertion count, because the number is the assertions actually made.
[CmdletBinding()]
param(
    [ValidateSet('all', 'rlay-first', 'echoreplay-first', 'rlay-absent')]
    [string]$Only = 'all',
    [int]$TimeoutSec = 90
)

$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)
if ($env:REPLAY_REPO_ROOT) { $repo = $env:REPLAY_REPO_ROOT }
$build = Join-Path $repo 'build'
if ($env:REPLAY_BUILD_DIR) { $build = $env:REPLAY_BUILD_DIR }
# ★ Absolute, because the child process is launched with its working directory set to the staging folder
# so the plugins scan happens where we want it. The gate passes REPLAY_BUILD_DIR as a relative path, and a
# relative loader path then resolves against the staging directory instead of the build - which showed up
# as LoadLibraryW failing with 126 and looked exactly like a broken DLL.
$repo = (Resolve-Path -LiteralPath $repo).Path
if (-not [System.IO.Path]::IsPathRooted($build)) { $build = Join-Path $repo $build }
$build = (Resolve-Path -LiteralPath $build).Path
$bin = Join-Path $build 'bin'

$script:checked = 0
$script:failures = New-Object System.Collections.ArrayList

function Assert([string]$what, [bool]$ok, [string]$detail = '') {
    $script:checked++
    if (-not $ok) {
        $line = "FAIL $what"
        if ($detail) { $line += " -- $detail" }
        [void]$script:failures.Add($line)
        Write-Host $line
    }
}

function Fatal([string]$why) {
    Write-Host "FAIL $why"
    Write-Host "GATE-RESULT checked=$script:checked subject=w1c-loader-lock+coexistence status=NO-VERDICT"
    exit 1
}

# --- prerequisites. A missing binary is "could not run", never "ran and passed".
$hostExe = Join-Path $bin 'probe_host.exe'
$loaderDll = Join-Path $bin 'probe_loader.dll'
$rlayDll = Join-Path $bin 'rlay.dll'
$pluginDll = Join-Path $bin 'echoreplay.dll'
foreach ($p in @($hostExe, $loaderDll, $rlayDll, $pluginDll)) {
    Assert "prerequisite exists: $(Split-Path -Leaf $p)" (Test-Path -LiteralPath $p) $p
}
if ($script:failures.Count -gt 0) { Fatal 'the probe binaries are not built; run the build first' }

# The independent witness for our own MD5: Windows computes it, we compare the plugin's log against it.
$hostMd5 = (Get-FileHash -LiteralPath $hostExe -Algorithm MD5).Hash.ToLowerInvariant()
$goldMd5 = 'caa05ee3bca2f5df0809994e93e2cc05'

$scenarios = @(
    @{ Name = 'rlay-first';       Order = 'reverse'; Rlay = $true },
    @{ Name = 'echoreplay-first'; Order = '';        Rlay = $true },
    @{ Name = 'rlay-absent';      Order = '';        Rlay = $false }
)
if ($Only -ne 'all') { $scenarios = @($scenarios | Where-Object { $_.Name -eq $Only }) }
if ($scenarios.Count -eq 0) { Fatal "no scenario named '$Only'" }

foreach ($scenario in $scenarios) {
    $stage = Join-Path $build ('loaderprobe_' + $scenario.Name)
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    New-Item -ItemType Directory -Path (Join-Path $stage 'plugins') -Force | Out-Null

    Copy-Item -LiteralPath $pluginDll -Destination (Join-Path $stage 'plugins\echoreplay.dll')
    if ($scenario.Rlay) { Copy-Item -LiteralPath $rlayDll -Destination (Join-Path $stage 'plugins\rlay.dll') }

    # Everything present before the run. Anything not in this set afterwards is residue, and S4.5.3 rule 5
    # says there must be exactly two such paths.
    $before = @(Get-ChildItem -LiteralPath $stage -Recurse -Force | ForEach-Object { $_.FullName })

    $logPath = Join-Path $stage 'echoreplay.log'
    $env:REPLAY_PROBE_ORDER = $scenario.Order

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $hostExe
    $psi.Arguments = ('"{0}" "{1}" buildguard 30000' -f $loaderDll, $logPath)
    $psi.WorkingDirectory = $stage          # ★ the scan root is the CWD, not the exe directory
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables['REPLAY_PROBE_ORDER'] = $scenario.Order

    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdout = $proc.StandardOutput.ReadToEndAsync()
    $stderr = $proc.StandardError.ReadToEndAsync()
    $exited = $proc.WaitForExit($TimeoutSec * 1000)
    if (-not $exited) {
        # A hang here is the signature of a deadlock under the loader lock, which is the single failure
        # this probe exists to catch. It is reported as such rather than as a generic timeout.
        try { $proc.Kill() } catch {}
        Assert "$($scenario.Name): probe_host exits (no deadlock under the nested loader lock)" $false `
            "still running after $TimeoutSec s"
        continue
    }
    $exitCode = $proc.ExitCode
    $console = "$($stdout.Result)$($stderr.Result)"
    $proc.Dispose()

    $n = $scenario.Name
    Assert "${n}: probe_host exits 0 (no deadlock, marker seen)" ($exitCode -eq 0) `
        "exit $exitCode; $console"

    if (-not (Test-Path -LiteralPath $logPath)) {
        Assert "${n}: echoreplay.log was written" $false 'no log file at all: the DLL never attached'
        continue
    }
    Assert "${n}: echoreplay.log was written" $true
    $lines = @(Get-Content -LiteralPath $logPath)
    $log = ($lines -join "`n")

    # ★ The first statement. EchoLoader logs nothing, so this line is the only evidence we ran at all -
    # and it has to be FIRST, before anything that could fault.
    Assert "${n}: the first line is the attach line" ($lines.Count -gt 0 -and $lines[0] -match '\battached\b') `
        $(if ($lines.Count -gt 0) { $lines[0] } else { '(empty)' })

    Assert "${n}: the log records the CWD the plugins scan used" ($log -match [regex]::Escape("cwd=$stage"))
    Assert "${n}: the log records our own module path" `
        ($log -match [regex]::Escape("plugins\echoreplay.dll"))

    # Static self-registration happened during CRT init, before DllMain: a refused build still proves the
    # registry mechanism itself is alive.
    $registered = [regex]::Match($log, '(\d+) module\(s\) self-registered')
    Assert "${n}: at least one module self-registered before DllMain ran" `
        ($registered.Success -and [int]$registered.Groups[1].Value -ge 1) $registered.Value

    # The wrong-build refusal, end to end.
    Assert "${n}: the build guard refused" ($log -match 'REFUSED')
    Assert "${n}: the refusal names the build the registry was read from" ($log -match [regex]::Escape($goldMd5))
    # ... and the md5 it computed is the real md5 of the host, per Windows' own implementation. This is
    # what makes the refusal evidence rather than a message: a broken hash would refuse just as loudly.
    Assert "${n}: the md5 in the log is the host's real md5 (cross-checked against Get-FileHash)" `
        ($log -match [regex]::Escape($hostMd5)) "expected $hostMd5"
    Assert "${n}: nothing was accepted" ($log -notmatch 'build accepted')
    Assert "${n}: a refused build initialises no module" ($log -notmatch 'module registry live')
    Assert "${n}: a refused build arms no hook" ($log -notmatch ': armed(?!=)')

    Assert "${n}: config.json was written into the state directory" `
        (Test-Path -LiteralPath (Join-Path $stage 'echoreplay\config.json'))

    # SC9: present in either order, and absent.
    #
    # This observation is deterministic rather than lucky, and the reason is worth writing down: the
    # survey runs on our worker thread and calls GetModuleHandle, which takes the Windows loader lock -
    # the same lock probe_loader's DllMain is holding while it loads the rest of the folder. So the
    # worker cannot observe a half-loaded folder however the scan was ordered. It is also why the survey
    # must not run in DllMain, where that block would be a deadlock instead of a wait.
    if ($scenario.Rlay) {
        Assert "${n}: rlay.dll was detected as loaded" ($log -match 'rlay\.dll is loaded')
        Assert "${n}: rlay.dll was named as sharing the folder" ($log -match 'sharing this folder with rlay\.dll')
    } else {
        Assert "${n}: rlay.dll was correctly reported absent" ($log -match 'rlay\.dll is absent')
        Assert "${n}: no other plugin was claimed to be present" ($log -match '0 besides us')
    }

    # ★ Uninstall is deleting one file. Exactly two paths may appear, and this is the check that says so.
    $after = @(Get-ChildItem -LiteralPath $stage -Recurse -Force | ForEach-Object { $_.FullName })
    $created = @($after | Where-Object { $before -notcontains $_ } | ForEach-Object {
            $_.Substring($stage.Length).TrimStart('\') } | Sort-Object)
    $expected = @('echoreplay', 'echoreplay.log', 'echoreplay\config.json')
    Assert "${n}: nothing was created outside the log and the state directory" `
        (($created -join '|') -eq ($expected -join '|')) "created: $($created -join ', ')"
}

$env:REPLAY_PROBE_ORDER = $null

# Who else is patching echovr.exe, and does any of it collide with a site WE hook?
#
# The 2026-08-28 sweep found rlay.dll had hooked one of the build guard's fingerprint sites. That costs a
# log line, because a fingerprint site is read and never written. A collision on a site this project
# PATCHES would be a real contention, and that is what this scan is for. It is run here so the answer is
# re-derived every gate rather than remembered from the day somebody looked.
$python = Get-Command python.exe -ErrorAction SilentlyContinue
if ($python) {
    $scan = & $python.Source (Join-Path $here 'scan_foreign_detours.py') 2>&1
    $scanCode = $LASTEXITCODE
    $scan | ForEach-Object { Write-Host "  $_" }
    $line = ($scan | Where-Object { $_ -match '^RESULT foreigndetours ' } | Select-Object -Last 1)
    Assert 'the foreign-detour scan reported a result' ($null -ne $line)
    if ($line) {
        $targets = 0
        if ($line -match 'targets=(\d+)') { $targets = [int]$matches[1] }
        # rlay.dll is a Detours DLL, so when it is installed a scan that finds nothing means the idiom
        # search stopped matching. Without it, zero targets is the correct answer.
        if ($line -match 'rlay=1') {
            Assert 'the scan found the targets rlay.dll is known to resolve (it is not silently matching nothing)' `
                ($targets -ge 12) "targets=$targets"
        }
        Assert 'no foreign plugin patches a site this project hooks' `
            ($line -match 'collisions=0') $line
        Assert 'the foreign-detour scan exited cleanly' ($scanCode -eq 0) "exit $scanCode"
    }
} else {
    Assert 'python is available for the foreign-detour scan' $false
}

# The networking surface, enumerated from the PE import directories rather than remembered.
#
# "The complete networking surface" had once been asserted over 18 of echovr.exe's 37 ws2_32 imports,
# with WININET omitted entirely while BugSplat64.dll statically imports ten of its functions. This runs
# the enumeration every gate so the claim cannot drift from the binaries again.
Assert 'python is available for the networking-surface scan' ($null -ne $python)
if ($python) {
    $surfaceOut = Join-Path $build 'loaderprobe_network_surface.tsv'
    $surface = & $python.Source (Join-Path $here 'scan_network_surface.py') $surfaceOut 2>&1
    $surfaceCode = $LASTEXITCODE
    $surface | ForEach-Object { Write-Host "  $_" }
    $sline = ($surface | Where-Object { $_ -match '^RESULT networksurface ' } | Select-Object -Last 1)
    Assert 'the networking-surface scan reported a result' ($null -ne $sline)
    if ($sline -and $sline -notmatch 'status=SKIP') {
        $rows = 0
        if ($sline -match 'rows=([0-9]+)') { $rows = [int]$matches[1] }
        # echovr.exe alone imports 37 ws2_32 entries; a scan finding fewer than that across the whole
        # install has stopped reading import directories rather than found a smaller surface.
        Assert 'the surface scan still reads the import directories' ($rows -ge 37) "rows=$rows"
        Assert 'the surface scan exited cleanly' ($surfaceCode -eq 0) "exit $surfaceCode"
    }
}



# Normalises a run's log for comparison: the wall clock, the pid and the staging directory name are the
# only things two runs of the same code are allowed to differ in.
function Get-NormalisedLog([string]$path) {
    $text = (Get-Content -LiteralPath $path) -join "`n"
    $text = $text -replace '(?m)^\d\d:\d\d:\d\d\.\d\d\d ', ''
    $text = $text -replace 'pid=\d+', 'pid=N'
    $text = $text -replace 'loaderprobe_[a-z-]+', 'STAGE'
    return [string]$text
}

# The SC9 property itself, stated as one comparison rather than inferred from the assertions above:
# reversing the load order changes nothing this plugin does.
#
# The rlay-absent run is the known-negative control. Without it this whole comparison could be comparing
# two empty strings and reporting a pass - which is exactly what it did on its first draft, when a line
# continuation was eaten and two of the three -replace operators silently never ran.
if ($Only -eq 'all') {
    $forward = Join-Path $build 'loaderprobe_echoreplay-first\echoreplay.log'
    $reverse = Join-Path $build 'loaderprobe_rlay-first\echoreplay.log'
    $control = Join-Path $build 'loaderprobe_rlay-absent\echoreplay.log'
    if ((Test-Path -LiteralPath $forward) -and (Test-Path -LiteralPath $reverse) -and
        (Test-Path -LiteralPath $control)) {
        $a = Get-NormalisedLog $forward
        $b = Get-NormalisedLog $reverse
        $c = Get-NormalisedLog $control

        Assert 'the normalisation produced text to compare' ($a.Length -gt 400) "normalised to $($a.Length) chars"
        Assert 'the normalisation removed every timestamp' ($a -notmatch '(?m)^\d\d:\d\d:\d\d') 'a timestamp survived, so two runs could never compare equal'
        Assert 'load order changes nothing: the two orders produce the same log' ($a -ceq $b) 'reversing the plugins scan changed our behaviour'
        Assert 'control: the comparison can tell two genuinely different runs apart' ($a -cne $c) 'rlay present and rlay absent normalise identically, so the comparison above proves nothing'
    } else {
        Assert 'load order changes nothing: all three logs exist to compare' $false
    }
}

Write-Host "  scenarios=$($scenarios.Count) host=$(Split-Path -Leaf $hostExe) host_md5=$hostMd5"
$status = if ($script:failures.Count -eq 0) { 'PASS' } else { 'FAIL' }
Write-Host "GATE-RESULT checked=$script:checked subject=w1c-loader-lock+coexistence status=$status"
if ($script:failures.Count -gt 0) { exit 1 }
exit 0
