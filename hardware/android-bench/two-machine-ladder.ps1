<#
    One rung of a band ladder between the two physical machines.

    THE BOARD AND THE PHONE ARE BOTH DRIVEN, AND NEITHER CARRIES MCL BYTES FOR
    THE OTHER. The board is armed over its own SoftAP and the phone over adb.
    Both are control planes; every MCL byte in a rung crosses the air as sound.

    WHY THE BOARD IS ARMED FIRST AND THEN WAITED FOR

    Arming the board restarts it into the run -- see the note at kArmedMagic in
    the firmware -- so there is a boot, a quiesce and a phase change between
    the arm and the first sample it will search. Emitting during that window
    measures nothing and looks like a dead band. -WarmupSeconds is that gap and
    it is deliberately generous.

    WHY RECOVERIES ARE REPORTED AGAINST EMISSIONS AND NOT AS A RATE ALONE

    The board cannot listen continuously: it searches about 11 000 sample
    positions per second against 48 000 arriving, so it is deaf roughly four
    fifths of the time and its recovery count is floored by that duty cycle
    rather than by the band. Comparing two bands on this receiver is still
    valid -- the duty cycle is the same for both -- but comparing the board's
    rate to the phone's is not, and the raw counts are printed so the reader
    can see which comparison is being made.

    .\two-machine-ladder.ps1 -Direction board-to-phone -Low 6000 -High 9000
    .\two-machine-ladder.ps1 -Direction phone-to-board -Low 3000 -High 6000
#>
param(
    [ValidateSet('board-to-phone', 'phone-to-board')]
    [string]$Direction = 'board-to-phone',
    [int]$Low = 6000,
    [int]$High = 9000,
    [int]$Repetitions = 10,
    [int]$WarmupSeconds = 12,
    [string]$Board = 'http://192.168.4.1',
    [string]$Adb = '',
    [string]$Package = 'org.mcl.bench'
)

$ErrorActionPreference = 'Stop'

# WHERE adb COMES FROM, AND WHY IT IS NOT A PATH IN THIS FILE
#
# A tracked script naming a path under someone's home directory is
# configuration an adopter cannot discover. So adb is resolved, in order:
# -Adb if given, then MCL_ADB, then ANDROID_SDK_ROOT/ANDROID_HOME's
# platform-tools, then whatever is on PATH. Failing to find it is an error
# that says how to fix it, not a silent fallback to a path that exists on one
# machine.
function Resolve-Adb([string]$explicit) {
    if ($explicit) {
        if (Test-Path -LiteralPath $explicit) { return $explicit }
        throw "adb not found at -Adb '$explicit'"
    }
    if ($env:MCL_ADB -and (Test-Path -LiteralPath $env:MCL_ADB)) { return $env:MCL_ADB }
    foreach ($root in @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME)) {
        if ($root) {
            $p = Join-Path $root 'platform-tools\adb.exe'
            if (Test-Path -LiteralPath $p) { return $p }
        }
    }
    $cmd = Get-Command adb -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw ("adb not found. Pass -Adb <path>, or set MCL_ADB, or put adb on PATH, " +
           "or set ANDROID_SDK_ROOT.")
}
$Adb = Resolve-Adb $Adb

$benchDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Send-Bench([string]$text) {
    # Single quotes for the device shell: the remote shell splits the string
    # again, so `--es cmd "emit PRESENCE"` would deliver only "emit".
    & $Adb shell am broadcast -a "$Package.CMD" -p $Package --es cmd "'$text'" | Out-Null
}

# A quiesced run takes the SoftAP off the air for its whole duration, so the
# laptop's association is gone and has to come back before the board can be
# read. One timed request treats that reassociation as a dead board; this
# waits for it instead.
function Board-Get([string]$path, [int]$WaitSeconds = 90) {
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ($true) {
        try {
            return Invoke-RestMethod -Uri "$Board$path" -TimeoutSec 6
        } catch {
            if ((Get-Date) -ge $deadline) { throw }
            Start-Sleep -Seconds 3
        }
    }
}

