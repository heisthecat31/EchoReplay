# CLEAN(L5), computed. Until now it was a four-clause conjunction over two artifacts, with a four-state
# vocabulary on top of it, read BY HAND - and AUDIT_L5_RUN3 B1 is what that costs: a competent reader
# bound clause 3 to the egress record instead of the observer's done.json and scored 3 of 4 where the
# answer was 2 of 4. A definition whose only executor is a person is not single-sourced, it is unsourced.
#
# The clauses are the design notes as ARCH-14 rewrote them, and the four-state vocabulary is
# section 8.4a.4: an ABSENT field is could-not-ask, for the clause and for the verdict. Never zero, never
# false, never not-clean - but rule 5 amendment (a) settles what happens when a clause has both: A
# PRESENT DEFINITE NEGATIVE BEATS AN ABSENT FIELD, so an absent field decides a clause only when nothing
# present decides it. Section 8.4a states plainly that CLEAN(L5) is not evaluable today - ownerDraws
# and ownerTicks exist in no artifact until ARCH-13-C2 lands - so could-not-ask is the expected answer
# and it names the field.
param(
    [string]$LaunchDir,
    [switch]$SelfTest,
    [switch]$DefineOnly
)

$ErrorActionPreference = 'Stop'

# Section 8.4a.4: a row is a DECLARATION row iff every source it reads was written before the game
# process started - an input restated is not an observation. The flag is the authority; the list is the
# fallback for records written before ARCH-14-C2 emits it, and the resolver says which one decided.
$script:DeclarationRowIds = @('w1j-launch-window-declared')
$script:CleanL5RunningWords = @('Running', 'MarshalStale', 'CallbackSilent')

# Section 8.4a: rows[] is the field, and answers[] is accepted "only during the deliberate beside-phase"
# until ARCH-13-C4 has landed. The record this resolver was written against carries answers[] and no
# rows[], so the fallback is real and not defensive. Which one was read is printed, because "clause 3
# was scored off answers[]" is a different claim from "clause 3 was scored off rows[]".
function Get-CleanL5Rows($observer) {
    if (Test-JsonField $observer 'rows') { return [pscustomobject]@{ Rows = @($observer.rows); Source = 'rows[]' } }
    if (Test-JsonField $observer 'answers') { return [pscustomobject]@{ Rows = @($observer.answers); Source = 'answers[] (beside-phase)' } }
    return $null
}

function Test-JsonField($object, [string]$name) {
    if ($null -eq $object) { return $false }
    return (@($object.PSObject.Properties.Name) -contains $name)
}

function New-CleanL5Clause([int]$number, [string]$result, $fields, [string]$missing, [string]$why) {
    return [pscustomobject]@{
        Number = $number; Result = $result
        Fields = (@($fields) -join ', ')
        MissingField = $missing
        Why = $why
    }
}

# Section 8.4a.4 rule 5 amendment (a): A PRESENT DEFINITE NEGATIVE BEATS AN ABSENT FIELD. Each clause
# evaluates the fields it HAS first; an absent field decides the clause only when nothing present
# decides it, and then it decides it could-not-ask. It is one function so that all four clauses obey
# one rule: the shape this replaces answered could-not-ask on the recorded run that was killed on a
# timeout with probes still waiting, because a counter ARCH-13-C2 has not shipped yet was absent
# (AUDIT_W1J15 W1J15-2) - and a reader given could-not-ask waits for a field where they should stop.
# The absent fields are still NAMED on a not-clean clause; they just do not decide it.
function Resolve-CleanL5Clause([int]$number, $fields, $missing, [string[]]$negatives, [string]$absentWhy) {
    $neg = @(@($negatives) | Where-Object { $_ })
    $absent = (@($missing) -join ', ')
    if ($neg.Count -gt 0) { return New-CleanL5Clause $number 'not-clean' $fields $absent ($neg -join '; ') }
    if (@($missing).Count -gt 0) { return New-CleanL5Clause $number 'could-not-ask' $fields $absent $absentWhy }
    return New-CleanL5Clause $number 'clean' $fields '' ''
}

