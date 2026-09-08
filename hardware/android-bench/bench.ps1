<#
    Drive the MCL Android bench over adb.

    adb IS DEPLOYMENT AND INSTRUMENTATION, NEVER TRANSPORT. This pushes a
    command and reads logcat. Every MCL byte crosses the air or the radio.

    The quoting matters and is the reason this script exists: `adb shell am
    broadcast --es cmd "emit PRESENCE"` delivers only "emit", because the
    remote shell splits the string again. The extra has to survive two shells,
    so it is wrapped in single quotes for the device side.

    Examples:
      .\bench.ps1 -Command 'ble on'
      .\bench.ps1 -Command 'adv A5C30F17' -Listen 5
      .\bench.ps1 -Command 'band 6000 9000' -Then 'emit PRESENCE' -Listen 10
      .\bench.ps1 -Tail 40
#>
param(
    [string]$Adb = 'C:\Users\marsm\rdb\adb.exe',
    [string]$Package = 'org.mcl.bench',
    [string]$Command = '',
    [string]$Then = '',
    [int]$Listen = 3,
    [int]$Tail = 0,
    [switch]$Clear,
    [switch]$Restart
)

$ErrorActionPreference = 'Stop'

function Send-Bench([string]$text) {
    if ([string]::IsNullOrWhiteSpace($text)) { return }
    # Single quotes for the device shell; the outer double quotes are consumed
    # by PowerShell.
    & $Adb shell am broadcast -a "$Package.CMD" -p $Package --es cmd "'$text'" | Out-Null
}

if ($Clear) { & $Adb logcat -c }

if ($Restart) {
    & $Adb shell am force-stop $Package | Out-Null
    & $Adb logcat -c
    & $Adb shell am start -n "$Package/.BenchActivity" | Out-Null
    Start-Sleep -Seconds 3
}

if ($Tail -gt 0) {
    & $Adb logcat -d -s MCLBENCH | Select-Object -Last $Tail
    exit 0
}

Send-Bench $Command
if ($Then -ne '') {
    Start-Sleep -Milliseconds 600
    Send-Bench $Then
}

Start-Sleep -Seconds $Listen
& $Adb logcat -d -s MCLBENCH | Select-Object -Last 40
