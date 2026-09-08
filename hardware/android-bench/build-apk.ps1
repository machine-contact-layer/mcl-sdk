<#
    Build the MCL Android bench APK.

    WITHOUT GRADLE, DELIBERATELY.

    aapt2, javac, d8 and apksigner are all that packaging an APK needs, and
    calling them directly means this build has no daemon, no plugin resolution,
    no lock files and no multi-gigabyte cache. It also means every step is
    visible: a reader can see exactly what goes into the image, which is the
    same property build-firmware.ps1 gives the firmware.

    THE PROTOCOL SOURCES ARE STAGED, NEVER COPIED IN.

    The phone must run THE SAME source the board and the host run. So the
    canonical files are copied out of the sibling repositories at build time,
    their SHA-256 printed beside the image's, and the staging deleted
    afterwards. No protocol source lives in this directory, so the phone and
    the board cannot drift apart.

    TOOLCHAIN

    Nothing is installed by this script. It finds a JDK, an Android SDK
    platform, build-tools and an NDK that are already on the machine and
    prints what it used, because a build whose toolchain is invisible is a
    build nobody can reproduce.
#>
param(
    # These default to the toolchains already present on this machine. Point
    # them anywhere else and the build is unchanged.
    # Resolved from the environment when not given; see Resolve-Tool below.
    [string]$JdkHome = '',
    [string]$SdkRoot = '',
    # BUILD-TOOLS 35, NOT 34, AND THE REASON IS A CRASH RATHER THAN TASTE.
    # 34.0.0 ships R8 8.2.2-dev, whose dexer dies on this app's anonymous
    # BroadcastReceiver with "Cannot invoke String.length() because
    # <parameter1> is null" -- an internal error, not a complaint about the
    # code. 35.0.0 dexes the identical jar without a murmur.
    [string]$BuildToolsVersion = '35.0.0',
    [string]$Platform = 'android-35',
    [string]$NdkVersion = '26.1.10909125',
    # arm64-v8a only. The bench runs on one phone and every other ABI is dead
    # weight in the image and on the disk.
    [string]$Abi = 'arm64-v8a',
    [int]$MinSdk = 26,
    [switch]$KeepStaging
)

$ErrorActionPreference = 'Stop'

# WHERE THE JDK AND THE SDK COME FROM
#
# Not from a path written into this file: the publication gate treats a tracked
# script naming someone's home directory as a fatal finding, because it is
# configuration an adopter cannot discover. Both are resolved from the
# environment variables the Android tooling already defines, and a missing one
# is an error that says how to supply it.
function Resolve-Root([string]$explicit, [string[]]$envNames, [string]$what, [string]$hint) {
    if ($explicit) {
        if (Test-Path -LiteralPath $explicit) { return $explicit }
        throw "$what not found at '$explicit'"
    }
    foreach ($n in $envNames) {
        $v = [Environment]::GetEnvironmentVariable($n)
        if ($v -and (Test-Path -LiteralPath $v)) { return $v }
    }
    throw "$what not found. $hint"
}

$JdkHome = Resolve-Root $JdkHome @('MCL_JDK_HOME', 'JAVA_HOME') 'JDK' `
    'Pass -JdkHome <path>, or set JAVA_HOME or MCL_JDK_HOME.'
$SdkRoot = Resolve-Root $SdkRoot @('MCL_ANDROID_SDK', 'ANDROID_SDK_ROOT', 'ANDROID_HOME') 'Android SDK' `
    'Pass -SdkRoot <path>, or set ANDROID_SDK_ROOT or MCL_ANDROID_SDK.'


$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
# NOTE: not $sdkRoot. PowerShell variable names are case-insensitive,
# so an MCL $sdkRoot and an Android $SdkRoot are ONE variable, and the
# second assignment silently redirected every toolchain path into the
# MCL tree. Named apart rather than relying on case.
$MclSdkDir = (Resolve-Path (Join-Path $scriptDir '..\..')).Path
$Root      = (Resolve-Path (Join-Path $scriptDir '..\..\..')).Path

