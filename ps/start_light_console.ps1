param(
    [string]$PythonExecutable = 'py',
    [string]$LogDirectory = ''
)

$ErrorActionPreference = 'Stop' # Makes launcher failures visible to Task Scheduler.
$projectRoot = Split-Path -Parent $PSScriptRoot # Resolves the project regardless of the caller's working directory.
$appPath = Join-Path $projectRoot 'python\app.py' # Selects the companion web application's Python entry point.

function Join-KillOnCloseJob {
    param([System.Diagnostics.Process]$Process)

    if (-not ('LightConsole.KillOnCloseJob' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace LightConsole {
    public static class KillOnCloseJob {
        [StructLayout(LayoutKind.Sequential)]
        struct BasicLimits {
            public long PerProcessUserTimeLimit;
            public long PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize;
            public UIntPtr MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass;
            public uint SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct IoCounters {
            public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
            public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct ExtendedLimits {
            public BasicLimits Basic;
            public IoCounters Io;
            public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
        }

        const int ExtendedLimitInformation = 9;           // JobObjectExtendedLimitInformation
        const uint KillOnJobClose = 0x2000;               // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern IntPtr CreateJobObject(IntPtr attributes, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtendedLimits info, uint length);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        // Creates an unnamed job that kills its members when its last handle (held by this launcher) closes.
        public static IntPtr Attach(IntPtr process) {
            IntPtr job = CreateJobObject(IntPtr.Zero, null);
            if (job == IntPtr.Zero) throw new Win32Exception();
            ExtendedLimits info = new ExtendedLimits();
            info.Basic.LimitFlags = KillOnJobClose;
            if (!SetInformationJobObject(job, ExtendedLimitInformation, ref info, (uint)Marshal.SizeOf(typeof(ExtendedLimits)))) throw new Win32Exception();
            if (!AssignProcessToJobObject(job, process)) throw new Win32Exception();
            return job;
        }
    }
}
'@ # Compiles the small Windows job-object helper once per launcher run.
    }
    return [LightConsole.KillOnCloseJob]::Attach($Process.Handle) # Ties Python's lifetime to this launcher process.
} # Completes kill-on-close job creation so stopping the task also stops Python.

if ($LogDirectory) {
    New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null # Ensures background logs have a destination.
    foreach ($stream in 'stdout', 'stderr') {
        $logPath = Join-Path $LogDirectory "$stream.log" # Keeps HTTP output separate from startup errors.
        if (Test-Path -LiteralPath $logPath) {
            Copy-Item -LiteralPath $logPath -Destination "$logPath.previous" -Force # Retains the previous run for diagnosis.
        }
    }
    $server = Start-Process -FilePath $PythonExecutable -ArgumentList @('-u', ('"{0}"' -f $appPath)) -WorkingDirectory $projectRoot -WindowStyle Hidden -RedirectStandardOutput (Join-Path $LogDirectory 'stdout.log') -RedirectStandardError (Join-Path $LogDirectory 'stderr.log') -PassThru # Runs Python without buffering in its own hidden process.
    $null = $server.Handle # Caches the process handle so ExitCode is readable after Python exits.
    try {
        $script:serverJob = Join-KillOnCloseJob -Process $server # Holds the job open for this launcher's lifetime; Windows kills Python when it closes.
    } catch {
        Write-Warning "Python will outlive this launcher if the task is stopped: $($_.Exception.Message)" # Keeps the server running even if the safety net cannot be created.
    }
    $server.WaitForExit() # Blocks until Python exits so Task Scheduler sees the task as running.
    exit $server.ExitCode # Lets Task Scheduler restart the server after a failure.
}

& $PythonExecutable -u $appPath # Starts the HTTP and MQTT services interactively when logging is not requested.
exit $LASTEXITCODE # Preserves Python's exit status for callers.
