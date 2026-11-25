# serial_log.ps1
# Reads the DOLL-REMOTE Cardputer's USB serial log (the "[remote] ..." request lines)
# and prints it, optionally copying it to a file. DTR/RTS stay low, so opening the
# port doesn't reset the board.
#
#   .\ps\serial_log.ps1 -Port COM5
#   .\ps\serial_log.ps1 -Port COM5 -LogFile remote.log -Seconds 120
param(
    [string]$Port = "COM5",       # the Cardputer's USB serial port (S3 native USB)
    [string]$LogFile = "",        # optional: also append every line here
    [int]$Seconds = 0             # 0 = run until Ctrl+C
)

$serial = New-Object System.IO.Ports.SerialPort $Port, 115200   # baud is ignored by native USB, set for completeness
$serial.DtrEnable = $false        # don't wiggle the reset lines
$serial.RtsEnable = $false
$serial.ReadTimeout = 500         # ms; lets the loop check the clock
$serial.Open()
Write-Host "Listening on $Port..."
$stopAt = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { [datetime]::MaxValue }
try {
    while ((Get-Date) -lt $stopAt) {
        try {
            $line = $serial.ReadLine().TrimEnd()          # one log line from the board
            $stamped = "{0:HH:mm:ss} {1}" -f (Get-Date), $line
            Write-Host $stamped
            if ($LogFile) { Add-Content -Path $LogFile -Value $stamped }
        } catch [System.TimeoutException] { }             # nothing this half-second
    }
} finally {
    $serial.Close()                                       # always free the port for uploads
}