$javac     = Join-Path $JdkHome 'bin\javac.exe'
$keytool   = Join-Path $JdkHome 'bin\keytool.exe'
$aapt2     = Join-Path $SdkRoot "build-tools\$BuildToolsVersion\aapt2.exe"
$d8        = Join-Path $SdkRoot "build-tools\$BuildToolsVersion\d8.bat"
$apksigner = Join-Path $SdkRoot "build-tools\$BuildToolsVersion\apksigner.bat"
$zipalign  = Join-Path $SdkRoot "build-tools\$BuildToolsVersion\zipalign.exe"
$androidJar= Join-Path $SdkRoot "platforms\$Platform\android.jar"
$ndkBin    = Join-Path $SdkRoot "ndk\$NdkVersion\toolchains\llvm\prebuilt\windows-x86_64\bin"
$clang     = Join-Path $ndkBin "aarch64-linux-android$MinSdk-clang.cmd"

Write-Host '=== MCL Android bench ===' -ForegroundColor Cyan
foreach ($tool in @($javac, $aapt2, $d8, $apksigner, $androidJar, $clang)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Missing toolchain component: $tool" }
}
Write-Host "jdk        $JdkHome"
Write-Host "sdk        $SdkRoot ($Platform, build-tools $BuildToolsVersion)"
Write-Host "ndk        $NdkVersion, $Abi, minSdk $MinSdk"
# d8.bat and apksigner.bat resolve java through JAVA_HOME and refuse without
# it, whatever is on PATH.
$env:JAVA_HOME = $JdkHome

# ------------------------------------------------------------------ staging
$staging = Join-Path ([System.IO.Path]::GetTempPath()) 'mcl-android-bench-staging'
if (Test-Path -LiteralPath $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force -Path (Join-Path $staging 'mcl') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $staging 'src') | Out-Null

$WireDir = Join-Path $Root 'mcl-wire'
$LinkDir = Join-Path $Root 'mcl-link'
$ApDir   = Join-Path $Root 'mcl-ap'
$BleDir  = Join-Path $Root 'mcl-ble'
$IpDir   = Join-Path $Root 'mcl-ip'

$headers = @(
    (Join-Path $WireDir 'include\mcl\wire.h'),
    (Join-Path $WireDir 'include\mcl\extension.h'),
    (Join-Path $LinkDir 'include\mcl\link.h'),
    (Join-Path $LinkDir 'include\mcl\contact.h'),
    (Join-Path $LinkDir 'include\mcl\handoff.h'),
    (Join-Path $LinkDir 'include\mcl\control.h'),
    (Join-Path $LinkDir 'include\mcl\negotiation.h'),
    (Join-Path $LinkDir 'include\mcl\endpoint_rendezvous.h'),
    (Join-Path $ApDir   'include\mcl\ap_modem.h'),
    (Join-Path $ApDir   'include\mcl\ap_listen.h'),
    (Join-Path $MclSdkDir 'include\mcl\sdk.h'),
    (Join-Path $MclSdkDir 'include\mcl\rendezvous.h'),
    (Join-Path $MclSdkDir 'include\mcl\machine.h'),
    (Join-Path $BleDir  'include\mcl\ble_binding.h'),
    (Join-Path $IpDir   'include\mcl\ip_binding.h')
)
$sources = @(
    (Join-Path $WireDir 'src\wire.c'),
    (Join-Path $WireDir 'src\extension.c'),
    (Join-Path $LinkDir 'src\link.c'),
    (Join-Path $LinkDir 'src\contact.c'),
    (Join-Path $LinkDir 'src\handoff.c'),
    (Join-Path $LinkDir 'src\control.c'),
    (Join-Path $LinkDir 'src\negotiation.c'),
    (Join-Path $LinkDir 'src\endpoint_rendezvous.c'),
    (Join-Path $ApDir   'src\ap_modem.c'),
    (Join-Path $ApDir   'src\ap_listen.c'),
    (Join-Path $MclSdkDir 'src\sdk.c'),
    (Join-Path $MclSdkDir 'src\rendezvous.c'),
    (Join-Path $MclSdkDir 'src\machine.c'),
    (Join-Path $BleDir  'src\ble_binding.c'),
    (Join-Path $IpDir   'src\ip_binding.c')
)

