[CmdletBinding()]
param(
    [string]$Port = 'COM6',

    [int]$BaudRate = 9600,

    [string]$LogPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$availablePorts = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
if ($availablePorts -notcontains $Port) {
    $listedPorts = if ($availablePorts.Count -gt 0) { $availablePorts -join ', ' } else { '(沒有偵測到)' }
    throw "找不到 $Port。目前可用的 serial ports：$listedPorts"
}

$serial = [System.IO.Ports.SerialPort]::new(
    $Port,
    $BaudRate,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$serial.ReadTimeout = 100
$serial.DtrEnable = $false
$serial.RtsEnable = $false

$writer = $null
try {
    if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
        $logDirectory = Split-Path -Parent $LogPath
        if (-not [string]::IsNullOrWhiteSpace($logDirectory)) {
            New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
        }
        $writer = [System.IO.StreamWriter]::new($LogPath, $true, [Text.UTF8Encoding]::new($false))
        $writer.AutoFlush = $true
    }

    try {
        $serial.Open()
    } catch {
        throw "無法開啟 $Port。請確認 MeowKit 在正常 firmware 模式、USB 線仍連接，並關閉其他 serial monitor。詳細錯誤：$($_.Exception.Message)"
    }
    Write-Host "Serial monitor connected: $Port @ $BaudRate baud"
    Write-Host '按 Ctrl+C 結束；若出現 Access denied，請先關閉其他佔用 COM port 的程式。'

    while ($true) {
        $text = $serial.ReadExisting()
        if ($text.Length -gt 0) {
            [Console]::Write($text)
            if ($null -ne $writer) {
                $writer.Write($text)
            }
        }
        Start-Sleep -Milliseconds 50
    }
} finally {
    if ($null -ne $writer) {
        $writer.Dispose()
    }
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
