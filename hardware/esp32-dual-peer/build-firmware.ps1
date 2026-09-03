<#
    Build the MCL dual-transport peer firmware for the DFR1154 (ESP32-S3).

    The sketch is not a copy of the protocol. This script stages the canonical
    mcl-wire, mcl-link, mcl-sdk, mcl-ip and mcl-ble sources into a throwaway
    build tree next to it, so the firmware and the host harness are provably the
    same code and cannot drift apart. The staging tree is deleted afterwards:
    the compiled image is the artifact, and a stale copy left behind is what
    eventually gets edited by mistake.

    This script COMPILES ONLY. It never uploads. Uploading through the Arduino
    toolchain can rewrite the bootloader and the partition table, which would
    make the factory application unrecoverable from an application-only backup.
    Flashing is done separately by flash-app-only.ps1, which writes the
    application partition and nothing else.
#>
param(
    [string]$WireDir = '',
    [string]$LinkDir = '',
    [string]$IpDir   = '',
    [string]$BleDir  = '',
    [string]$ArduinoCli = 'C:\Program Files\Arduino CLI\arduino-cli.exe',
    [switch]$KeepStaging
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdkRoot   = (Resolve-Path (Join-Path $scriptDir '..\..')).Path
$mclRoot   = (Resolve-Path (Join-Path $sdkRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($WireDir)) { $WireDir = Join-Path $mclRoot 'mcl-wire' }
if ([string]::IsNullOrWhiteSpace($LinkDir)) { $LinkDir = Join-Path $mclRoot 'mcl-link' }
if ([string]::IsNullOrWhiteSpace($IpDir))   { $IpDir   = Join-Path $mclRoot 'mcl-ip' }
if ([string]::IsNullOrWhiteSpace($BleDir))  { $BleDir  = Join-Path $mclRoot 'mcl-ble' }

foreach ($pair in @(@('mcl-wire', $WireDir), @('mcl-link', $LinkDir),
                    @('mcl-ip', $IpDir), @('mcl-ble', $BleDir))) {
    if (-not (Test-Path -LiteralPath $pair[1] -PathType Container)) {
        throw "$($pair[0]) not found at $($pair[1]). Pass the directory explicitly."
    }
}

# Exactly the board configuration the DFR1154 acoustic instrument was built
# with. Changing it changes the USB and partition behaviour, so it is pinned.
$fqbn = 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=default,EraseFlash=none,JTAGAdapter=builtin'

$staging = Join-Path $scriptDir 'build\esp32_dual_peer'
if (Test-Path -LiteralPath $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force -Path (Join-Path $staging 'mcl') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $staging 'src') | Out-Null

Copy-Item (Join-Path $scriptDir 'esp32_dual_peer\esp32_dual_peer.ino') $staging

# Headers land under <sketch>/mcl so that #include "mcl/sdk.h" resolves: the
# Arduino build puts the sketch root on the include path. Sources land under
# <sketch>/src, which the build compiles recursively.
$headers = @(
    (Join-Path $WireDir 'include\mcl\wire.h'),
    (Join-Path $WireDir 'include\mcl\extension.h'),
    (Join-Path $LinkDir 'include\mcl\link.h'),
    (Join-Path $LinkDir 'include\mcl\contact.h'),
    (Join-Path $LinkDir 'include\mcl\handoff.h'),
    (Join-Path $LinkDir 'include\mcl\control.h'),
    (Join-Path $LinkDir 'include\mcl\rendezvous.h'),
    (Join-Path $sdkRoot 'include\mcl\sdk.h'),
    (Join-Path $IpDir   'include\mcl\ip_binding.h'),
    (Join-Path $BleDir  'include\mcl\ble_binding.h')
)
$sources = @(
    (Join-Path $WireDir 'src\wire.c'),
    (Join-Path $WireDir 'src\extension.c'),
    (Join-Path $LinkDir 'src\link.c'),
    (Join-Path $LinkDir 'src\contact.c'),
    (Join-Path $LinkDir 'src\handoff.c'),
    (Join-Path $LinkDir 'src\control.c'),
    (Join-Path $LinkDir 'src\rendezvous.c'),
    (Join-Path $sdkRoot 'src\sdk.c'),
    (Join-Path $IpDir   'src\ip_binding.c'),
    (Join-Path $BleDir  'src\ble_binding.c')
)

foreach ($h in $headers) {
    if (-not (Test-Path -LiteralPath $h -PathType Leaf)) { throw "Missing header: $h" }
    Copy-Item $h (Join-Path $staging 'mcl')
}
foreach ($s in $sources) {
    if (-not (Test-Path -LiteralPath $s -PathType Leaf)) { throw "Missing source: $s" }
    Copy-Item $s (Join-Path $staging 'src')
}

Write-Host 'Staged protocol sources (sha256):' -ForegroundColor Cyan
foreach ($f in ($headers + $sources)) {
    $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash
    Write-Host ("  {0}  {1}" -f $h.Substring(0, 16), (Split-Path -Leaf $f))
}

if (-not (Test-Path -LiteralPath $ArduinoCli -PathType Leaf)) {
    throw "arduino-cli not found at $ArduinoCli"
}

$outDir = Join-Path $scriptDir 'build\out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Write-Host "Compiling for $fqbn" -ForegroundColor Cyan
# Windows PowerShell turns any stderr output from a native command into a
# NativeCommandError while ErrorActionPreference is Stop, so a compiler warning
# would abort a build that actually succeeded. The exit code is the authority.
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $ArduinoCli compile --fqbn $fqbn --output-dir $outDir --warnings default $staging
$compileExit = $LASTEXITCODE
$ErrorActionPreference = $previousPreference
if ($compileExit -ne 0) { throw "arduino-cli compile failed with exit code $compileExit" }

$bin = Join-Path $outDir 'esp32_dual_peer.ino.bin'
if (-not (Test-Path -LiteralPath $bin -PathType Leaf)) { throw "Expected application image not produced: $bin" }

$binHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $bin).Hash
Write-Host ''
Write-Host "APP_IMAGE=$bin" -ForegroundColor Green
Write-Host "APP_SHA256=$binHash"
Write-Host ("APP_BYTES={0}" -f (Get-Item $bin).Length)
Write-Host ''
Write-Host 'Not flashed. Run flash-app-only.ps1 to write the application partition.' -ForegroundColor Yellow

if (-not $KeepStaging) {
    Remove-Item -Recurse -Force $staging
}