foreach ($h in $headers) {
    if (-not (Test-Path -LiteralPath $h)) { throw "Missing header: $h" }
    Copy-Item $h (Join-Path $staging 'mcl')
}
foreach ($s in $sources) {
    if (-not (Test-Path -LiteralPath $s)) { throw "Missing source: $s" }
    Copy-Item $s (Join-Path $staging 'src')
}

Write-Host ''
Write-Host 'Staged protocol sources (sha256):' -ForegroundColor Cyan
$stagedHashes = @()
foreach ($f in ($headers + $sources)) {
    $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash
    $stagedHashes += ("  {0,-26} {1}" -f (Split-Path -Leaf $f), $h)
    Write-Host ("  {0}  {1}" -f $h.Substring(0, 16), (Split-Path -Leaf $f))
}

# --------------------------------------------------------------- native .so
$outDir = Join-Path $scriptDir 'build'
$libDir = Join-Path $outDir "lib\$Abi"
New-Item -ItemType Directory -Force -Path $libDir | Out-Null

Write-Host ''
Write-Host "Compiling libmclbench.so for $Abi" -ForegroundColor Cyan
$cSources = @((Join-Path $scriptDir 'app\jni\mcl_jni.c'))
$cSources += (Get-ChildItem (Join-Path $staging 'src') -Filter *.c | ForEach-Object { $_.FullName })

$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
# -lm is not optional and is not implied. Android keeps libm.so separate from
# libc.so and the NDK's clang driver does not add it, so a library whose only
# libm reference is the modem's sinf() links cleanly and then fails at dlopen
# with "cannot locate symbol sinf" -- a runtime failure produced by a silent
# link-time omission.
& $clang -std=c99 -O2 -fPIC -shared -Wall -Wextra `
    "-I$staging" `
    -o (Join-Path $libDir 'libmclbench.so') `
    @cSources -lm
$clangExit = $LASTEXITCODE
$ErrorActionPreference = $previous
if ($clangExit -ne 0) { throw "clang failed with exit code $clangExit" }

& (Join-Path $ndkBin 'llvm-strip.exe') (Join-Path $libDir 'libmclbench.so')
$soBytes = (Get-Item (Join-Path $libDir 'libmclbench.so')).Length
Write-Host "libmclbench.so  $soBytes bytes" -ForegroundColor Green

# ------------------------------------------------------------------- java
$classes = Join-Path $outDir 'classes'
if (Test-Path $classes) { Remove-Item -Recurse -Force $classes }
New-Item -ItemType Directory -Force -Path $classes | Out-Null

$javaFiles = Get-ChildItem (Join-Path $scriptDir 'app\java') -Recurse -Filter *.java |
             ForEach-Object { $_.FullName }

Write-Host ''
Write-Host "Compiling $($javaFiles.Count) Java sources" -ForegroundColor Cyan
$ErrorActionPreference = 'Continue'
& $javac -source 11 -target 11 -nowarn -classpath $androidJar -d $classes @javaFiles
$javacExit = $LASTEXITCODE
$ErrorActionPreference = $previous
if ($javacExit -ne 0) { throw "javac failed with exit code $javacExit" }

Write-Host 'Dexing' -ForegroundColor Cyan
# Through a jar rather than a list of .class paths: the list grows with the
# app and Windows has a command-line length limit, which would fail later and
# look like a compiler problem.
$classesJar = Join-Path $outDir 'classes.jar'
if (Test-Path $classesJar) { Remove-Item -Force $classesJar }
$ErrorActionPreference = 'Continue'
& (Join-Path $JdkHome 'bin\jar.exe') --create --file $classesJar -C $classes .
& cmd /c "`"$d8`" --lib `"$androidJar`" --min-api $MinSdk --output `"$outDir`" `"$classesJar`""
$d8Exit = $LASTEXITCODE
$ErrorActionPreference = $previous
if ($d8Exit -ne 0) { throw "d8 failed with exit code $d8Exit" }

# ----------------------------------------------------------------- package
$unsigned = Join-Path $outDir 'mcl-bench-unsigned.apk'
$aligned  = Join-Path $outDir 'mcl-bench-aligned.apk'
$signed   = Join-Path $outDir 'mcl-bench.apk'
foreach ($f in @($unsigned, $aligned, $signed)) {
    if (Test-Path $f) { Remove-Item -Force $f }
}

