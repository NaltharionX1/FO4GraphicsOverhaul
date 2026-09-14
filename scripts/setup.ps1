#Requires -Version 5.1
<#
.SYNOPSIS
    Fetches every build dependency at its pinned commit, applies the local patches, and downloads NVIDIA's
    redistributable DLSS runtimes from the official Streamline SDK release.

.PARAMETER SkipRuntimes
    Skip the NVIDIA runtime download (the plugin still builds; DLSS will report its runtime missing).

.PARAMETER Force
    Delete and re-fetch dependencies that are already present.
#>
[CmdletBinding()]
param(
    [switch]$SkipRuntimes,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content (Join-Path $root 'dependencies.json') -Raw | ConvertFrom-Json
$extern = Join-Path $root 'extern'
New-Item -ItemType Directory -Force -Path $extern | Out-Null

function Invoke-Git {
    param([string]$WorkDir, [string[]]$Arguments)
    & git -C $WorkDir @Arguments
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed in $WorkDir" }
}

foreach ($dep in $manifest.sources) {
    $dir = Join-Path $extern $dep.name
    $marker = Join-Path $dir '.fo4go-pinned'
    $pin = "$($dep.commit) sparse=$($dep.sparse -join ',') patches=$($dep.patches -join ',')"
    if ((Test-Path $marker) -and -not $Force) {
        if ((Get-Content $marker -Raw).Trim() -eq $pin) {
            Write-Host "[ok]    $($dep.name) @ $($dep.commit.Substring(0, 12))"
            continue
        }
    }
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Write-Host "[fetch] $($dep.name) @ $($dep.commit.Substring(0, 12))"
    Invoke-Git $dir @('init', '--quiet')
    Invoke-Git $dir @('config', 'core.longpaths', 'true')
    Invoke-Git $dir @('config', 'core.autocrlf', 'false')
    Invoke-Git $dir @('config', 'core.eol', 'lf')
    Invoke-Git $dir @('remote', 'add', 'origin', $dep.url)
    if ($dep.sparse.Count -gt 0) {
        Invoke-Git $dir @('sparse-checkout', 'set', '--no-cone')
        $patterns = $dep.sparse | ForEach-Object { "/$_" }
        Set-Content -Path (Join-Path $dir '.git/info/sparse-checkout') -Value $patterns -Encoding ascii
    }
    Invoke-Git $dir @('fetch', '--quiet', '--depth', '1', '--filter=blob:none', 'origin', $dep.commit)
    Invoke-Git $dir @('checkout', '--quiet', 'FETCH_HEAD')
    foreach ($patch in $dep.patches) {
        Write-Host "        patch $patch"
        Invoke-Git $dir @('apply', '--whitespace=nowarn', (Join-Path $root $patch))
    }
    Set-Content -Path $marker -Value $pin -Encoding ascii
}

if ($SkipRuntimes) {
    Write-Host '[skip]  NVIDIA runtimes'
    exit 0
}

foreach ($rt in $manifest.runtimes) {
    $to = Join-Path $root $rt.to
    $present = $true
    foreach ($f in $rt.files) {
        $dst = Join-Path $to (Split-Path -Leaf $f.from)
        if (-not (Test-Path $dst) -or (Get-FileHash -Algorithm SHA256 $dst).Hash.ToLower() -ne $f.sha256) { $present = $false }
    }
    if ($present -and -not $Force) {
        Write-Host "[ok]    $($rt.name)"
        continue
    }
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("fo4go-" + [guid]::NewGuid())
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        $zip = Join-Path $tmp 'archive.zip'
        Write-Host "[fetch] $($rt.name)"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $rt.archive -OutFile $zip -UseBasicParsing
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
        try {
            New-Item -ItemType Directory -Force -Path $to | Out-Null
            foreach ($f in $rt.files) {
                $entry = $archive.Entries | Where-Object { $_.FullName -eq $f.from -or $_.FullName.EndsWith('/' + $f.from) } | Select-Object -First 1
                if ($null -eq $entry) { throw "$($f.from) not found in $($rt.archive)" }
                $dst = Join-Path $to (Split-Path -Leaf $f.from)
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dst, $true)
                $hash = (Get-FileHash -Algorithm SHA256 $dst).Hash.ToLower()
                if ($hash -ne $f.sha256) {
                    Remove-Item -Force $dst
                    throw "SHA-256 mismatch for $($f.from): expected $($f.sha256), got $hash"
                }
                Write-Host "        $(Split-Path -Leaf $f.from) verified"
            }
        } finally {
            $archive.Dispose()
        }
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

Write-Host 'Dependencies ready. Next: cmake --preset release'