function Read-CleanL5Json([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    try { return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json) }
    catch { return $null }
}

# The observer is EchoReplay's record and nothing else. Section 8.4a names it twice, in bold, because
# the last reader took the egress record instead. If it is not there, that is a refusal by name.
function Get-CleanL5ObserverPath([string]$launchDir) {
    $probes = Join-Path $launchDir 'probes'
    if (-not (Test-Path -LiteralPath $probes)) { return $null }
    $named = Join-Path (Join-Path $probes 'EchoReplay') 'done.json'
    if (Test-Path -LiteralPath $named -PathType Leaf) { return $named }
    return $null
}

function Resolve-CleanL5([string]$launchDir) {
    $refuse = {
        param([string]$why)
        [pscustomobject]@{ Verdict = 'REFUSED'; Clauses = @(); Why = $why }
    }
    if ([string]::IsNullOrWhiteSpace($launchDir) -or -not (Test-Path -LiteralPath $launchDir -PathType Container)) {
        return & $refuse "no launch directory at '$launchDir'"
    }
    $launchPath = Join-Path $launchDir 'launch.json'
    $launchRec = Read-CleanL5Json $launchPath
    if ($null -eq $launchRec) { return & $refuse "clauses 1 and 2 read $launchPath, and it is absent or not readable as JSON" }
    if (-not (Test-JsonField $launchRec 'launch')) { return & $refuse "$launchPath has no 'launch' object, which is where clauses 1 and 2 read their fields" }
    $launch = $launchRec.launch

    $observerPath = Get-CleanL5ObserverPath $launchDir
    if ($null -eq $observerPath) {
        return & $refuse ("clauses 1, 2, 3 and 4 read the OBSERVER record " +
            "$launchDir\probes\EchoReplay\done.json - written by EchoReplay, NOT the egress record and NOT launch.json - and it is not on disk")
    }
    $observer = Read-CleanL5Json $observerPath
    if ($null -eq $observer) { return & $refuse "the observer record $observerPath is not readable as JSON" }

    $clauses = New-Object System.Collections.ArrayList

    # ---- clause 1 -----------------------------------------------------------------------------------
    $fields = New-Object System.Collections.ArrayList
    $missing = New-Object System.Collections.ArrayList
    foreach ($f in @('ProcessOutcome', 'KillReason', 'ProbesStillWaiting')) {
        if (Test-JsonField $launch $f) { [void]$fields.Add("launch.$f=" + (@($launch.$f) -join '|')) } else { [void]$missing.Add("launch.$f") }
    }
    foreach ($f in @('levelLoadAtMs', 'ownerDraws', 'ownerDrawsAtLevel')) {
        if (Test-JsonField $observer $f) { [void]$fields.Add("done.$f=" + [string]$observer.$f) } else { [void]$missing.Add("done.$f") }
    }
    $why = @()
    if ((Test-JsonField $launch 'ProcessOutcome') -and ([string]$launch.ProcessOutcome -ne 'killed-by-harness')) { $why += 'ProcessOutcome is not killed-by-harness' }
    if ((Test-JsonField $launch 'KillReason') -and ([string]$launch.KillReason -ne 'probes-complete')) { $why += 'KillReason is not probes-complete' }
    if ((Test-JsonField $launch 'ProbesStillWaiting') -and (@($launch.ProbesStillWaiting).Count -ne 0)) { $why += 'ProbesStillWaiting is not empty' }
    # The draw test needs BOTH counters present: one of them alone cannot say whether the panel drew
    # after the level, and an absent counter read as zero is the defect this vocabulary exists to remove.
    if ((Test-JsonField $observer 'ownerDraws') -and (Test-JsonField $observer 'ownerDrawsAtLevel') -and
        ((([int]$observer.ownerDraws) - ([int]$observer.ownerDrawsAtLevel)) -lt 1)) { $why += 'the panel was not drawn after the level loaded' }
    [void]$clauses.Add((Resolve-CleanL5Clause 1 $fields $missing $why 'an absent field is not a negative answer, and no field that IS present is negative'))

    # ---- clause 2 -----------------------------------------------------------------------------------
    $fields = New-Object System.Collections.ArrayList
    $missing = New-Object System.Collections.ArrayList
    if (Test-JsonField $launch 'Contaminated') { [void]$fields.Add('launch.Contaminated=' + [string]$launch.Contaminated) } else { [void]$missing.Add('launch.Contaminated') }
    if (Test-JsonField $observer 'batteryRan') { [void]$fields.Add('done.batteryRan=' + [string]$observer.batteryRan) } else { [void]$missing.Add('done.batteryRan') }
    $why = @()
    if ((Test-JsonField $launch 'Contaminated') -and ([bool]$launch.Contaminated)) { $why += 'the launch is contaminated' }
    if ((Test-JsonField $observer 'batteryRan') -and (-not [bool]$observer.batteryRan)) { $why += 'batteryRan is false: the probe battery did not execute' }
    [void]$clauses.Add((Resolve-CleanL5Clause 2 $fields $missing $why 'a run that did nothing contaminates nothing, so Contaminated alone certifies nothing'))

    # ---- clause 3 -----------------------------------------------------------------------------------
    # The verdict is DERIVED from rows[] here, never read from the record's own verdict string: a run in
    # which every row is could-not-ask must fail this clause on its own text rather than ride a
    # non-FAIL aggregate.
    $fields = New-Object System.Collections.ArrayList
    $missing = New-Object System.Collections.ArrayList
    if (Test-JsonField $observer 'provisional') { [void]$fields.Add('done.provisional=' + [string]$observer.provisional) } else { [void]$missing.Add('done.provisional') }
    $rowSet = Get-CleanL5Rows $observer
    if ($null -eq $rowSet) { [void]$missing.Add('done.rows[] (and no beside-phase answers[])') }
    $why = @()
    if ((Test-JsonField $observer 'provisional') -and ([bool]$observer.provisional)) { $why += 'provisional is true: the record could not be asked' }
    if ($null -ne $rowSet) {
        $rows = @($rowSet.Rows)
        [void]$fields.Add('scored from ' + $rowSet.Source)
        $declaredBy = 'declaration flag'
        if (@($rows | Where-Object { Test-JsonField $_ 'declaration' }).Count -eq 0) { $declaredBy = 'named fallback (' + ($script:DeclarationRowIds -join ', ') + ')' }
        $failed = @($rows | Where-Object { [string]$_.state -eq 'asked-fail' })
        # The inner call is parenthesised on its own. Without that, PowerShell parses the sub-expression
        # in COMMAND mode -- it opens with a command name -- and passes '-and' and the second operand to
        # Test-JsonField as extra arguments, which it discards: the test degrades to 'does the row carry
        # the key', so declaration: false reads as a declaration row (AUDIT_W1J15 W1J15-1).
        $observations = @($rows | Where-Object {
                [string]$_.state -eq 'asked-pass' -and
                -not (((Test-JsonField $_ 'declaration') -and [bool]$_.declaration) -or ($script:DeclarationRowIds -contains [string]$_.id))
            })
        [void]$fields.Add("done.rows[]=$(@($rows).Count) row(s)")
        [void]$fields.Add('asked-fail=' + (@($failed | ForEach-Object { [string]$_.id }) -join '|'))
        [void]$fields.Add("non-declaration asked-pass=$(@($observations).Count)")
        [void]$fields.Add("declaration source=$declaredBy")
        if (@($rows).Count -eq 0) { $why += 'rows[] is empty' }
        if (@($failed).Count -gt 0) { $why += ('asked-fail: ' + (@($failed | ForEach-Object { [string]$_.id }) -join ', ')) }
        if (@($observations).Count -eq 0) { $why += 'no asked-pass row that is not a declaration row' }
    }
    [void]$clauses.Add((Resolve-CleanL5Clause 3 $fields $missing $why 'the scored rows are the clause; without them there is nothing to conjoin'))

    # ---- clause 4 -----------------------------------------------------------------------------------
    # Kept deliberately as a NAMED SUBSET of clause 3: it says which row L5 exists to move, and it is
    # reported separately even when clause 3 has already failed.
    $fields = New-Object System.Collections.ArrayList
    $missing4 = New-Object System.Collections.ArrayList
    $why = @()
    $rowSet4 = Get-CleanL5Rows $observer
    if ($null -eq $rowSet4) {
        [void]$missing4.Add('done.rows[] (and no beside-phase answers[])')
        [void]$clauses.Add((Resolve-CleanL5Clause 4 $fields $missing4 $why 'the epoch row is read from rows[]'))
    }
    else {
        [void]$fields.Add('scored from ' + $rowSet4.Source)
        $epoch = @(@($rowSet4.Rows) | Where-Object { [string]$_.id -eq 'w2f-csregister-epoch' })[0]
        if ($null -eq $epoch) {
            [void]$missing4.Add('done.rows[].w2f-csregister-epoch')
            [void]$clauses.Add((Resolve-CleanL5Clause 4 $fields $missing4 $why 'the row L5 exists to move is not in the record'))
        }
        else {
            # The row's own state is in the same four-state vocabulary as the clause. asked-fail is a
            # present definite negative; could-not-ask is the row saying the question was never put, so
            # it is read as an absence and cannot make the clause not-clean.
            if (Test-JsonField $epoch 'state') {
                [void]$fields.Add('row.state=' + [string]$epoch.state)
                if ([string]$epoch.state -eq 'could-not-ask') { [void]$missing4.Add('done.rows[w2f-csregister-epoch] was not asked') }
                elseif ([string]$epoch.state -ne 'asked-pass') { $why += ('the row is ' + [string]$epoch.state) }
            }
            else { [void]$missing4.Add('done.rows[w2f-csregister-epoch].state') }
            $statusWord = ''
            foreach ($k in @('status', 'statusWord', 'value')) {
                if (Test-JsonField $epoch $k) { $statusWord = [string]$epoch.$k; [void]$fields.Add("row.$k=$statusWord"); break }
            }
            if (-not $statusWord) { [void]$missing4.Add('done.rows[w2f-csregister-epoch].status') }
            elseif ($script:CleanL5RunningWords -notcontains $statusWord) { $why += ("the status word is '$statusWord'") }
            if (Test-JsonField $epoch 'staleDetected') {
                [void]$fields.Add('row.staleDetected=' + [string]$epoch.staleDetected)
                if ([int]$epoch.staleDetected -ne 0) { $why += ('staleDetected is ' + [string]$epoch.staleDetected) }
            }
            else { [void]$missing4.Add('done.rows[w2f-csregister-epoch].staleDetected') }
            [void]$clauses.Add((Resolve-CleanL5Clause 4 $fields $missing4 $why 'absent is not zero'))
        }
    }

    $verdict = 'clean'
    if (@($clauses | Where-Object { $_.Result -eq 'not-clean' }).Count -gt 0) { $verdict = 'not-clean' }
    elseif (@($clauses | Where-Object { $_.Result -eq 'could-not-ask' }).Count -gt 0) { $verdict = 'could-not-ask' }
    return [pscustomobject]@{ Verdict = $verdict; Clauses = @($clauses); Why = '' }
}

