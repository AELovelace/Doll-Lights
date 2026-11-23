# Restarts the Light Console Server task cleanly: stops the task, removes any leftover
# servers from this project, starts the task again, and confirms exactly one server answers.
# Run from an administrator PowerShell (LOCAL SERVICE processes cannot be stopped otherwise).

$ErrorActionPreference = 'Stop' # Stops at the first failure instead of reporting a false success.
$taskName = 'Light Console Server' # Matches the name used by install_light_console_startup.ps1.
$projectRoot = Split-Path -Parent $PSScriptRoot # Resolves this checkout regardless of the caller's directory.
$appPath = Join-Path $projectRoot 'python\app.py' # Identifies servers that belong to this project.
$port = (Get-Content (Join-Path $projectRoot 'python\config.json') -Raw | ConvertFrom-Json).web.port # Reads the configured web port.

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw 'Run this script from an administrator PowerShell.' # Task and LOCAL SERVICE process control both need elevation.
}

function Get-ConsoleServers {
    Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine -like "*$appPath*" -and $_.Name -match '^(py|pythonw?)\.exe$' }
} # Completes lookup of every Python process running this project's app.py.

Stop-ScheduledTask -TaskName $taskName # Ends the launcher; new launchers also take their Python child down.
Start-Sleep -Seconds 1

$leftovers = @(Get-ConsoleServers) # Catches servers orphaned by launchers that predate the kill-on-close fix.
foreach ($process in $leftovers) {
    Write-Output "Stopping leftover server PID $($process.ProcessId)"
    Stop-Process -Id $process.ProcessId -Force
}

foreach ($attempt in 1..20) {
    if (-not (Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue)) { break } # Waits for the port to be released.
    Start-Sleep -Milliseconds 500
}
$blocking = @(Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue)
if ($blocking) {
    $owners = ($blocking | ForEach-Object { $_.OwningProcess } | Sort-Object -Unique) -join ', '
    throw "Port $port is still held by PID $owners, which is not this project's server. Stop it manually, then rerun." # Never kills unrelated programs.
}

Start-ScheduledTask -TaskName $taskName # Launches a fresh server that re-reads config.json.

$listeners = @()
foreach ($attempt in 1..30) {
    $listeners = @(Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue)
    if ($listeners) { break } # Waits for Python to finish binding the port.
    Start-Sleep -Milliseconds 500
}
if ($listeners.Count -ne 1) {
    throw "Expected exactly one server on port $port, found $($listeners.Count). Check logs\stderr.log." # Reports startup failures or duplicates.
}

$config = Invoke-RestMethod -Uri "http://127.0.0.1:$port/api/config" -TimeoutSec 5 -ErrorAction SilentlyContinue # Unauthenticated read; skipped quietly if tokens are required.
Write-Output "Light Console restarted: PID $($listeners[0].OwningProcess) on port $port"
if ($config) {
    $config.boards | ForEach-Object { Write-Output ("  {0}  {1}" -f $_.id, $_.name) } # Shows the board names the browser will receive.
}
# Completes a verified single-instance restart of the Light Console Server task.
