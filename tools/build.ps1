<#
.SYNOPSIS
    Configures and builds echoreplay.dll and every test target with MSVC + Ninja.

.DESCRIPTION
    Needs Visual Studio 2022 or newer (or its Build Tools) with the "Desktop development with C++"
    workload, MSVC 14.38 or later. The newest such install is found through vswhere; nothing is looked
    up by path. Ninja and CMake come from PATH or, failing that, from any Visual Studio install.
    MSVC only: the root CMakeLists.txt refuses any other compiler.

    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 is passed here as well as being set in CMakeLists.txt: CMake 4
    rejects any project declaring cmake_minimum_required below 3.5, and vendored MinHook declares
    3.0...3.5.

.PARAMETER VcVarsVer
    Pin a toolset, e.g. 14.38. Default: the newest install's default toolset.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1
#>
[CmdletBinding()]
param(
    [string]$BuildDir,
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string]$Config = 'Release',
    [string]$VcVarsVer,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build' }

function Fail([string]$message) {
    Write-Host "BUILD FAILED: $message" -ForegroundColor Red
    exit 1
}

function Find-First([string[]]$candidates, [string]$what) {
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return (Resolve-Path -LiteralPath $c).Path }
    }
    Fail "$what not found. Looked in:`n  $($candidates -join "`n  ")"
}

$MinimumMsvc = [version]'14.38'

# CMake keeps the compiler a build dir was configured with, while vcvars sets INCLUDE/LIB for whichever
# toolset it picks: a newer toolset's headers under the cached cl.exe fail with STL1001. An existing build
# dir therefore keeps its toolset unless -Clean or -VcVarsVer says otherwise.
$cachedCompiler = $null
$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (-not $Clean -and (Test-Path -LiteralPath $cacheFile)) {
    $hit = Select-String -LiteralPath $cacheFile -Pattern '^CMAKE_CXX_COMPILER:FILEPATH=(.+)$' | Select-Object -First 1
    if ($hit) { $cachedCompiler = $hit.Matches[0].Groups[1].Value.Trim() -replace '/', '\' }
}
if ($cachedCompiler -and -not $VcVarsVer -and $cachedCompiler -match '\\MSVC\\(\d+\.\d+)\.\d+\\') {
    $VcVarsVer = $matches[1]
}

$vsInstaller = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
$vswhere = Join-Path $vsInstaller 'vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    Fail "vswhere.exe is not at $vswhere. Install Visual Studio 2022 or newer (or its Build Tools) with the 'Desktop development with C++' workload."
}
# Newest first. With -VcVarsVer, the newest install that carries that toolset.
$vsInstalls = @(& $vswhere -products * -version '[17.0,)' -sort -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
$vsInstall = $null
foreach ($candidate in $vsInstalls) {
    if (-not $VcVarsVer -or @(Get-ChildItem -Path (Join-Path $candidate 'VC\Tools\MSVC') -Directory -Filter "$VcVarsVer*" -ErrorAction SilentlyContinue).Count -gt 0) {
        $vsInstall = $candidate
        break
    }
}
if (-not $vsInstall) {
    if ($VcVarsVer) { Fail "no Visual Studio 2022+ install carries MSVC $VcVarsVer. Installs found:`n  $($vsInstalls -join "`n  ")" }
    Fail "no Visual Studio 2022+ install with the C++ x64 toolset was found. Install the 'Desktop development with C++' workload."
}
$vcvars = Find-First @(Join-Path $vsInstall 'VC\Auxiliary\Build\vcvars64.bat') 'vcvars64.bat'

$vsCMakeDirs = @($vsInstalls | ForEach-Object { Join-Path $_ 'Common7\IDE\CommonExtensions\Microsoft\CMake' })
$ninjaCandidates = @()
$ninjaOnPath = Get-Command ninja.exe -ErrorAction SilentlyContinue
if ($ninjaOnPath) { $ninjaCandidates += $ninjaOnPath.Source }
$ninjaCandidates += @($vsCMakeDirs | ForEach-Object { Join-Path $_ 'Ninja\ninja.exe' })
$ninja = Find-First $ninjaCandidates 'ninja.exe'

# vcvars64.bat shells out to a bare `vswhere.exe`; without the Installer directory on PATH it prints
# "not recognized" and falls back to a guess about which toolset it selected.
$env:PATH = "$vsInstaller;$env:PATH"

