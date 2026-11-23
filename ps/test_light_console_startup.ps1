#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop' # Fails verification when the installed service cannot be inspected.
$projectRoot = Split-Path -Parent $PSScriptRoot # Locates this installation's configuration and diagnostics.
$reportPath = Join-Path $projectRoot 'logs\startup-verification.json' # Saves the result for callers outside the elevated window.
try {
    $task = Get-ScheduledTask -TaskName 'Light Console Server' # Service-account tasks require administrator access to inspect.
    $info = Get-ScheduledTaskInfo -TaskName $task.TaskName # Retrieves the last launch status.
    $config = Get-Content (Join-Path $projectRoot 'python\config.json') -Raw | ConvertFrom-Json # Reads the configured HTTP port.
    $response = Invoke-WebRequest -Uri "http://localhost:$($config.web.port)" -UseBasicParsing -TimeoutSec 10 # Checks the page without issuing any light commands.
    $bootTrigger = @($task.Triggers | Where-Object { $_.CimClass.CimClassName -eq 'MSFT_TaskBootTrigger' }) # Distinguishes real boot startup from sign-in startup.
    if ($task.State -ne 'Running' -or $bootTrigger.Count -ne 1 -or $response.StatusCode -ne 200 -or $response.Content -notmatch 'Light Console') {
        throw 'The task must be running, have one boot trigger, and serve the Light Console page.'
    }
    [pscustomobject]@{
        TaskName = $task.TaskName
        State = [string]$task.State
        UserId = $task.Principal.UserId
        LogonType = [string]$task.Principal.LogonType
        BootDelay = $bootTrigger[0].Delay
        ExecutionTimeLimit = $task.Settings.ExecutionTimeLimit
        MultipleInstances = [string]$task.Settings.MultipleInstances
        RestartCount = $task.Settings.RestartCount
        RestartInterval = $task.Settings.RestartInterval
        DisallowStartIfOnBatteries = $task.Settings.DisallowStartIfOnBatteries
        StopIfGoingOnBatteries = $task.Settings.StopIfGoingOnBatteries
        LastRunTime = $info.LastRunTime.ToString('o')
        LastTaskResult = $info.LastTaskResult
        HttpStatus = $response.StatusCode
    } | ConvertTo-Json | Set-Content -LiteralPath $reportPath -Encoding UTF8 # Records the actual installed settings for review.
}
catch {
    @{ Error = ($_ | Out-String) } | ConvertTo-Json | Set-Content -LiteralPath $reportPath -Encoding UTF8 # Preserves failures from the hidden elevated process.
    exit 1
}
