# ops/record_session.ps1 -- record the whole FYERS session, whether or not ALTAIR is open.
#
# The "ALTAIR Recorder" scheduled task (ops\install_tasks.ps1) runs this at
# 08:50 IST, Monday to Friday. It:
#   1. starts altair_feed_recorder, which records everything the price service
#      publishes to data\store\tapes\<day>\ until 15:45 (it waits for the feed);
#   2. makes sure a FYERS feed is running on 127.0.0.1:7421. If the desktop
#      already runs one, that one is recorded. Otherwise it starts
#      altair_price_service --live with the RECORDER'S universe: NIFTY and
#      BANKNIFTY spot, INDIA VIX, the near futures, the nearest -Expiries
#      option expiries ATM +/- -Strikes (NIFTY's weeklies among them) plus the
#      nearest monthly if it is further out, the
#      NIFTY 50 and their futures, with the 5-level book on -DepthStrikes each
#      side and FYERS's 50-level book on the slots the plan allows;
#   3. if the feed stops (no FYERS login yet, a drop), tries again every
#      5 minutes until 15:30, and says so in the activity log.
#
# The desktop starts its own feed only when nothing streams, so it simply
# connects to this one when it opens.
#
# THE FYERS LOGIN IS STILL DAILY. A FYERS token lasts one day and is issued in
# the browser: log in on the Brokers page (or run altair_fyers_login) once each
# morning. Until then the activity log says "waiting for today's FYERS login".
#
#   powershell -ExecutionPolicy Bypass -File ops\record_session.ps1            # dry run
#   powershell -ExecutionPolicy Bypass -File ops\record_session.ps1 -Go

param(
    [switch]$Go,
    [string]$Until = '15:45',
    [int]$Strikes = 40,
    [int]$DepthStrikes = 10,
    [int]$Expiries = 3,
    [string]$Depth50 = '1',
    [string]$Bin = 'build\net\app'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
. "$PSScriptRoot\altair_log.ps1"

$recorder = Join-Path $Bin 'altair_feed_recorder.exe'
$service = Join-Path $Bin 'altair_price_service.exe'
foreach ($exe in @($recorder, $service)) {
    if (-not (Test-Path $exe)) {
        Write-AltairLog record ERROR "no $exe -- build it first: .\build.bat net"
        exit 1
    }
}

$feedArgs = @('--live', '--go', '--until', $Until, '--strikes', "$Strikes", '--depth-strikes', "$DepthStrikes",
              '--expiries', "$Expiries", '--monthly', '--depth50', $Depth50)
$recArgs = @('--until', $Until, '--go')
if (-not $Go) {
    Write-Host "DRY RUN. With -Go this runs:"
    Write-Host "  $recorder $($recArgs -join ' ')"
    Write-Host "  $service $($feedArgs -join ' ')   (only when nothing streams on 7421)"
    exit 0
}

function Test-FeedUp {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $t = $c.ConnectAsync('127.0.0.1', 7421)
        return $t.Wait(500) -and $c.Connected
    } catch { return $false } finally { $c.Dispose() }
}

$ist = Get-AltairIstNow
$day = $ist.ToString('yyyy-MM-dd')
$hm = $Until.Split(':')
$end = $ist.Date.AddHours([int]$hm[0]).AddMinutes([int]$hm[1])
$lastTry = $ist.Date.AddHours(15).AddMinutes(30)
if ($ist -ge $end) { Write-AltairLog record INFO "started after $Until IST; nothing to record today"; exit 0 }
$logDir = Join-Path (Get-Location) 'data\logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }

Write-AltairLog record INFO "session $day`: starting the recorder (until $Until IST)"
$rec = Start-Process -FilePath $recorder -ArgumentList $recArgs -NoNewWindow -PassThru `
    -RedirectStandardOutput (Join-Path $logDir "recorder-$day.out.txt")

$feed = $null
$started = 0
$waitingSince = $null
while ((Get-AltairIstNow) -lt $end -and -not $rec.HasExited) {
    if (Test-FeedUp) {
        if ($null -ne $waitingSince) { Write-AltairLog record INFO 'the feed is streaming'; $waitingSince = $null }
        Start-Sleep -Seconds 30
        continue
    }
    if ($null -ne $feed -and -not $feed.HasExited) { Start-Sleep -Seconds 10; continue }   # still starting
    if ($null -ne $feed) {
        $why = (Get-Content (Join-Path $logDir "price_service-$day.out.txt") -Tail 3 -ErrorAction SilentlyContinue) -join ' | '
        if ($why -match 'no live source today|no valid FYERS session|session') {
            if ($null -eq $waitingSince -or ((Get-AltairIstNow) - $waitingSince).TotalMinutes -ge 30) {
                Write-AltairLog record WARN "waiting for today's FYERS login (Brokers page, or altair_fyers_login): $why"
                $waitingSince = Get-AltairIstNow
            }
        } else {
            Write-AltairLog record WARN "the feed exited ($($feed.ExitCode)): $why"
        }
        $feed = $null
        if ((Get-AltairIstNow) -ge $lastTry) { break }
        Start-Sleep -Seconds 300
        continue
    }
    $started++
    Write-AltairLog record INFO "starting altair_price_service (attempt $started): $($feedArgs -join ' ')"
    $feed = Start-Process -FilePath $service -ArgumentList $feedArgs -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $logDir "price_service-$day.out.txt") `
        -RedirectStandardError (Join-Path $logDir "price_service-$day.err.txt")
    Start-Sleep -Seconds 15
}

if (-not $rec.HasExited) { $rec.WaitForExit() }
Write-AltairLog record INFO "session $day done: recorder exit $($rec.ExitCode), feed started $started time(s) by this task"
exit $rec.ExitCode
