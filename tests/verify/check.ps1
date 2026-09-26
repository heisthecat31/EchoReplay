# gate-tier: T1
#
# W2-I's gate entry point, discovered by tools\gate.ps1 by convention: tests\<name>\check.ps1.
#
# Four sub-checks, each judged twice -- by its exit code AND by what it reported covering. A check
# that exits 0 having covered nothing must never be indistinguishable from one that passed.
#
#   arms        every O2, O3 and E-7 case, each asserting a VERDICT and a REFUSAL REASON
#   floors      the measured sensitivity floor, at four orientations and four positions, and the
#               blind spot it is required to demonstrate
#   mutations   every knob of the gate perturbed; a knob no arm reacts to is a knob nothing tests
#   selftest    the doctest half: the metric, the source tag, the domain scoring policy
#
# ★ The GATE-RESULT count is itself mutation-tested. The harness is re-run with one arm removed from
# the domain the count measures, and this script fails unless the number drops by exactly one. A
# substance count nobody has watched move is not a substance count.

# -ShowRungs and not -Verbose: CmdletBinding already defines -Verbose, and redefining it makes
# the script unloadable rather than merely odd.
[CmdletBinding()]
param(
    [switch]$ShowRungs
)

# NOT 'Stop': under 'Stop' a native process writing to stderr raises NativeCommandError and aborts
# this script before it can record anything, turning a real failure into a run with no verdict.
$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)
if ($env:REPLAY_REPO_ROOT) { $repo = $env:REPLAY_REPO_ROOT }
$buildDir = Join-Path $repo 'build\W2-I'
if ($env:REPLAY_BUILD_DIR) { $buildDir = $env:REPLAY_BUILD_DIR }

$artifactDir = Join-Path $buildDir 'W2-I'
New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
$artifact = Join-Path $artifactDir 'verify_result.txt'

# Stamped FAIL before anything runs: a crashed run must not leave the previous run's PASS on disk
# for something else to read as this run's verdict.
Set-Content -Path $artifact -Encoding utf8 -Value @(
    ("timestamp: " + (Get-Date -Format 'o')),
    'VERDICT FAIL',
    'check.ps1 did not finish; this line is replaced only once every sub-check has reported.')

$failed = New-Object System.Collections.ArrayList
$summary = New-Object System.Collections.ArrayList
$checkedTotal = 0

function Add-Fail([string]$m) { [void]$failed.Add($m) }
function Add-Note([string]$m) { [void]$summary.Add($m) }

function Get-BinaryPath([string]$name) {
    $f = Join-Path $buildDir "${name}_path.txt"
    if (-not (Test-Path -LiteralPath $f)) { return $null }
    $p = (Get-Content -LiteralPath $f -Raw).Trim()
    if (-not $p -or -not (Test-Path -LiteralPath $p)) { return $null }
    return $p
}

# ---------------------------------------------------------------- the oracle harness

