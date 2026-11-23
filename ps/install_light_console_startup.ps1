#Requires -RunAsAdministrator
param(
    [Parameter(Mandatory = $true)]
    [string]$PythonExecutable
)

$ErrorActionPreference = 'Stop' # Stops installation if Windows rejects any required step.
$projectRoot = Split-Path -Parent $PSScriptRoot # Anchors the task to this checkout.
$logDirectory = Join-Path $projectRoot 'logs' # Keeps installation and server diagnostics together.
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null # Creates the log destination before registering the task.
Start-Transcript -Path (Join-Path $logDirectory 'startup-install.log') -Force | Out-Null # Records elevated errors even when the window is hidden.
try {
    $pythonPath = (Resolve-Path -LiteralPath $PythonExecutable).Path # Uses a real interpreter instead of a per-user launcher alias.
    & $pythonPath -c 'import sys; assert sys.version_info >= (3, 11)' # Checks the project's minimum Python version.
    if ($LASTEXITCODE -ne 0) { throw 'Python 3.11 or newer is required.' }

    $taskName = 'Light Console Server' # Provides a recognizable name in Windows Task Scheduler.
    $launcherPath = Join-Path $PSScriptRoot 'start_light_console.ps1' # Reuses the documented project launcher.
    $existingTask = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue # Detects earlier installations before replacing anything.
    if ($existingTask) {
        if ($existingTask.Actions.Arguments -notlike ('*"{0}"*' -f $launcherPath)) {
            throw "A task named '$taskName' already belongs to another installation."
        }
        if ($existingTask.State -eq 'Running') { throw 'Stop the existing Light Console Server task before reinstalling it.' }
    }

    $config = Get-Content (Join-Path $projectRoot 'python\config.json') -Raw | ConvertFrom-Json # Reads only the configured listening port.
    if (Get-NetTCPConnection -State Listen -LocalPort $config.web.port -ErrorAction SilentlyContinue) {
        throw "Port $($config.web.port) is already in use; stop the existing server before installing."
    }

    $logAcl = Get-Acl -LiteralPath $logDirectory # Limits the service account's new write permission to logs.
    $serviceSid = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-19') # Identifies Windows LOCAL SERVICE without relying on display language.
    $logRule = New-Object System.Security.AccessControl.FileSystemAccessRule($serviceSid, 'Modify', 'ContainerInherit,ObjectInherit', 'None', 'Allow') # Allows the background server to write its diagnostics.
    $logAcl.SetAccessRule($logRule)
    Set-Acl -LiteralPath $logDirectory -AclObject $logAcl # Applies the log permission without changing source permissions.

    $powershellPath = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe' # Uses the Windows-provided task host.
    $arguments = '-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "{0}" -PythonExecutable "{1}" -LogDirectory "{2}"' -f $launcherPath, $pythonPath, $logDirectory # Quotes paths so spaces remain safe.
    $action = New-ScheduledTaskAction -Execute $powershellPath -Argument $arguments -WorkingDirectory $projectRoot # Starts the launcher from the correct checkout.
    $trigger = New-ScheduledTaskTrigger -AtStartup # Runs before any user signs in.
    $trigger.Delay = 'PT30S' # Gives local drives and networking time to initialize.
    $principal = New-ScheduledTaskPrincipal -UserId 'S-1-5-19' -LogonType ServiceAccount -RunLevel Limited # Runs as LOCAL SERVICE without a stored password or administrator privileges.
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) # Keeps the server running and retries failed launches.
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Description "Boot-time Light Console server for $projectRoot" -Force | Out-Null # Installs this project's startup task.
    Start-ScheduledTask -TaskName $taskName # Starts the server immediately so installation can be verified without rebooting.
    Write-Output "Installed and started '$taskName'. Logs: $logDirectory"
}
catch {
    Write-Output ($_ | Out-String) # Makes the complete installation error available in the transcript.
    exit 1
}
finally {
    Stop-Transcript | Out-Null # Finishes the diagnostic record on both success and failure.
}
