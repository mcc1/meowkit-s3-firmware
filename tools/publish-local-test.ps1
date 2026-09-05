<#
.SYNOPSIS
    Publish the current build to the installer's local-test channel WITHOUT a
    window in which the channel directory does not exist.

.DESCRIPTION
    publish-firmware.ps1 writes into <installer>\generated\<channel>\ and refuses
    to overwrite an existing artifact, so the naive flow was "move the old
    directory to attic, then publish" - during those seconds the web installer
    got HTTP 404 for manifest.json or for a half-downloaded factory image
    ("Failed to download manifest", "flash failed half-way").

    This wrapper publishes into a staging root first, then swaps directories
    with two renames (old -> attic, staging -> live). The live path is missing
    for milliseconds instead of seconds, and a browser that already started
    downloading the old image keeps its open file handle.

.PARAMETER Version
    Version string written into manifest/metadata (e.g. ac-remote-20260906e).

.PARAMETER SkipBuild
    Passed through to publish-firmware.ps1 (use the existing .pio artifacts).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$firmwareRoot  = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$installerRoot = (Resolve-Path (Join-Path $firmwareRoot '..\meowkit-s3-installer')).Path
$generated     = Join-Path $installerRoot 'generated'
$staging       = Join-Path $generated '.staging'
$live          = Join-Path $generated 'local-test'
$attic         = Join-Path $generated 'attic'

if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force $staging, $attic | Out-Null

$args = @('-Channel', 'local-test', '-Version', $Version, '-OutputRoot', $staging, '-Force')
if ($SkipBuild) { $args += '-SkipBuild' }
& pwsh -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'publish-firmware.ps1') @args
if ($LASTEXITCODE -ne 0) { throw "publish-firmware.ps1 failed ($LASTEXITCODE)" }

$stagedChannel = Join-Path $staging 'local-test'
foreach ($f in 'manifest.json', 'metadata.json', 'meowkit-s3-local-test-factory.bin') {
    if (-not (Test-Path (Join-Path $stagedChannel $f))) { throw "staging is missing $f" }
}

# Swap: old live -> attic (named by its previous version), staging -> live.
if (Test-Path $live) {
    $oldVersion = 'unknown'
    try { $oldVersion = (Get-Content (Join-Path $live 'metadata.json') -Raw | ConvertFrom-Json).version } catch {}
    $dest = Join-Path $attic ("local-test-" + (Get-Date -Format 'yyyyMMdd-HHmm') + "-" + $oldVersion)
    Move-Item $live $dest
}
Move-Item $stagedChannel $live
Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue

Write-Host "local-test now serves $Version" -ForegroundColor Green
Write-Host "  $live"
