# ops/fetch_bhavcopy.ps1 -- NSE F&O bhavcopy, one file per trading day.
#
# WHY THIS EXISTS
#
# altair_vol_premium --source bhavcopy prices real at-the-money straddles and
# futures from NSE's end-of-day F&O files. They are public exchange data, but
# bulky (every strike of every contract), so they are fetched per machine into
# data/bhavcopy/ (git-ignored) and never committed.
#
# TWO FORMATS, BOTH HANDLED BY app/bhavcopy.hpp:
#   through 2024-07-05  archives.nseindia.com/content/historical/DERIVATIVES/<YYYY>/<MON>/fo<DD><MON><YYYY>bhav.csv.zip
#   from 2024-07-08     nsearchives.nseindia.com/content/fo/BhavCopy_NSE_FO_0_0_0_<YYYYMMDD>_F_0000.csv.zip
# A weekday with no file is a holiday: counted, not an error.
#
# DRY RUN BY DEFAULT: it prints what it would fetch. Pass -Go to fetch. An
# existing day is kept unless -Force. Requests are paced (-DelayMs) -- NSE
# refuses clients that hammer it.
#
#   powershell -ExecutionPolicy Bypass -File ops\fetch_bhavcopy.ps1 -From 2015-01-01 -Go
#   .\build\net\app\altair_vol_premium.exe --source bhavcopy --unverified-costs

param(
    [switch]$Go,
    [switch]$Force,
    [string]$From = '2015-01-01',
    [string]$To   = (Get-Date).ToString('yyyy-MM-dd'),
    [string]$Out  = 'data\bhavcopy',
    [int]$DelayMs = 400
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

$udiffFrom = [datetime]'2024-07-08'
$headers = @{
    'User-Agent'      = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36'
    'Accept'          = '*/*'
    'Accept-Language' = 'en-US,en;q=0.9'
    'Referer'         = 'https://www.nseindia.com/'
}

$day = [datetime]$From
$end = [datetime]$To
$fetched = 0; $kept = 0; $holidays = 0; $failed = 0
while ($day -le $end) {
    if ($day.DayOfWeek -eq 'Saturday' -or $day.DayOfWeek -eq 'Sunday') { $day = $day.AddDays(1); continue }
    $mon = $day.ToString('MMM', [Globalization.CultureInfo]::InvariantCulture).ToUpper()
    if ($day -ge $udiffFrom) {
        $name = 'BhavCopy_NSE_FO_0_0_0_' + $day.ToString('yyyyMMdd') + '_F_0000.csv'
        $url = 'https://nsearchives.nseindia.com/content/fo/' + $name + '.zip'
    } else {
        $name = 'fo' + $day.ToString('dd') + $mon + $day.ToString('yyyy') + 'bhav.csv'
        $url = 'https://archives.nseindia.com/content/historical/DERIVATIVES/' + $day.ToString('yyyy') + '/' + $mon + '/' + $name + '.zip'
    }
    $dir = Join-Path $Out $day.ToString('yyyy')
    $csv = Join-Path $dir $name
    if ((Test-Path $csv) -and -not $Force) { $kept++; $day = $day.AddDays(1); continue }
    if (-not $Go) {
        Write-Host "would fetch $url"
        $day = $day.AddDays(1)
        continue
    }
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $zip = Join-Path $env:TEMP ($name + '.zip')
    try {
        Invoke-WebRequest -Uri $url -Headers $headers -OutFile $zip -UseBasicParsing -TimeoutSec 60
        Expand-Archive -Path $zip -DestinationPath $dir -Force
        Remove-Item $zip -ErrorAction SilentlyContinue
        $fetched++
        Write-Host "$($day.ToString('yyyy-MM-dd'))  ok"
    } catch {
        $code = $null
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        if ($code -eq 404) { $holidays++ }
        else { $failed++; Write-Host "$($day.ToString('yyyy-MM-dd'))  failed: $($_.Exception.Message)" -ForegroundColor Yellow }
    }
    Start-Sleep -Milliseconds $DelayMs
    $day = $day.AddDays(1)
}

Write-Host "`nfetched $fetched, already had $kept, no file (holiday) $holidays, failed $failed"
if (-not $Go) { Write-Host "Dry run. Pass -Go to fetch." -ForegroundColor Cyan }
if ($failed -gt 0) { exit 1 }