# /api/config and /api/run are POST. They take their arguments in the query
# string all the same, which is why they look like GETs and were called as
# such the first time.
function Board-Post([string]$path) {
    return Invoke-RestMethod -Uri "$Board$path" -Method Post -TimeoutSec 8
}

# How long a rung has to run to produce $Repetitions emissions, which differs
# by direction because the two transmitters are paced differently.
#
# The phone emits on demand: 1.2 s of audio for a 10-byte object at 300 baud,
# then a 1.5 s gap, so about 3 s each.
#
# The board announces on its OWN randomised schedule -- 3 to 7 s between
# emissions, deliberately, so that two boards do not lock into one cadence --
# and there is no way to ask it for exactly N. Budgeting 3 s each is what made
# the first rung produce 3 emissions where 8 were asked for. Seven seconds
# covers the worst case of that interval plus the emission.
$secondsPerEmission = if ($Direction -eq 'board-to-phone') { 7 } else { 3 }
$rungSeconds = $Repetitions * $secondsPerEmission
$listenMs = ($rungSeconds + 10) * 1000

Write-Host "=== rung: $Direction  band $Low/$High  reps $Repetitions ===" -ForegroundColor Cyan

if ($Direction -eq 'board-to-phone') {
    # Board announces, phone listens. The phone keeps up with the audio in
    # real time, so its count is a clean measure of the path.
    $cfg = "$Board/api/config?scenario=1&duration_ms=$($rungSeconds * 1000)" +
           "&band_low_hz=$Low&band_high_hz=$High&emit_gain_pct=100&quiesce_wifi=1"
    Board-Post $cfg.Substring($Board.Length) | Out-Null

    & $Adb logcat -c
    Send-Bench "band $Low $High"
    Start-Sleep -Milliseconds 500
    Send-Bench "listen $listenMs"

    Board-Post "/api/run" | Out-Null
    Write-Host "board armed; waiting $([int]($rungSeconds + $WarmupSeconds + 14)) s"
    Start-Sleep -Seconds ($rungSeconds + $WarmupSeconds + 14)

    $log = & $Adb logcat -d -s MCLBENCH
    $contacts = ($log | Select-String 'CONTACT ').Count
    $done = $log | Select-String 'LISTEN done'
    $result = Board-Get '/api/result'
    Write-Host "board emitted : $($result.counters.frames_emitted)"
    Write-Host "phone contacts: $contacts"
    if ($done) { Write-Host "phone         : $($done[-1].Line.Split(':')[-1].Trim())" }
    $log | Select-String 'CONTACT ' | Select-Object -First 3 | ForEach-Object { Write-Host "  $($_.Line.Split(':')[-1].Trim())" }
}
else {
    # Phone announces, board listens. The board's count is floored by its duty
    # cycle; see the header.
    $cfg = "$Board/api/config?scenario=0&duration_ms=$($listenMs)" +
           "&band_low_hz=$Low&band_high_hz=$High&quiesce_wifi=1"
    Board-Post $cfg.Substring($Board.Length) | Out-Null

    & $Adb logcat -c
    Send-Bench "band $Low $High"
    Board-Post "/api/run" | Out-Null
    Write-Host "board armed; warming up $WarmupSeconds s before the phone emits"
    Start-Sleep -Seconds $WarmupSeconds

    Send-Bench "ladder $Low $High $Repetitions"
    Start-Sleep -Seconds ($rungSeconds + 16)

    $log = & $Adb logcat -d -s MCLBENCH
    $emitted = ($log | Select-String 'AUDIO played').Count
    $result = Board-Get '/api/result'
    Write-Host "phone emitted  : $emitted"
    Write-Host "board recovered: $($result.counters.frames_recovered) (objects $($result.counters.objects_decoded))"
    Write-Host "board captured : $($result.counters.cap_samples) samples, peak $($result.counters.cap_peak)"
}