$harness = Get-BinaryPath 'verify_harness'
$baseChecked = 0
if (-not $harness) {
    Add-Fail "the oracle harness is not built; expected its path in $buildDir\verify_harness_path.txt"
} else {
    $out = & $harness 2>&1
    $code = $LASTEXITCODE
    $res = ($out | Where-Object { $_ -match '^O3-RESULT ' } | Select-Object -Last 1)
    foreach ($c in ($out | Where-Object { $_ -match '^O3-COVER ' })) { Add-Note ([string]$c) }
    foreach ($c in ($out | Where-Object { $_ -match '^FLOOR-(SITE|RESULT|POS) ' })) { Add-Note ([string]$c) }
    if ($ShowRungs) {
        foreach ($c in ($out | Where-Object { $_ -match '^FLOOR-RUNG ' })) { Add-Note ([string]$c) }
    }

    if (-not $res) {
        Add-Fail 'the oracle harness printed no O3-RESULT line'
        Write-Host ($out | Out-String)
    } elseif ($code -ne 0 -or $res -notmatch 'status=PASS') {
        Add-Fail ("oracle arms: " + $res)
        foreach ($f in ($out | Where-Object { $_ -match '^O3-FAIL ' })) { Write-Host ([string]$f) }
    } else {
        if ($res -match '\bchecked=(\d+)') { $baseChecked = [int]$matches[1] } else { $baseChecked = 0 }
        if ($baseChecked -le 0) {
            Add-Fail 'the oracle harness reported checked=0'
        } else {
            $checkedTotal += $baseChecked
        }
        Add-Note ([string]$res)

        # ---------------------------------------------------------- the sensitivity floor
        #
        # S12.3 requires the floor to include a defect the gate is SHOWN to miss, and says a run
        # where nothing is missed means the gate is mis-scaled and is itself a finding. The harness
        # enforces both directions internally; this reads the numbers back so the gate output
        # carries them rather than only the harness's own verdict.
        $missTotal = 0
        $redTotal = 0
        foreach ($line in ($out | Where-Object { $_ -match '^FLOOR-RESULT ' })) {
            if ($line -match 'missed_rungs=(\d+) of (\d+)') {
                $missTotal += [int]$matches[1]
            }
            if ($line -match 'first_red_deg=([0-9.]+)') {
                if ([double]$matches[1] -gt 0) { $redTotal++ }
            }
        }
        if ($missTotal -le 0) {
            Add-Fail ('the orientation floor missed nothing at any site; an oracle with no ' +
                      'demonstrated blind spot has not been characterised (S12.10.2 item 2)')
        }
        if ($redTotal -le 0) {
            Add-Fail 'no floor site saw any perturbation; the ladder cannot show the gate works'
        }
        Add-Note "floor: $missTotal rung(s) missed across sites, $redTotal site(s) saw a perturbation"

        # ---------------------------------------------------------- count mutation
        #
        # Remove one item from the domain the count measures and require the number to drop by
        # exactly one. `checked` is self-reported and the gate cannot verify it; this is what makes
        # the number mean something.
        $dropArm = 'o3-skew-frame'
        $dropOut = & $harness "--drop-arm=$dropArm" 2>&1
        $dropRes = ($dropOut | Where-Object { $_ -match '^O3-RESULT ' } | Select-Object -Last 1)
        if ($dropRes -and $dropRes -match '\bchecked=(\d+)') {
            $dropped = [int]$matches[1]
            if ($dropped -ne ($baseChecked - 1)) {
                Add-Fail ("the GATE-RESULT count is not measuring its domain: removing arm " +
                          "'$dropArm' moved checked from $baseChecked to $dropped, expected " +
                          "$($baseChecked - 1)")
            } else {
                Add-Note "count mutation: removing arm '$dropArm' moved checked $baseChecked -> $dropped"
            }
        } else {
            Add-Fail 'the count mutation produced no O3-RESULT line'
        }

        # A second victim, in a different part of the domain, because one arm could be special.
        $dropArm2 = 'o2-negative-zero'
        $dropOut2 = & $harness "--drop-arm=$dropArm2" 2>&1
        $dropRes2 = ($dropOut2 | Where-Object { $_ -match '^O3-RESULT ' } | Select-Object -Last 1)
        if ($dropRes2 -and $dropRes2 -match '\bchecked=(\d+)') {
            $dropped2 = [int]$matches[1]
            if ($dropped2 -ne ($baseChecked - 1)) {
                Add-Fail ("the GATE-RESULT count is not measuring its domain: removing arm " +
                          "'$dropArm2' moved checked from $baseChecked to $dropped2")
            } else {
                Add-Note "count mutation: removing arm '$dropArm2' moved checked $baseChecked -> $dropped2"
            }
        } else {
            Add-Fail 'the second count mutation produced no O3-RESULT line'
        }

        # A name that is not an arm must NOT move the count. Without this, a --drop-arm that silently
        # dropped the last arm whatever it was told would pass the two checks above.
        $dropOut3 = & $harness "--drop-arm=not-an-arm-at-all" 2>&1
        $dropRes3 = ($dropOut3 | Where-Object { $_ -match '^O3-RESULT ' } | Select-Object -Last 1)
        if ($dropRes3 -and $dropRes3 -match '\bchecked=(\d+)') {
            if ([int]$matches[1] -ne $baseChecked) {
                Add-Fail ("dropping a name that is not an arm moved checked from $baseChecked to " +
                          "$($matches[1]); --drop-arm is not selecting by name")
            } else {
                Add-Note "count mutation control: an unknown arm name left checked at $baseChecked"
            }
        } else {
            Add-Fail 'the count mutation control produced no O3-RESULT line'
        }
    }

    # ---------------------------------------------------------------- oracle mutations
    #
    # Every knob of the gate, perturbed one at a time. A knob no arm reacts to is a knob nothing is
    # testing -- the "arm no mutation could reach" defect, approached from the mutation's side.
    $muts = @(& $harness '--list-oracle-mutations' 2>&1 | Where-Object { $_ -match '\S' })
    if ($muts.Count -lt 4) {
        Add-Fail "the oracle mutation set has only $($muts.Count) entries; every O3Config knob needs one"
    }
    $caught = 0
    $unreached = New-Object System.Collections.ArrayList
    foreach ($m in $muts) {
        $name = ([string]$m).Split("`t")[0]
        $mo = & $harness "--mutate-oracle=$name" 2>&1
        $mr = ($mo | Where-Object { $_ -match '^MUTATION-RESULT ' } | Select-Object -Last 1)
        if ($mr -match 'status=CAUGHT') {
            $caught++
        } else {
            [void]$unreached.Add("$name ($mr)")
        }
    }
    Add-Note ("oracle mutations: total={0} caught={1} unreached={2}" -f $muts.Count, $caught, $unreached.Count)
    if ($unreached.Count -gt 0) {
        Add-Fail ("{0} gate parameter(s) no arm reacts to: {1}" -f $unreached.Count, ($unreached -join '; '))
    }
    $checkedTotal += $caught
}