# Import the toolchain environment once so cmake and cl report their own exit codes directly instead of
# cmd.exe's.
$vcvarsArgs = ''
if ($VcVarsVer) { $vcvarsArgs = "-vcvars_ver=$VcVarsVer" }
Write-Host "vcvars   : $vcvars $vcvarsArgs"
$vcvarsOutput = & cmd.exe /c "call `"$vcvars`" $vcvarsArgs >nul && set"
$imported = 0
foreach ($line in $vcvarsOutput) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
        $imported++
    }
}
if ($imported -lt 10) {
    Fail "vcvars64.bat produced no environment ($vcvars $vcvarsArgs)."
}

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl) { Fail 'cl.exe is not on PATH after vcvars64.bat.' }
if ($cl.Source -notmatch '\\MSVC\\(\d+\.\d+)\.\d+\\') { Fail "cannot read the MSVC version out of $($cl.Source)" }
$msvcVersion = $matches[1]
# -vcvars_ver is a request, not a guarantee.
if ($VcVarsVer -and -not $msvcVersion.StartsWith($VcVarsVer)) {
    Fail "asked for MSVC $VcVarsVer but vcvars selected $($cl.Source)"
}
if ($cachedCompiler -and $cachedCompiler -ne $cl.Source) {
    Fail "$BuildDir was configured with $cachedCompiler but this toolchain is $($cl.Source). Pass -Clean to reconfigure it, or a -BuildDir of its own."
}
if ([version]$msvcVersion -lt $MinimumMsvc) {
    Fail "MSVC $msvcVersion is older than the minimum $MinimumMsvc (Visual Studio 2022 17.8). Update Visual Studio or pass -VcVarsVer."
}
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmake) {
    $bundled = @($vsCMakeDirs | ForEach-Object { Join-Path $_ 'CMake\bin\cmake.exe' } | Where-Object { Test-Path -LiteralPath $_ })
    if ($bundled.Count -eq 0) { Fail 'cmake.exe is not on PATH and no Visual Studio install bundles one. Install CMake 3.20 or newer.' }
    $cmake = Get-Command $bundled[0]
}

$dumpbin = Join-Path (Split-Path -Parent $cl.Source) 'dumpbin.exe'
if (-not (Test-Path -LiteralPath $dumpbin)) { Fail "dumpbin.exe not found beside $($cl.Source)" }

$cmakeVersion = (& $cmake.Source --version | Select-Object -First 1)
Write-Host "cl       : $($cl.Source)"
Write-Host "ninja    : $ninja"
Write-Host "cmake    : $($cmake.Source)  ($cmakeVersion)"
Write-Host "build dir: $BuildDir  ($Config)"
Write-Host ''

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

$ninjaFile = Join-Path $BuildDir 'build.ninja'
$ninjaStampBefore = $null
if (Test-Path -LiteralPath $ninjaFile) { $ninjaStampBefore = (Get-Item -LiteralPath $ninjaFile).LastWriteTimeUtc }

& $cmake.Source -S $repoRoot -B $BuildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
if ($LASTEXITCODE -ne 0) { Fail "cmake configure returned $LASTEXITCODE" }

# WINDOWS_EXPORT_ALL_SYMBOLS caches a generated exports.def that CMake does not regenerate when a source
# file disappears, so removing a module leaves a .def naming a symbol nothing defines and the link fails
# with LNK2001 on a name that is no longer greppable. Drop the def whenever the build graph was rewritten.
$ninjaStampAfter = $null
if (Test-Path -LiteralPath $ninjaFile) { $ninjaStampAfter = (Get-Item -LiteralPath $ninjaFile).LastWriteTimeUtc }
if ($ninjaStampBefore -ne $ninjaStampAfter) {
    foreach ($def in @(Get-ChildItem -Path $BuildDir -Recurse -Filter 'exports.def' -ErrorAction SilentlyContinue)) {
        Remove-Item -LiteralPath $def.FullName -Force
        Write-Host "regenerating $($def.FullName) (build graph changed)"
    }
}

# Ninja decides what to rebuild from mtimes, so a tracked source whose CONTENT moved while its mtime did
# not is never recompiled and every check then runs against a stale binary reporting PASS. This was
# guarded by watching the build output for ninja's "no work to do" - a guard that could never fire, because
# CONFIGURE_DEPENDS keeps cmake.verify_globs permanently dirty and several probe DLLs relink on every run,
# so ninja always has work and the comparison was unreachable. Hash the sources by content instead and give
# any file whose content moved a fresh mtime BEFORE building: ninja then cannot miss it. The class is
# removed rather than detected, which is the only version of this that does not depend on a string.
function Get-TrackedSourceHashes([string]$root, $git) {
    if (-not $git) { return $null }
    Push-Location -LiteralPath $root
    try {
        # tools/ is in scope because the checks certify instruments built from tools/**.cpp against this
        # record: a source left out of it has no recorded hash, and its binary can never be told current.
        $tracked = @(& $git.Source ls-files -- src tests third_party tools CMakeLists.txt 2>$null | Where-Object { $_ })
        if ($LASTEXITCODE -ne 0 -or $tracked.Count -eq 0) { return $null }
    }
    finally { Pop-Location }
    $hashes = @{}
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        foreach ($f in $tracked) {
            $abs = Join-Path $root $f
            if (-not (Test-Path -LiteralPath $abs -PathType Leaf)) { continue }
            # Eleven agents share this tree, so a file can be held open by whoever is writing it. Leaving it
            # out of the record makes the next build treat it as changed and rebuild it, which is the safe
            # direction; failing the build here would make another packet's save our error.
            try {
                $stream = [IO.File]::OpenRead($abs)
                try { $hashes[$f] = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
                finally { $stream.Dispose() }
            }
            catch { Write-Host "sources  : could not read $f ($($_.Exception.Message)); it will be rebuilt next time" }
        }
    }
    finally { $sha.Dispose() }
    return $hashes
}

$gitCmd = Get-Command git.exe -ErrorAction SilentlyContinue
$hashFile = Join-Path $BuildDir 'source_hashes.txt'
$currentHashes = Get-TrackedSourceHashes $repoRoot $gitCmd
$previousHashes = $null
if (Test-Path -LiteralPath $hashFile) {
    $previousHashes = @{}
    foreach ($line in (Get-Content -LiteralPath $hashFile)) {
        $parts = $line -split ' ', 2
        if ($parts.Count -eq 2) { $previousHashes[$parts[1]] = $parts[0] }
    }
}
if ($null -ne $currentHashes) {
    # No record beside an existing build graph means the objects in this directory cannot be attributed to
    # any source content - a deleted record would otherwise restore exactly the staleness this closes. Every
    # source is refreshed, which costs one full rebuild and is the only honest answer.
    $unattributed = ($null -eq $previousHashes -and $null -ne $ninjaStampBefore)
    $stale = New-Object System.Collections.ArrayList
    foreach ($f in @($currentHashes.Keys | Sort-Object)) {
        if ($unattributed -or $null -eq $previousHashes -or
            -not $previousHashes.ContainsKey($f) -or $previousHashes[$f] -ne $currentHashes[$f]) {
            [void]$stale.Add($f)
        }
    }
    $now = [DateTime]::UtcNow
    $refreshed = 0
    foreach ($f in $stale) {
        $abs = Join-Path $repoRoot $f
        # A source dated in the future is already newer than every object; moving it back would create the
        # very gap this closes.
        if ([IO.File]::GetLastWriteTimeUtc($abs) -ge $now) { continue }
        try { [IO.File]::SetLastWriteTimeUtc($abs, $now); $refreshed++ }
        catch { Fail "$f changed since the last build and its timestamp could not be refreshed ($($_.Exception.Message)); ninja would compile the old object into a binary the checks then pass against" }
    }
    if ($unattributed) {
        Write-Host "sources  : $($currentHashes.Count) tracked, no content record for this build directory - refreshing all $refreshed so nothing built from an older source survives"
    }
    elseif ($null -eq $previousHashes) {
        Write-Host "sources  : $($currentHashes.Count) tracked, first build in this directory"
    }
    else {
        Write-Host "sources  : $($currentHashes.Count) tracked, $($stale.Count) changed since the last successful build, $refreshed mtime(s) refreshed"
    }
}
else {
    Write-Host 'sources  : NOT hashed (no git) - ninja mtimes are the only staleness protection in this run' -ForegroundColor Yellow
}

$buildOutput = & $cmake.Source --build $BuildDir
$buildExit = $LASTEXITCODE
$buildOutput | ForEach-Object { Write-Host $_ }
if ($buildExit -ne 0) { Fail "cmake --build returned $buildExit" }

# Written only after a successful build, and from the pre-build content: a file edited while the build ran
# is recorded as changed next time rather than as built.
if ($null -ne $currentHashes) {
    @($currentHashes.Keys | Sort-Object | ForEach-Object { "$($currentHashes[$_]) $_" }) |
        Set-Content -LiteralPath $hashFile -Encoding ascii
}

# Checks that need the toolchain (tests\exports runs dumpbin) get it from here rather than re-deriving it.
@(
    "cl=$($cl.Source)",
    "dumpbin=$dumpbin",
    "ninja=$ninja",
    "cmake=$($cmake.Source)",
    "vcvars_ver=$msvcVersion"
) | Set-Content -LiteralPath (Join-Path $BuildDir 'toolchain.txt') -Encoding utf8

$dll = Join-Path (Join-Path $BuildDir 'bin') 'echoreplay.dll'
if (-not (Test-Path -LiteralPath $dll)) { Fail "build reported success but $dll does not exist" }
$dllSize = (Get-Item -LiteralPath $dll).Length
if ($dllSize -lt 1024) { Fail "$dll is $dllSize bytes; that is the broken-linker signature" }

Write-Host ''
Write-Host "BUILD OK: $dll ($dllSize bytes)" -ForegroundColor Green
exit 0
