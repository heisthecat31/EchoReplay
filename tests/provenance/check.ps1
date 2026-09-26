# Verifies third_party/PROVENANCE.md against what git actually stores, and against what the compiler
# actually reads.
#
# Nothing checked this document before, which made it the one artifact in the tree whose only job is to be
# true and whose truth nobody tested. It also hashed the bytes on disk, and core.autocrlf=true rewrites
# text files to CRLF in the working tree while the blob stays LF - so those digests could not be reproduced
# from a clean checkout at all. Blob ids are what git stores, so they are the same for everyone.
#
# The manifest is compared against the WORKING TREE copy, because that is the copy cmake compiles. Compared
# against the index alone, a vendored file edited and never staged went into EchoReplay.dll while this check
# reported clean. `git hash-object` applies the same clean filter the blob id encodes, so the two ids are
# directly comparable; the index is then compared as well, because a difference between it and the working
# tree is a commit away from making the manifest wrong.
#
# A source download (GitHub zip, git archive) has no index. There the vendored set is what is on disk and
# the blob ids are computed here, so the manifest is still checked against the bytes cmake compiles.
$ErrorActionPreference = 'Stop'
$repo = $env:REPLAY_REPO_ROOT
if (-not $repo) { Write-Host 'REPLAY_REPO_ROOT is not set'; exit 1 }

$manifestPath = Join-Path (Join-Path $repo 'third_party') 'PROVENANCE.md'
if (-not (Test-Path -LiteralPath $manifestPath)) { Write-Host "no $manifestPath"; exit 1 }

$declared = @{}
$inBlock = $false
foreach ($line in (Get-Content -LiteralPath $manifestPath)) {
    if ($line -match '^```') { $inBlock = -not $inBlock; continue }
    if (-not $inBlock) { continue }
    if ($line -match '^(?<sha>[0-9a-f]{40})\s\s(?<path>\S.*)$') { $declared[$matches['path'].Trim()] = $matches['sha'] }
}
if ($declared.Count -eq 0) { Write-Host 'PROVENANCE.md declares no blob rows; a manifest this check cannot read is not a manifest'; exit 1 }

function Test-IsVendoredFile([string]$rel) {
    # First-party and the manifest itself are deliberately outside the vendored set.
    return -not ($rel -eq 'PROVENANCE.md' -or $rel -eq 'doctest/doctest_main.cpp' -or $rel -eq 'CMakeLists.txt')
}

