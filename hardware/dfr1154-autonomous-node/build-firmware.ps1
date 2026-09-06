<#
    Build the MCL autonomous node firmware. Builds only -- never uploads.

    WHY THE SOURCES ARE STAGED RATHER THAN COPIED INTO THE SKETCH

    The board must run THE SAME source the host runs. If the MCL sources lived
    in the sketch directory as their own copies they would drift, and "one
    implementation compiled twice" would quietly stop being true. So this
    script copies them from their canonical repositories at build time and
    prints the SHA-256 of every staged file next to the SHA-256 of the image.

    WHY IT DOES NOT UPLOAD

    The Arduino CLI upload step can rewrite the bootloader and the partition
    table. This board is flashed application-partition-only, at 0x20000, with
    esptool via flash-app-only.ps1. Flashing is a decision, not a build step.
#>
param(
    [string]$ArduinoCli = 'C:\Program Files\Arduino CLI\arduino-cli.exe',
    [switch]$KeepStaging
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdkRoot   = (Resolve-Path (Join-Path $scriptDir '..\..')).Path
$Root      = (Resolve-Path (Join-Path $scriptDir '..\..\..')).Path
$WireDir   = Join-Path $Root 'mcl-wire'
$LinkDir   = Join-Path $Root 'mcl-link'
$ApDir     = Join-Path $Root 'mcl-ap'

# CDCOnBoot=cdc is not optional: without it `Serial` is UART0 rather than the
# native USB CDC/JTAG the board enumerates as (VID 303A, PID 1001), and every
# line this firmware prints would go to pins nobody is reading.
#
# PSRAM is deliberately left at its default (disabled). The sketch header says
# why: the correlation working set stays in internal DRAM, and this project has
# never verified the QSPI/OPI mode option for this part.
$fqbn = 'esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB'

$Backup = Join-Path $env:USERPROFILE 'Downloads\MCL_DFR1154_BACKUP_20260902\dfr1154-factory-app-before-mcl.bin'

Write-Host '=== MCL autonomous node firmware ===' -ForegroundColor Cyan
Write-Host "root:   $Root"

if (-not (Test-Path -LiteralPath $Backup -PathType Leaf)) {
    Write-Host 'REFUSING TO BUILD: no factory backup at' -ForegroundColor Red
    Write-Host "  $Backup"
    Write-Host ''
    Write-Host 'The build itself is harmless, but the only reason to build is to flash,'
    Write-Host 'and this board must not be flashed without a restorable factory'
    Write-Host 'application. Take the backup first.'
    exit 1
}
Write-Host "factory backup present" -ForegroundColor Green

# ------------------------------------------------------------------ staging
#
# Headers land under <sketch>/mcl so that #include "mcl/wire.h" resolves: the
# Arduino build puts the sketch root on the include path. Sources land under
# <sketch>/src, which the build compiles recursively. No -I flag is needed,
# and none is used -- passing include paths through compiler.*.extra_flags is
# what broke the first version of this script.
$staging = Join-Path ([System.IO.Path]::GetTempPath()) 'mcl-auto-node-staging'
if (Test-Path -LiteralPath $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force -Path $staging | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $staging 'mcl') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $staging 'src') | Out-Null

# The staging directory is named for the sketch, so the image is named for it.
$stagedIno = Join-Path $staging 'mcl-auto-node-staging.ino'
Copy-Item (Join-Path $scriptDir 'dfr1154_autonomous_node\dfr1154_autonomous_node.ino') $stagedIno

$headers = @(
    (Join-Path $WireDir 'include\mcl\wire.h'),
    (Join-Path $LinkDir 'include\mcl\link.h'),
    (Join-Path $ApDir   'include\mcl\ap_modem.h'),
    (Join-Path $ApDir   'include\mcl\ap_listen.h')
)
$sources = @(
    (Join-Path $WireDir 'src\wire.c'),
    (Join-Path $LinkDir 'src\link.c'),
    (Join-Path $ApDir   'src\ap_modem.c'),
    (Join-Path $ApDir   'src\ap_listen.c')
)

foreach ($h in $headers) {
    if (-not (Test-Path -LiteralPath $h -PathType Leaf)) { throw "Missing header: $h" }
    Copy-Item $h (Join-Path $staging 'mcl')
}
foreach ($s in $sources) {
    if (-not (Test-Path -LiteralPath $s -PathType Leaf)) { throw "Missing source: $s" }
    Copy-Item $s (Join-Path $staging 'src')
}

Write-Host ''
Write-Host 'Staged protocol sources (sha256):' -ForegroundColor Cyan
foreach ($f in ($headers + $sources)) {
    $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash
    Write-Host ("  {0}  {1}" -f $h.Substring(0, 16), (Split-Path -Leaf $f))
}

if (-not (Test-Path -LiteralPath $ArduinoCli -PathType Leaf)) {
    throw "arduino-cli not found at $ArduinoCli"
}

# ------------------------------------------------------------------ compile
$outDir = Join-Path $scriptDir 'build\out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Write-Host ''
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

$bin = Join-Path $outDir 'mcl-auto-node-staging.ino.bin'
if (-not (Test-Path -LiteralPath $bin -PathType Leaf)) {
    throw "Expected application image not produced: $bin"
}

$binHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $bin).Hash
$binBytes = (Get-Item -LiteralPath $bin).Length

Write-Host ''
Write-Host "APP_IMAGE=$bin" -ForegroundColor Green
Write-Host "APP_SHA256=$binHash"
Write-Host "APP_BYTES=$binBytes"

$manifestPath = Join-Path $scriptDir 'build-manifest.txt'
$lines = @(
    'MCL autonomous node firmware build',
    "built: $(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')",
    "fqbn:  $fqbn",
    '',
    'STAGED SOURCES, copied from the canonical repositories at build time.',
    'The board runs the same source the host runs; these hashes say which.',
    ''
)
foreach ($f in ($headers + $sources)) {
    $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash
    $lines += ("  {0,-24} {1}" -f (Split-Path -Leaf $f), $h)
}
$lines += @(
    '',
    'IMAGE',
    '',
    ("  {0}  {1} bytes" -f (Split-Path -Leaf $bin), $binBytes),
    ("  sha256 {0}" -f $binHash),
    '',
    'FLASH (application partition only): use flash-app-only.ps1.',
    'Do NOT upload through the Arduino CLI: it can also rewrite the bootloader',
    'and the partition table. Restore with RESTORE_DFR1154_APP.cmd.'
)
$lines | Set-Content -Path $manifestPath -Encoding utf8

Write-Host ''
Write-Host 'Not flashed. Run flash-app-only.ps1 to write the application partition.' -ForegroundColor Yellow

if (-not $KeepStaging) { Remove-Item -Recurse -Force $staging }
