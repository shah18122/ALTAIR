# ops/install_tasks.ps1 -- register ALTAIR's two Windows scheduled tasks (or remove them).
#
#   ALTAIR Recorder   Mon-Fri 08:50  ops\record_session.ps1 -Go   (records the session to 15:45)
#   ALTAIR Nightly    Mon-Fri 20:00  ops\nightly.ps1 -Go          (store, snapshot, retrain, push)
#
# Both run whether or not the ALTAIR desktop is open, start late if the PC was
# asleep or off at their time (StartWhenAvailable), and wake the PC for it.
# They run as YOU, while you are logged on to Windows (the default): that is
# what lets the nightly push use your own git login. -WhetherLoggedOn runs
# them with you logged off too (S4U: no password is stored, but such a task
# cannot reach Git Credential Manager, so the push step fails until you log on).
#
# The times are this PC's clock; the scripts themselves work in IST.
#
#   powershell -ExecutionPolicy Bypass -File ops\install_tasks.ps1            # dry run: what it would register
#   powershell -ExecutionPolicy Bypass -File ops\install_tasks.ps1 -Go
#   powershell -ExecutionPolicy Bypass -File ops\install_tasks.ps1 -Remove -Go
#   Get-ScheduledTask 'ALTAIR*' | Get-ScheduledTaskInfo                        # last run, result

param(
    [switch]$Go,
    [switch]$Remove,
    [switch]$WhetherLoggedOn,
    [string]$RecordAt = '08:50',
    [string]$NightlyAt = '20:00'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot\altair_log.ps1"
$names = @('ALTAIR Recorder', 'ALTAIR Nightly')

if ($Remove) {
    foreach ($n in $names) {
        if (-not $Go) { Write-Host "would remove: $n"; continue }
        if (Get-ScheduledTask -TaskName $n -ErrorAction SilentlyContinue) {
            Unregister-ScheduledTask -TaskName $n -Confirm:$false
            Write-AltairLog tasks INFO "scheduled task removed: $n"
        }
    }
    exit 0
}

$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$tasks = @(
    @{ name = 'ALTAIR Recorder'; at = $RecordAt; script = 'record_session.ps1'; hours = 8
       what = 'Records the whole FYERS session (ticks, quotes, 5/50-level books) to data\store\tapes, whether or not ALTAIR is open.' },
    @{ name = 'ALTAIR Nightly'; at = $NightlyAt; script = 'nightly.ps1'; hours = 10
       what = 'Stores the day, merges dataset\ and FYERS into one snapshot at one cutoff, retrains with the leak check, pushes the compact data.' }
)
foreach ($t in $tasks) {
    $arg = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$root\ops\$($t.script)`" -Go"
    if (-not $Go) {
        Write-Host "would register '$($t.name)': Mon-Fri $($t.at), $ps $arg (in $root)"
        continue
    }
    $action = New-ScheduledTaskAction -Execute $ps -Argument $arg -WorkingDirectory $root
    $trigger = New-ScheduledTaskTrigger -Weekly -DaysOfWeek Monday, Tuesday, Wednesday, Thursday, Friday -At $t.at
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -WakeToRun -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit (New-TimeSpan -Hours $t.hours) -MultipleInstances IgnoreNew
    $user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    $principal = if ($WhetherLoggedOn) {
        New-ScheduledTaskPrincipal -UserId $user -LogonType S4U -RunLevel Limited
    } else {
        New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    }
    Register-ScheduledTask -TaskName $t.name -Description $t.what -Action $action -Trigger $trigger -Settings $settings `
        -Principal $principal -Force | Out-Null
    Write-AltairLog tasks INFO "scheduled task registered: $($t.name), Mon-Fri $($t.at) as $user"
}
if (-not $Go) { Write-Host "`nDry run. Add -Go to register." }
