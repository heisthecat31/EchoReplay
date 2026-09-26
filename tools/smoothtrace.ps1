<#
.SYNOPSIS
    Reads the [smooth-trace] lines playback.motion_smoothing_trace writes and reports how choppy the
    driven motion was with smoothing off (w=0) and on (w=1).

.DESCRIPTION
    Each trace line is one rendered frame: the playhead t (source microseconds) and the lowest bound
    player's body com, left/right hand offsets and head quaternion as that frame drove them. Per channel,
    the per-frame velocity is v_f = (p_f - p_{f-1}) / (t_f - t_{f-1}) in SOURCE time, so the result does
    not depend on the playback rate or the frame rate. Choppiness is J = RMS(v_{f+1} - v_f) / RMS(v_f):
    the frame-to-frame change of velocity relative to the velocity itself.

    Frames are split by w (0 = off, 1 = on); crossfade frames (0 < w < 1), frames where the playhead did
    not advance (paused, or held for decode) and any pair across a jump of more than 100 ms of source are
    left out. Pass: J_on <= 0.5 x J_off for lh, rh and com.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\smoothtrace.ps1 C:\path\to\echoreplay.log
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$LogPath
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $LogPath)) {
    Write-Host "smoothtrace: no such log: $LogPath"
    exit 2
}

$pattern = 'smooth-trace\] n=\d+ wall_us=-?\d+ t=(-?\d+) rate=(-?[\d.]+) w=([\d.]+) com=([^ ]+) lh=([^ ]+) rh=([^ ]+) hmd=([^ ]+)'
$frames = New-Object System.Collections.Generic.List[object]
foreach ($line in [System.IO.File]::ReadLines($LogPath)) {
    $m = [regex]::Match($line, $pattern)
    if (-not $m.Success) { continue }
    $frames.Add([pscustomobject]@{
        t   = [double]$m.Groups[1].Value
        w   = [double]$m.Groups[3].Value
        com = [double[]]($m.Groups[4].Value -split ',')
        lh  = [double[]]($m.Groups[5].Value -split ',')
        rh  = [double[]]($m.Groups[6].Value -split ',')
    })
}
if ($frames.Count -lt 3) {
    Write-Host "smoothtrace: $($frames.Count) trace line(s) in $LogPath -- was playback.motion_smoothing_trace on?"
    exit 2
}

function Get-Choppiness([System.Collections.Generic.List[object]]$all, [double]$weight, [string]$channel) {
    $sumStep = 0.0; $sumVel = 0.0; $steps = 0; $vels = 0
    $prevV = $null
    for ($i = 1; $i -lt $all.Count; $i++) {
        $a = $all[$i - 1]; $b = $all[$i]
        $dt = ($b.t - $a.t) / 1e6
        if ($a.w -ne $weight -or $b.w -ne $weight -or $dt -eq 0 -or [math]::Abs($dt) -gt 0.1) {
            $prevV = $null
            continue
        }
        $v = @(0.0, 0.0, 0.0)
        for ($c = 0; $c -lt 3; $c++) { $v[$c] = ($b.$channel[$c] - $a.$channel[$c]) / $dt }
        $sumVel += $v[0] * $v[0] + $v[1] * $v[1] + $v[2] * $v[2]; $vels++
        if ($null -ne $prevV) {
            $d = 0.0
            for ($c = 0; $c -lt 3; $c++) { $d += ($v[$c] - $prevV[$c]) * ($v[$c] - $prevV[$c]) }
            $sumStep += $d; $steps++
        }
        $prevV = $v
    }
    if ($steps -eq 0 -or $sumVel -eq 0) { return $null }
    return [pscustomobject]@{ J = [math]::Sqrt($sumStep / $steps) / [math]::Sqrt($sumVel / $vels); Steps = $steps }
}

$verdict = 0
Write-Host ("smoothtrace: {0} frames in {1}" -f $frames.Count, $LogPath)
foreach ($ch in @('com', 'lh', 'rh')) {
    $off = Get-Choppiness $frames 0.0 $ch
    $on = Get-Choppiness $frames 1.0 $ch
    if ($null -eq $off -or $null -eq $on) {
        Write-Host ("  {0,-3}  not enough frames with w=0 and w=1 (off: {1}, on: {2})" -f $ch,
            $(if ($off) { $off.Steps } else { 0 }), $(if ($on) { $on.Steps } else { 0 }))
        $verdict = 2
        continue
    }
    $ratio = $on.J / $off.J
    $pass = $ratio -le 0.5
    if (-not $pass -and $verdict -eq 0) { $verdict = 1 }
    Write-Host ("  {0,-3}  J_off {1:N3} ({2} steps)  J_on {3:N3} ({4} steps)  ratio {5:N2}  {6}" -f $ch,
        $off.J, $off.Steps, $on.J, $on.Steps, $ratio, $(if ($pass) { 'PASS' } else { 'FAIL' }))
}
exit $verdict
