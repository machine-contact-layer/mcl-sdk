<#
    Write the MCL dual-transport peer application to the DFR1154 application partition.

    This writes the application partition at 0x20000 and nothing else. The
    bootloader at 0x0, the partition table at 0x8000 and the NVS/OTA data are
    left untouched, which is what makes the operation reversible.

    Uploading through the Arduino toolchain is deliberately not used here: its
    upload step can also rewrite the bootloader and the partition table, which
    would make the factory application unrecoverable from an application-only
    backup.

    The script refuses to run unless a factory application backup exists, so a
    board can never be reflashed into a state it cannot be returned from.
#>
param(
    [string]$PortName = 'COM3',
    [string]$Image = '',
    [string]$BackupImage = (Join-Path $env:USERPROFILE 'Downloads\MCL_DFR1154_BACKUP_20260902\dfr1154-factory-app-before-mcl.bin'),
    [string]$EsptoolPath = 'esptool',
    [int]$Baud = 921600
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($Image)) {
    $Image = Join-Path $scriptDir 'build\out\esp32_dual_peer.ino.bin'
}

if (-not (Test-Path -LiteralPath $Image -PathType Leaf)) {
    throw "Application image not found: $Image. Run build-firmware.ps1 first."
}

# A board with no retained factory image must not be reflashed.
if (-not (Test-Path -LiteralPath $BackupImage -PathType Leaf)) {
    throw "Factory application backup not found: $BackupImage. Refusing to flash a board that cannot be restored."
}

# The default 16M partition scheme gives the application 0x140000 bytes.
$appPartitionSize = 0x140000
$imageBytes = (Get-Item -LiteralPath $Image).Length
if ($imageBytes -gt $appPartitionSize) {
    throw "Image is $imageBytes bytes, which exceeds the $appPartitionSize byte application partition."
}

# Confirm the port really is the ESP32-S3 native USB device before writing.
$device = Get-CimInstance Win32_PnPEntity |
    Where-Object { $_.Name -match "\($([regex]::Escape($PortName))\)$" -and $_.PNPDeviceID -like 'USB\VID_303A&PID_1001*' } |
    Select-Object -First 1
if ($null -eq $device) {
    throw "$PortName is not the expected ESP32-S3 native USB CDC/JTAG device (VID 303A, PID 1001)"
}

$imageHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Image).Hash
Write-Host "IMAGE=$Image"
Write-Host "IMAGE_SHA256=$imageHash"
Write-Host "IMAGE_BYTES=$imageBytes"
Write-Host "PORT=$PortName"
Write-Host 'TARGET=0x20000 (application partition only)' -ForegroundColor Yellow

& $EsptoolPath --chip esp32s3 --port $PortName --baud $Baud write-flash 0x20000 $Image
if ($LASTEXITCODE -ne 0) { throw "esptool write-flash failed with exit code $LASTEXITCODE" }

Write-Host ''
Write-Host 'Application partition written. Bootloader and partition table untouched.' -ForegroundColor Green
Write-Host 'Restore the factory application with RESTORE_DFR1154_APP.cmd when the campaign ends.'
