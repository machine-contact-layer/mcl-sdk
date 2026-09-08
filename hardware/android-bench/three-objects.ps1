<#
    The three AP-BOOTSTRAP-1 objects over air, phone to board.

    PRESENCE is 10 bytes, TRANSPORT_ACCEPT 16 and TRANSPORT_OFFER 17. They are
    run separately and counted separately because they are not the same test:
    at 300 baud and 160 samples a symbol, a 10-byte object spans about 25 000
    samples with its preamble and a 17-byte one about 33 900, and the board
    can only recover a frame that falls entirely inside one contiguous capture
    stretch of 40 000 samples. The longest object has the least room to land
    in, so a result that pools all three hides exactly the case most likely to
    fail.

    ONE-SHOT RECOVERY IS REPORTED SEPARATELY FROM BOUNDED-RETRY SUCCESS.
    Emissions and recoveries are both printed. A profile that repeats is
    allowed to; a reader who wants the per-emission figure must be able to see
    it rather than infer it from a success rate.
#>
param(
    [int]$Low = 3000,
    [int]$High = 6000,
    [int]$Repetitions = 10,
    [int]$WarmupSeconds = 12,
    [string]$Board = 'http://192.168.4.1',
    [string]$Adb = 'C:\Users\marsm\rdb\adb.exe',
    [string]$Package = 'org.mcl.bench'
)

$ErrorActionPreference = 'Stop'

function Send-Bench([string]$text) {
    & $Adb shell am broadcast -a "$Package.CMD" -p $Package --es cmd "'$text'" | Out-Null
}
function Wait-Board {
    $d = (Get-Date).AddSeconds(150)
    while ($true) {
        try { return Invoke-RestMethod -Uri "$Board/api/status" -TimeoutSec 6 }
        catch { if ((Get-Date) -ge $d) { throw 'board never came back' }; Start-Sleep -Seconds 3 }
    }
}

$results = @()
foreach ($obj in @('PRESENCE', 'ACCEPT', 'OFFER')) {
    Wait-Board | Out-Null
    $rungSeconds = $Repetitions * 3
    $listenMs = ($rungSeconds + 12) * 1000
    Invoke-RestMethod -Method Post -TimeoutSec 8 -Uri `
        "$Board/api/config?scenario=0&duration_ms=$listenMs&band_low_hz=$Low&band_high_hz=$High&quiesce_wifi=1" | Out-Null
    & $Adb logcat -c
    Send-Bench "band $Low $High"
    Invoke-RestMethod -Method Post -TimeoutSec 8 -Uri "$Board/api/run" | Out-Null
    Write-Host "=== $obj  band $Low/$High  reps $Repetitions ===" -ForegroundColor Cyan
    Start-Sleep -Seconds $WarmupSeconds

    for ($i = 1; $i -le $Repetitions; $i++) {
        Send-Bench "emit $obj 100"
        Start-Sleep -Milliseconds 3000
    }
    Start-Sleep -Seconds 14

    $log = & $Adb logcat -d -s MCLBENCH
    $emitted = ($log | Select-String 'AUDIO played').Count
    $incomplete = ($log | Select-String 'AUDIO play INCOMPLETE').Count
    $bytes = ($log | Select-String "EMIT $obj" | Select-Object -First 1)
    Wait-Board | Out-Null
    $r = Invoke-RestMethod -Uri "$Board/api/result" -TimeoutSec 8
    $row = [pscustomobject]@{
        object     = $obj
        emitted    = $emitted
        incomplete = $incomplete
        recovered  = $r.counters.frames_recovered
        decoded    = $r.counters.objects_decoded
        heard_only = $r.counters.frames_heard
        peak       = $r.counters.cap_peak
        captured   = $r.counters.cap_samples
    }
    $results += $row
    Write-Host "  $($bytes.Line -replace '.*EMIT ', '')"
    $row | Format-List | Out-String | Write-Host
}

Write-Host '=== SUMMARY: one-shot recovery per emission ===' -ForegroundColor Cyan
$results | Format-Table -AutoSize