$latin1 = [Text.Encoding]::GetEncoding(28591)
function Get-BlobId([byte[]]$bytes) {
    $header = [Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0")
    $sha = [Security.Cryptography.SHA1]::Create()
    try {
        [void]$sha.TransformBlock($header, 0, $header.Length, $null, 0)
        [void]$sha.TransformFinalBlock($bytes, 0, $bytes.Length)
        return [BitConverter]::ToString($sha.Hash).Replace('-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }
}

$repoFull = [IO.Path]::GetFullPath($repo).TrimEnd('\', '/')
$git = Get-Command git.exe -ErrorAction SilentlyContinue
$isCheckout = $false
if ($git) {
    # Outside a repository git reports on stderr, which 'Stop' would turn into a terminating error.
    $ErrorActionPreference = 'Continue'
    $top = & $git.Source -C $repo rev-parse --show-toplevel 2>$null
    $topExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($topExit -eq 0 -and $top) {
        $isCheckout = [IO.Path]::GetFullPath(($top | Select-Object -First 1)).TrimEnd('\', '/') -eq $repoFull
    }
}

$indexed = $null
$actual = @{}
$onDisk = @{}
$missing = New-Object System.Collections.ArrayList
if (-not $isCheckout) {
    $vendorRoot = Join-Path $repoFull 'third_party'
    foreach ($file in @(Get-ChildItem -LiteralPath $vendorRoot -Recurse -File -Force)) {
        $rel = $file.FullName.Substring($vendorRoot.Length + 1).Replace('\', '/')
        if (-not (Test-IsVendoredFile $rel)) { continue }
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        $id = Get-BlobId $bytes
        # An archive extracted with core.autocrlf=true carries CRLF where the blob has LF; git hash-object
        # would undo that, so the LF form is accepted too.
        if ($declared.ContainsKey($rel) -and $declared[$rel] -ne $id -and [Array]::IndexOf($bytes, [byte]0) -lt 0) {
            $lfId = Get-BlobId ($latin1.GetBytes($latin1.GetString($bytes).Replace("`r`n", "`n")))
            if ($lfId -eq $declared[$rel]) { $id = $lfId }
        }
        $actual[$rel] = $id
        $onDisk[$rel] = $id
    }
    Write-Host 'not a git checkout: vendored set and blob ids taken from the files on disk'
}
else {
Push-Location -LiteralPath $repo
try {
    $indexed = & $git.Source ls-files -s -- third_party 2>$null
    if ($LASTEXITCODE -ne 0) { Write-Host 'git ls-files failed'; exit 1 }

    foreach ($line in $indexed) {
        $meta, $path = $line -split "`t", 2
        $parts = $meta -split '\s+'
        if ($parts.Count -lt 3 -or -not $path) { continue }
        $rel = $path.Trim() -replace '^third_party/', ''
        if (-not (Test-IsVendoredFile $rel)) { continue }
        $actual[$rel] = $parts[1]
    }

    $present = New-Object System.Collections.ArrayList
    foreach ($rel in @($actual.Keys | Sort-Object)) {
        if (Test-Path -LiteralPath (Join-Path $repo "third_party/$rel") -PathType Leaf) { [void]$present.Add($rel) }
        else { [void]$missing.Add($rel) }
    }
    if ($present.Count -gt 0) {
        $ids = @(& $git.Source hash-object -- @($present | ForEach-Object { "third_party/$_" }) 2>$null)
        if ($LASTEXITCODE -ne 0 -or $ids.Count -ne $present.Count) {
            Write-Host "git hash-object returned $($ids.Count) id(s) for $($present.Count) file(s)"
            exit 1
        }
        for ($i = 0; $i -lt $present.Count; $i++) { $onDisk[$present[$i]] = $ids[$i].Trim() }
    }
}
finally { Pop-Location }
}

$failures = 0
foreach ($rel in @($missing)) {
    Write-Host "ABSENT: $rel is tracked under third_party/ but is not in the working tree"
    $failures++
}
foreach ($path in @($declared.Keys | Sort-Object)) {
    if (-not $actual.ContainsKey($path)) { Write-Host "MISSING: $path is in PROVENANCE.md but not tracked under third_party/"; $failures++; continue }
    if (-not $onDisk.ContainsKey($path)) { continue }
    if ($onDisk[$path] -ne $declared[$path]) {
        Write-Host "CHANGED: $path compiles as $($onDisk[$path]), PROVENANCE.md says $($declared[$path])"
        $failures++
    }
}
foreach ($path in @($actual.Keys | Sort-Object)) {
    if (-not $declared.ContainsKey($path)) { Write-Host "UNRECORDED: $path is vendored but has no PROVENANCE.md row"; $failures++ }
}
foreach ($path in @($onDisk.Keys | Sort-Object)) {
    if ($onDisk[$path] -ne $actual[$path]) {
        Write-Host "UNSTAGED: $path is $($onDisk[$path]) in the working tree and $($actual[$path]) in the index; the build reads the working tree"
        $failures++
    }
}

Write-Host "GATE-RESULT checked=$($declared.Count + $actual.Count + $onDisk.Count) subject=vendored-blob-manifest"
if ($failures -gt 0) {
    Write-Host "$failures provenance row(s) wrong. Vendored bytes changed without the manifest being regenerated,"
    Write-Host 'or a file was added to third_party/ without being recorded.'
    exit 1
}
Write-Host "provenance clean: $($declared.Count) recorded, $($actual.Count) indexed, $($onDisk.Count) working-tree blob id(s), all in agreement"
exit 0
