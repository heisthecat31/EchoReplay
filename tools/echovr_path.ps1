# Finds the Echo VR install for scripts and checks. Dot-source it: it only defines functions.
#
# Order: an explicit -GameDir, REPLAY_ECHOVR_EXE (the file), ECHOVR_DIR (the folder), then the Oculus libraries registered on
# this PC. A folder or variable that is given but does not hold the game means "not found"; it never
# falls through to a different install, so a machine that has the game can still exercise the absent path.

function Get-EchoVrExeUnder([string]$Dir) {
    if (-not $Dir) { return $null }
    foreach ($tail in @('bin\win10\echovr.exe', 'echovr.exe')) {
        $exe = Join-Path $Dir $tail
        if (Test-Path -LiteralPath $exe -PathType Leaf) { return $exe }
    }
    return $null
}

function Get-OculusLibraryRoots {
    $roots = @()
    foreach ($lib in @(Get-ChildItem -LiteralPath 'HKCU:\Software\Oculus VR, LLC\Oculus\Libraries' -ErrorAction SilentlyContinue)) {
        $props = Get-ItemProperty -LiteralPath $lib.PSPath -ErrorAction SilentlyContinue
        if ($props.OriginalPath) { $roots += $props.OriginalPath }
        elseif ($props.Path) { $roots += $props.Path }
    }
    foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\Oculus VR, LLC\Oculus', 'HKLM:\SOFTWARE\Oculus VR, LLC\Oculus')) {
        $props = Get-ItemProperty -LiteralPath $key -Name Base -ErrorAction SilentlyContinue
        if ($props -and $props.Base) { $roots += (Join-Path $props.Base 'Software') }
    }
    if ($env:ProgramFiles) { $roots += (Join-Path $env:ProgramFiles 'Oculus\Software') }
    return $roots
}

# The echovr.exe to use, or $null when there is none.
function Find-EchoVrExe([string]$GameDir) {
    if ($GameDir) { return Get-EchoVrExeUnder $GameDir }
    if ($env:REPLAY_ECHOVR_EXE) {
        if (Test-Path -LiteralPath $env:REPLAY_ECHOVR_EXE -PathType Leaf) { return $env:REPLAY_ECHOVR_EXE }
        return $null
    }
    if ($env:ECHOVR_DIR) { return Get-EchoVrExeUnder $env:ECHOVR_DIR }
    foreach ($root in Get-OculusLibraryRoots) {
        $exe = Get-EchoVrExeUnder (Join-Path $root 'Software\ready-at-dawn-echo-arena')
        if ($exe) { return $exe }
    }
    return $null
}

# The install folder (the one holding bin\win10), or $null.
function Find-EchoVrDir([string]$GameDir) {
    $exe = Find-EchoVrExe $GameDir
    if (-not $exe) { return $null }
    $dir = Split-Path -Parent $exe
    if ((Split-Path -Leaf $dir) -eq 'win10' -and (Split-Path -Leaf (Split-Path -Parent $dir)) -eq 'bin') {
        return (Split-Path -Parent (Split-Path -Parent $dir))
    }
    return $dir
}

$script:EchoVrNotFound = 'echovr.exe was not found. Set ECHOVR_DIR to the Echo VR install folder (the one holding bin\win10), or REPLAY_ECHOVR_EXE to echovr.exe.'