Write-Host 'Linking resources' -ForegroundColor Cyan
$ErrorActionPreference = 'Continue'
& $aapt2 link -o $unsigned -I $androidJar `
    --manifest (Join-Path $scriptDir 'app\AndroidManifest.xml') `
    --min-sdk-version $MinSdk --target-sdk-version 34 `
    --version-code 1 --version-name '1.0-bench'
$aaptExit = $LASTEXITCODE
$ErrorActionPreference = $previous
if ($aaptExit -ne 0) { throw "aapt2 link failed with exit code $aaptExit" }

# The dex and the native library go in with plain zip entries. Add-Type is
# used rather than Compress-Archive because the paths inside the APK matter
# and Compress-Archive will not place them.
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$zip = [System.IO.Compression.ZipFile]::Open($unsigned, 'Update')
try {
    $dex = Join-Path $outDir 'classes.dex'
    if (-not (Test-Path $dex)) { throw "d8 produced no classes.dex" }
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $zip, $dex, 'classes.dex') | Out-Null
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $zip, (Join-Path $libDir 'libmclbench.so'), "lib/$Abi/libmclbench.so") | Out-Null
} finally {
    $zip.Dispose()
}

# A debug keystore, created once. This APK is a lab instrument installed by
# hand over adb; it is never distributed, and the key is not a secret.
$keystore = Join-Path $outDir 'bench-debug.keystore'
if (-not (Test-Path $keystore)) {
    Write-Host 'Creating a local debug keystore' -ForegroundColor Cyan
    $ErrorActionPreference = 'Continue'
    & $keytool -genkeypair -keystore $keystore -storepass mclbench -keypass mclbench `
        -alias benchkey -keyalg RSA -keysize 2048 -validity 3650 `
        -dname 'CN=MCL bench, OU=lab, O=machine-contact-layer' 2>&1 | Out-Null
    $ErrorActionPreference = $previous
}

if (Test-Path $zipalign) {
    Write-Host 'Aligning' -ForegroundColor Cyan
    $ErrorActionPreference = 'Continue'
    & $zipalign -p -f 4 $unsigned $aligned
    $ErrorActionPreference = $previous
} else {
    Copy-Item $unsigned $aligned
}

Write-Host 'Signing' -ForegroundColor Cyan
$ErrorActionPreference = 'Continue'
& cmd /c "`"$apksigner`" sign --ks `"$keystore`" --ks-pass pass:mclbench --key-pass pass:mclbench --out `"$signed`" `"$aligned`""
$signExit = $LASTEXITCODE
$ErrorActionPreference = $previous
if ($signExit -ne 0) { throw "apksigner failed with exit code $signExit" }

$apkHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $signed).Hash
$apkBytes = (Get-Item $signed).Length

Write-Host ''
Write-Host "APK=$signed" -ForegroundColor Green
Write-Host "APK_SHA256=$apkHash"
Write-Host "APK_BYTES=$apkBytes"

# ---------------------------------------------------------------- manifest
$manifestPath = Join-Path $scriptDir 'build-manifest.txt'
$lines = @(
    'MCL Android bench build',
    "built: $(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')",
    "abi:   $Abi   minSdk: $MinSdk   platform: $Platform",
    "ndk:   $NdkVersion",
    '',
    'STAGED SOURCES, copied from the canonical repositories at build time.',
    'The phone runs the same source the board and the host run.',
    ''
) + $stagedHashes + @(
    '',
    'IMAGE',
    '',
    ("  libmclbench.so  {0} bytes" -f $soBytes),
    ("  mcl-bench.apk   {0} bytes" -f $apkBytes),
    ("  sha256 {0}" -f $apkHash),
    '',
    'INSTALL',
    '  adb install -r build\mcl-bench.apk',
    '  adb shell am start -n org.mcl.bench/.BenchActivity',
    '  adb logcat -s MCLBENCH'
)
$lines | Set-Content -Path $manifestPath -Encoding utf8

if (-not $KeepStaging) { Remove-Item -Recurse -Force $staging }
Write-Host ''
Write-Host 'Not installed. adb install -r build\mcl-bench.apk' -ForegroundColor Yellow