function Write-CleanL5Report($result) {
    foreach ($c in @($result.Clauses)) {
        $colour = 'Gray'
        if ($c.Result -eq 'not-clean') { $colour = 'Red' }
        elseif ($c.Result -eq 'could-not-ask') { $colour = 'Yellow' }
        Write-Host ("clause {0}: {1,-13} read [{2}]" -f $c.Number, $c.Result, $c.Fields) -ForegroundColor $colour
        if ($c.MissingField) {
            # On a not-clean clause the absent fields are named but did not decide it, and saying so is the
            # difference between a reader waiting for a field and a reader stopping.
            $label = $(if ($c.Result -eq 'not-clean') { 'absent, and outranked by the answer above: ' } else { 'absent: ' })
            Write-Host ('           ' + $label + $c.MissingField) -ForegroundColor Yellow
        }
        if ($c.Why) { Write-Host ('           ' + $c.Why) }
    }
    if ($result.Why) { Write-Host ('REFUSED: ' + $result.Why) -ForegroundColor Red }
    Write-Host ('CLEAN-L5: ' + $result.Verdict)
}

function Invoke-CleanL5SelfTest {
    $checked = 0
    $failures = New-Object System.Collections.ArrayList
    $root = Join-Path ([IO.Path]::GetTempPath()) ('cleanl5-' + [guid]::NewGuid().ToString('N'))
    function Fixture([hashtable]$launchEdit, [hashtable]$doneEdit, [string[]]$dropLaunch, [string[]]$dropDone, [array]$rows) {
        $dir = Join-Path $root ([guid]::NewGuid().ToString('N'))
        $null = New-Item -ItemType Directory -Path (Join-Path $dir 'probes\EchoReplay') -Force
        $launch = [ordered]@{ ProcessOutcome = 'killed-by-harness'; KillReason = 'probes-complete'
            ProbesStillWaiting = @(); Contaminated = $false
        }
        foreach ($k in @($launchEdit.Keys)) { $launch[$k] = $launchEdit[$k] }
        foreach ($k in @($dropLaunch)) { $launch.Remove($k) }
        $done = [ordered]@{ levelLoadAtMs = 1200; ownerDraws = 90; ownerDrawsAtLevel = 12
            batteryRan = $true; provisional = $false
            rows = $(if ($null -eq $rows) {
                    @(
                        [ordered]@{ id = 'w1j-launch-window-declared'; state = 'asked-pass'; declaration = $true }
                        [ordered]@{ id = 'w1c-sc7-text-diff'; state = 'asked-pass' }
                        [ordered]@{ id = 'w2f-csregister-epoch'; state = 'asked-pass'; status = 'Running'; staleDetected = 0 }
                    )
                } else { $rows })
        }
        foreach ($k in @($doneEdit.Keys)) { $done[$k] = $doneEdit[$k] }
        foreach ($k in @($dropDone)) { $done.Remove($k) }
        Set-Content -LiteralPath (Join-Path $dir 'launch.json') -Encoding UTF8 -Value (
            [pscustomobject]@{ launch = [pscustomobject]$launch } | ConvertTo-Json -Depth 12)
        Set-Content -LiteralPath (Join-Path $dir 'probes\EchoReplay\done.json') -Encoding UTF8 -Value (
            [pscustomobject]$done | ConvertTo-Json -Depth 12)
        return $dir
    }
    function Expect([string]$name, [string]$expectVerdict, [int]$clause, [string]$expectClause, [string]$dir) {
        $script:sfChecked++
        $r = Resolve-CleanL5 $dir
        $got = [string]$r.Verdict
        $gotClause = ''
        if ($clause -gt 0) { $gotClause = [string](@($r.Clauses | Where-Object { $_.Number -eq $clause })[0]).Result }
        if ($got -eq $expectVerdict -and ($clause -eq 0 -or $gotClause -eq $expectClause)) {
            Write-Host ("ok   {0}" -f $name)
        }
        else {
            [void]$script:sfFailures.Add("$name : expected $expectVerdict/clause$clause=$expectClause, got $got/$gotClause")
            Write-Host ("FAIL {0} : expected {1}/clause{2}={3}, got {4}/{5}" -f $name, $expectVerdict, $clause, $expectClause, $got, $gotClause)
        }
    }
    function Assert([string]$name, [bool]$cond, [string]$detail) {
        $script:sfChecked++
        if ($cond) { Write-Host ("ok   {0}" -f $name) }
        else { [void]$script:sfFailures.Add("$name : $detail"); Write-Host ("FAIL {0} : {1}" -f $name, $detail) }
    }
    $script:sfChecked = 0
    $script:sfFailures = $failures

    Expect 'all four clauses clean' 'clean' 0 '' (Fixture @{} @{} @() @() $null)
    # clause 1: the three states, the other three clauses held clean
    Expect 'clause 1 not-clean: no draw after the level' 'not-clean' 1 'not-clean' (Fixture @{} @{ ownerDrawsAtLevel = 90 } @() @() $null)
    Expect 'clause 1 not-clean: the process was not killed by the harness' 'not-clean' 1 'not-clean' (Fixture @{ ProcessOutcome = 'crashed' } @{} @() @() $null)
    Expect 'clause 1 could-not-ask: ownerDraws absent' 'could-not-ask' 1 'could-not-ask' (Fixture @{} @{} @() @('ownerDraws') $null)
    Expect 'clause 1 could-not-ask: levelLoadAtMs absent' 'could-not-ask' 1 'could-not-ask' (Fixture @{} @{} @() @('levelLoadAtMs') $null)
    # clause 2
    Expect 'clause 2 not-clean: batteryRan false' 'not-clean' 2 'not-clean' (Fixture @{} @{ batteryRan = $false } @() @() $null)
    Expect 'clause 2 not-clean: contaminated' 'not-clean' 2 'not-clean' (Fixture @{ Contaminated = $true } @{} @() @() $null)
    Expect 'clause 2 could-not-ask: batteryRan absent' 'could-not-ask' 2 'could-not-ask' (Fixture @{} @{} @() @('batteryRan') $null)
    # clause 3
    Expect 'clause 3 not-clean: one asked-fail row' 'not-clean' 3 'not-clean' (Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1c-sc7-text-diff'; state = 'asked-fail' }
            [ordered]@{ id = 'w2f-csregister-epoch'; state = 'asked-pass'; status = 'Running'; staleDetected = 0 }))
    Expect 'clause 3 not-clean: every row could-not-ask' 'not-clean' 3 'not-clean' (Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1c-sc7-text-diff'; state = 'could-not-ask' }
            [ordered]@{ id = 'w2f-csregister-epoch'; state = 'could-not-ask' }))
    Expect 'clause 3 not-clean: the only asked-pass row is a declaration row' 'not-clean' 3 'not-clean' (Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1j-launch-window-declared'; state = 'asked-pass'; declaration = $true }
            [ordered]@{ id = 'w2f-csregister-epoch'; state = 'could-not-ask' }))
    # W1J15-1: the flag's VALUE decides, not its presence. ARCH-14 B9 makes `declaration` REQUIRED on
    # every emitted row, so once W2-R emits it this is the shape of every real record - and a resolver
    # that reads presence returns not-clean on all of them, forever.
    Expect 'clause 3 clean: declaration false on every observation, true on the declared row' 'clean' 3 'clean' (Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1j-launch-window-declared'; state = 'asked-pass'; declaration = $true }
            [ordered]@{ id = 'w1c-sc7-text-diff'; state = 'asked-pass'; declaration = $false }
            [ordered]@{ id = 'w2f-csregister-epoch'; state = 'asked-pass'; declaration = $false; status = 'Running'; staleDetected = 0 }))
    Expect 'clause 3 not-clean: the same rows with declaration TRUE on all of them' 'not-clean' 3 'not-clean' (Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1j-launch-window-declared'; state = 'asked-pass'; declaration = $true }
            [ordered]@{ id = 'w1c-sc7-text-diff'; state = 'asked-pass'; declaration = $true }
            [ordered]@{ id = 'w2f-csregister-epoch'; state = 'asked-pass'; declaration = $true; status = 'Running'; staleDetected = 0 }))
    Expect 'CONTROL: the same row set with the declaration row scored as an observation is clean for clause 3' 'could-not-ask' 3 'clean' (Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1j-launch-window-observed'; state = 'asked-pass' }
            [ordered]@{ id = 'w2f-csregister-epoch'; state = 'could-not-ask' }))
    Expect 'clause 3 not-clean: provisional' 'not-clean' 3 'not-clean' (Fixture @{} @{ provisional = $true } @() @() $null)
    Expect 'clause 3 could-not-ask: rows[] absent' 'could-not-ask' 3 'could-not-ask' (Fixture @{} @{} @() @('rows') $null)
    # clause 4
    Expect 'clause 4 not-clean: staleDetected is 1' 'not-clean' 4 'not-clean' (Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1c-sc7-text-diff'; state = 'asked-pass' }
            [ordered]@{ id = 'w2f-csregister-epoch'; state = 'asked-pass'; status = 'Running'; staleDetected = 1 }))
    Expect 'clause 4 could-not-ask: staleDetected absent is not zero' 'could-not-ask' 4 'could-not-ask' (Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1c-sc7-text-diff'; state = 'asked-pass' }
            [ordered]@{ id = 'w2f-csregister-epoch'; state = 'asked-pass'; status = 'Running' }))
    Expect 'clause 4 not-clean: a status word outside the three' 'not-clean' 4 'not-clean' (Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1c-sc7-text-diff'; state = 'asked-pass' }
            [ordered]@{ id = 'w2f-csregister-epoch'; state = 'asked-pass'; status = 'Stopped'; staleDetected = 0 }))
    Expect 'clause 4 could-not-ask: the epoch row is not in the record' 'could-not-ask' 4 'could-not-ask' (Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1c-sc7-text-diff'; state = 'asked-pass' }))
    # W1J15-2: A PRESENT DEFINITE NEGATIVE BEATS AN ABSENT FIELD (section 8.4a.4 rule 5 amendment (a)).
    # One case per clause with a present negative AND an absent field - the shape none of the cases
    # above build, and the shape every real record has until ARCH-13-C2 ships the observer counters.
    Expect 'clause 1 not-clean from KillReason even though levelLoadAtMs is absent' 'not-clean' 1 'not-clean' (
        Fixture @{ KillReason = 'timeout' } @{} @() @('levelLoadAtMs') $null)
    Expect 'clause 1 not-clean from ProcessOutcome even though ownerDraws is absent' 'not-clean' 1 'not-clean' (
        Fixture @{ ProcessOutcome = 'crashed' } @{} @() @('ownerDraws') $null)
    Expect 'clause 1 not-clean from ProbesStillWaiting even though all three observer counters are absent' 'not-clean' 1 'not-clean' (
        Fixture @{ ProbesStillWaiting = @('W2-R-browser: still working') } @{} @() @('levelLoadAtMs', 'ownerDraws', 'ownerDrawsAtLevel') $null)
    Expect 'CONTROL: clause 1 could-not-ask when the absent field is all there is' 'could-not-ask' 1 'could-not-ask' (
        Fixture @{} @{} @('KillReason') @() $null)
    Expect 'clause 2 not-clean from Contaminated even though batteryRan is absent' 'not-clean' 2 'not-clean' (
        Fixture @{ Contaminated = $true } @{} @() @('batteryRan') $null)
    Expect 'clause 3 not-clean from provisional even though rows[] is absent' 'not-clean' 3 'not-clean' (
        Fixture @{} @{ provisional = $true } @() @('rows') $null)
    Expect 'clause 4 not-clean from an asked-fail epoch row even though staleDetected is absent' 'not-clean' 4 'not-clean' (
        Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1c-sc7-text-diff'; state = 'asked-pass' }
            [ordered]@{ id = 'w2f-csregister-epoch'; state = 'asked-fail'; status = 'Running' }))
    Expect 'CONTROL: a could-not-ask epoch row is an absence, not a negative' 'could-not-ask' 4 'could-not-ask' (
        Fixture @{} @{} @() @() @(
            [ordered]@{ id = 'w1c-sc7-text-diff'; state = 'asked-pass' }
            [ordered]@{ id = 'w2f-csregister-epoch'; state = 'could-not-ask' }))
    # the artifacts themselves
    $noObserver = Fixture @{} @{} @() @() $null
    Remove-Item -LiteralPath (Join-Path $noObserver 'probes\EchoReplay\done.json') -Force
    Expect 'a missing observer record is REFUSED, not not-clean' 'REFUSED' 0 '' $noObserver
    $noLaunch = Fixture @{} @{} @() @() $null
    Remove-Item -LiteralPath (Join-Path $noLaunch 'launch.json') -Force
    Expect 'a missing launch.json is REFUSED' 'REFUSED' 0 '' $noLaunch

    # THE RETROACTIVE CONTROL section 8.4a.4 rule 5 mandates, run against the record that produced the
    # finding: killed on a timeout with probes still waiting, batteryRan false, w1c-sc7-text-diff
    # asked-fail, and the epoch row never asked. It is a launching-machine artifact and gitignored, the
    # same class as runs\replays\W2-G-fixture.echodemo, so its absence is REPORTED by name here rather
    # than skipped: a control that quietly does not run is the reason this resolver exists.
    $retro = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'runs\20260902-071934Z\launch-01'
    if (-not (Test-Path -LiteralPath $retro -PathType Container)) {
        Assert 'RETROACTIVE CONTROL runs/20260902-071934Z/launch-01' $false "$retro is not on disk - gitignored launching-machine artifact, so the control section 8.4a.4 rule 5 mandates could not be run"
    }
    else {
        $rr = Resolve-CleanL5 $retro
        foreach ($want in @(@(1, 'not-clean'), @(2, 'not-clean'), @(3, 'not-clean'), @(4, 'could-not-ask'))) {
            $c = @($rr.Clauses | Where-Object { $_.Number -eq $want[0] })[0]
            Assert ("RETROACTIVE CONTROL: clause $($want[0]) is $($want[1])") ([string]$c.Result -eq [string]$want[1]) ("got $([string]$c.Result) [$($c.Fields)] $($c.Why)")
        }
        $c1 = @($rr.Clauses | Where-Object { $_.Number -eq 1 })[0]
        Assert 'RETROACTIVE CONTROL: clause 1 read KillReason=timeout' ([bool]($c1.Fields -match 'KillReason=timeout')) $c1.Fields
        Assert 'RETROACTIVE CONTROL: clause 1 read a non-empty ProbesStillWaiting' ([bool]($c1.Fields -match 'ProbesStillWaiting=\S')) $c1.Fields
        Assert 'RETROACTIVE CONTROL: and the verdict is not-clean' ([string]$rr.Verdict -eq 'not-clean') ([string]$rr.Verdict)
    }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host ("CLEANL5-SELFTEST checked={0} failures={1}" -f $script:sfChecked, $script:sfFailures.Count)
    foreach ($f in $script:sfFailures) { Write-Host ('  ' + $f) }
    if ($script:sfFailures.Count -gt 0) { return 1 }
    return 0
}

if ($DefineOnly) { return }
if ($SelfTest) { exit (Invoke-CleanL5SelfTest) }
if (-not $LaunchDir) { Write-Host 'usage: Resolve-CleanL5.ps1 -LaunchDir <runs\<ts>\launch-NN> | -SelfTest | -DefineOnly'; exit 3 }
$result = Resolve-CleanL5 $LaunchDir
Write-CleanL5Report $result
if ($result.Verdict -eq 'clean') { exit 0 }
if ($result.Verdict -eq 'not-clean') { exit 2 }
exit 3
