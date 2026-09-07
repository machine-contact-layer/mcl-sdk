<#
    Drive the MCL autonomous node over its serial control plane.

    THE CONTROL PLANE IS NOT THE DATA PLANE. Every command here is
    instrumentation: arm a run, stop it, read the log. There is deliberately no
    command that carries a peer address, a BLE address, a source_ref, an
    endpoint_token, a migration_ref, a session_ref, a secret or a pairing
    state, and adding one would make every zero-prior claim the node prints
    false.

    Examples:
      .\node-serial.ps1 -Command STATUS
      .\node-serial.ps1 -Command 'CONFIG 2 120000 0 0 100 3 1' -Then ARM -Listen 130
      .\node-serial.ps1 -Command RESULT
#>
param(
    [string]$PortName = 'COM3',
    [int]$Baud = 921600,
    [string]$Command = 'STATUS',
    [string]$Then = '',
    # Seconds to keep reading after the command. A run is watched by listening,
    # not by polling: the node prints as it goes.
    [int]$Listen = 3,
    [switch]$Reset
)

$ErrorActionPreference = 'Stop'

$port = New-Object System.IO.Ports.SerialPort $PortName, $Baud, 'None', 8, 'One'
$port.ReadTimeout = 500
$port.NewLine = "`n"
$port.Open()
try {
    if ($Reset) {
        # ESP32-S3 USB-serial-JTAG maps DTR/RTS to the boot/reset lines.
        $port.DtrEnable = $false; $port.RtsEnable = $true
        Start-Sleep -Milliseconds 150
        $port.RtsEnable = $false; $port.DtrEnable = $true
        Start-Sleep -Milliseconds 1200
    } else {
        $port.DtrEnable = $true
        Start-Sleep -Milliseconds 200
    }
    $port.DiscardInBuffer()

    if ($Command -ne '') { $port.WriteLine($Command) }
    if ($Then -ne '') { Start-Sleep -Milliseconds 400; $port.WriteLine($Then) }

    $deadline = (Get-Date).AddSeconds($Listen)
    while ((Get-Date) -lt $deadline) {
        try {
            $line = $port.ReadLine()
            if ($line) { Write-Output $line.TrimEnd() }
        } catch [TimeoutException] { }
    }
} finally {
    $port.Close()
}
