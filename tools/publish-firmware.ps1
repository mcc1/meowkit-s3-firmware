[CmdletBinding()]
param(
    [ValidateSet('local-test', 'stable')]
    [string]$Channel = 'local-test',

    [string]$Version,

    [switch]$SkipBuild,

    [switch]$Force,

    [string]$InstallerRoot,

    [string]$OutputRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$firmwareRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$workspaceRoot = (Resolve-Path (Join-Path $firmwareRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($InstallerRoot)) {
    $InstallerRoot = [IO.Path]::Combine($workspaceRoot, 'meowkit-s3-installer')
}
$installerRoot = $InstallerRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = [IO.Path]::Combine($installerRoot, 'generated')
}
$outputRoot = $OutputRoot
$buildRoot = [IO.Path]::Combine($firmwareRoot, '.pio', 'build', 'esp32s3box')

function Invoke-External {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $false)]
        [string[]]$Arguments = @()
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "外部命令失敗（$LASTEXITCODE）：$FilePath $($Arguments -join ' ')"
    }
}

function Get-CommandPath {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Names
    )

    foreach ($name in $Names) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($null -ne $command) {
            return $command.Source
        }
    }

    return $null
}

if (-not (Test-Path -LiteralPath $installerRoot -PathType Container)) {
    throw "找不到 installer repo：$installerRoot"
}

if ($Channel -eq 'stable') {
    if ([string]::IsNullOrWhiteSpace($Version)) {
        throw 'stable 發布必須指定 -Version，例如 -Version 1.0.1'
    }
    if ($Version -notmatch '^\d+\.\d+\.\d+$') {
        throw 'stable 的 -Version 必須是三段版本號，例如 1.0.1'
    }
} elseif ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = 'local-test-' + (Get-Date -Format 'yyyy-MM-dd-HHmmss')
}

$submodulePins = @(
    @{ Path = 'lib/mooncake'; Commit = '0dfc177b72fcc55095fd222b0bdc69595b001d9f' },
    @{ Path = 'lib/IRremoteESP8266'; Commit = '3390e72877ac15c74f602151de4a2fb9a154bdb0' },
    @{ Path = 'lib/ESP32-BLE-Mouse'; Commit = 'ba38caa695cece3cf06ff67b203e9400efe546f4' },
    @{ Path = 'lib/arduinoFFT'; Commit = '6b3ef9732db800bb2c658eefb7ad8939eb44e6e2' }
)

$git = Get-CommandPath @('git.exe', 'git')
if ($null -eq $git) {
    throw '找不到 git。請先安裝 Git，再重新執行。'
}

Write-Host '同步並初始化 firmware submodules...'
Invoke-External $git @('-C', $firmwareRoot, 'submodule', 'update', '--init', '--recursive')

foreach ($pin in $submodulePins) {
    $submodulePath = Join-Path $firmwareRoot $pin.Path
    if (-not (Test-Path -LiteralPath $submodulePath -PathType Container)) {
        throw "submodule 尚未存在：$($pin.Path)"
    }

    $actualCommit = (& $git -C $submodulePath rev-parse HEAD 2>$null).Trim()
    if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $pin.Commit) {
        throw "submodule commit 不符合固定版本：$($pin.Path)`n期待：$($pin.Commit)`n目前：$actualCommit"
    }
    Write-Host "  $($pin.Path) @ $actualCommit"
}

$python = Get-CommandPath @('python.exe', 'python', 'py.exe', 'py')
if ($null -eq $python) {
    throw '找不到 Python。請先安裝 Python 與 PlatformIO Core。'
}

$pythonPrefix = @()
if ([IO.Path]::GetFileName($python) -in @('py.exe', 'py')) {
    $pythonPrefix = @('-3')
}

if (-not $SkipBuild) {
    Write-Host '開始 PlatformIO build（不會 flash 裝置）...'
    Push-Location $firmwareRoot
    try {
        Invoke-External $python ($pythonPrefix + @('-m', 'platformio', 'run', '-e', 'esp32s3box'))
    } finally {
        Pop-Location
    }
} else {
    Write-Host '略過 build，使用現有 .pio\build\esp32s3box artifacts。'
}