# ---------------------------------------------------------------- the doctest half

$selftest = Get-BinaryPath 'verify_selftest'
if (-not $selftest) {
    Add-Fail "verify_selftest is not built; expected its path in $buildDir\verify_selftest_path.txt"
} else {
    $so = & $selftest '--reporters=xml' 2>&1
    $scode = $LASTEXITCODE
    $text = ($so | Out-String)
    $m = [regex]::Matches($text, '<OverallResultsAsserts successes="(?<ok>\d+)" failures="(?<bad>\d+)"')
    if ($m.Count -eq 0) {
        Add-Fail 'verify_selftest produced no doctest XML; it never reported what it asserted'
        Write-Host $text
    } else {
        $last = $m[$m.Count - 1]
        $asserts = [int]$last.Groups['ok'].Value + [int]$last.Groups['bad'].Value
        if ($scode -ne 0 -or [int]$last.Groups['bad'].Value -gt 0) {
            Add-Fail ("verify_selftest: {0} assertion(s) failed" -f $last.Groups['bad'].Value)
            Write-Host $text
        } elseif ($asserts -le 0) {
            Add-Fail 'verify_selftest ran and asserted nothing'
        } else {
            $checkedTotal += $asserts
            Add-Note "selftest: $asserts assertion(s)"
        }
    }
}

# ---------------------------------------------------------------- record and report

$body = New-Object System.Collections.ArrayList
[void]$body.Add("timestamp: " + (Get-Date -Format 'o'))
foreach ($s in $summary) { [void]$body.Add([string]$s) }
[void]$body.Add($(if ($failed.Count -eq 0) { 'VERDICT PASS' } else { 'VERDICT FAIL' }))
foreach ($f in $failed) { [void]$body.Add([string]$f) }
Set-Content -Path $artifact -Value $body -Encoding utf8

foreach ($s in $summary) { Write-Host ("  " + $s) }
foreach ($f in $failed) { Write-Host ("FAIL " + $f) }

if ($checkedTotal -le 0) { Add-Fail 'nothing reported a non-zero item count; refusing to call that a pass' }

$subject = 'o2-o3-arms+sensitivity-floor+oracle-mutations+metric-selftest'
if ($failed.Count -gt 0) {
    Write-Host ("RESULT check.ps1 status=FAIL subchecks=4 failures={0}" -f $failed.Count)
    Write-Host ("GATE-RESULT checked={0} subject={1} status=FAIL" -f $checkedTotal, $subject)
    exit 1
}
Write-Host 'RESULT check.ps1 status=PASS subchecks=4 failures=0'
Write-Host ("GATE-RESULT checked={0} subject={1} status=PASS" -f $checkedTotal, $subject)
exit 0