$bootloader = Join-Path $buildRoot 'bootloader.bin'
$partitions = Join-Path $buildRoot 'partitions.bin'
$appFirmware = Join-Path $buildRoot 'firmware.bin'
foreach ($artifact in @($bootloader, $partitions, $appFirmware)) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "找不到 build artifact：$artifact"
    }
}

if ($Channel -eq 'stable') {
    $artifactDirectory = [IO.Path]::Combine($outputRoot, 'stable')
    $artifactName = "meowkit-s3-v$Version-factory.bin"
    $templatePath = [IO.Path]::Combine($installerRoot, 'templates', 'stable-manifest.json')
    $manifestName = 'MeowKit S3'
} else {
    $artifactDirectory = [IO.Path]::Combine($outputRoot, 'local-test')
    $artifactName = 'meowkit-s3-local-test-factory.bin'
    $templatePath = [IO.Path]::Combine($installerRoot, 'templates', 'local-test-manifest.json')
    $manifestName = 'MeowKit S3 (local test build)'
}

$factoryImage = Join-Path $artifactDirectory $artifactName
if ((Test-Path -LiteralPath $factoryImage -PathType Leaf) -and -not $Force) {
    throw "目標 artifact 已存在：$factoryImage`n若要覆寫，請加上 -Force。"
}

New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
    throw "找不到 manifest template：$templatePath"
}

$userProfile = [Environment]::GetFolderPath('UserProfile')
$esptoolCandidates = @(
    ([IO.Path]::Combine($userProfile, '.platformio', 'packages', 'tool-esptoolpy', 'esptool.py')),
    ([IO.Path]::Combine($firmwareRoot, '.pio', 'packages', 'tool-esptoolpy', 'esptool.py'))
)
$esptool = $esptoolCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if ($null -eq $esptool) {
    throw '找不到 PlatformIO 內附的 esptool.py。請先成功安裝 PlatformIO platform/tool dependencies。'
}

Write-Host "合併 factory image：$factoryImage"
$mergeArguments = @(
    $esptool,
    '--chip', 'esp32s3',
    'merge_bin',
    '-o', $factoryImage,
    '0x0', $bootloader,
    '0x8000', $partitions,
    '0x10000', $appFirmware
)
Invoke-External $python ($pythonPrefix + $mergeArguments)

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $factoryImage).Hash.ToLowerInvariant()
$checksumsPath = [IO.Path]::Combine($artifactDirectory, 'SHA256SUMS.txt')
$manifestPath = [IO.Path]::Combine($artifactDirectory, 'manifest.json')
$metadataPath = [IO.Path]::Combine($artifactDirectory, 'metadata.json')
$utf8NoBom = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($checksumsPath, "$hash  $artifactName`n", $utf8NoBom)

$manifest = Get-Content -Raw -LiteralPath $templatePath | ConvertFrom-Json
$manifest.name = $manifestName
$manifest.version = $Version
$manifest.builds[0].parts[0].path = $artifactName
$manifest.builds[0].parts[0].offset = 0
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 10) + [Environment]::NewLine, $utf8NoBom)

$firmwareCommit = (& $git -C $firmwareRoot rev-parse HEAD 2>$null).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($firmwareCommit)) {
    throw "無法取得 firmware source commit：$firmwareRoot"
}
$metadata = [ordered]@{
    schemaVersion = 1
    channel = $Channel
    version = $Version
    manifest = 'manifest.json'
    artifact = $artifactName
    sha256 = $hash
    firmwareCommit = $firmwareCommit
    generatedAt = (Get-Date).ToUniversalTime().ToString('o')
}
[IO.File]::WriteAllText($metadataPath, ($metadata | ConvertTo-Json -Depth 10) + [Environment]::NewLine, $utf8NoBom)

Write-Host ''
Write-Host '發布 artifact 已準備完成（尚未 commit、push、deploy 或 flash）：'
Write-Host "  Channel : $Channel"
Write-Host "  Version : $Version"
Write-Host "  Image   : $factoryImage"
Write-Host "  SHA256  : $hash"
Write-Host "  Metadata: $metadataPath"
Write-Host 'index.html 不會被修改；local Web Installer 重新整理即可載入 generated metadata。'
