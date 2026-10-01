# ops/record_depth.ps1 -- record a session of 5-level market depth for altair_depth_study.
#
# WHY THIS EXISTS
#
# Order-book signals (order flow imbalance, microprice) live at the
# seconds scale, and no history of NSE depth is in the dataset. The only way to
# study them is to record it: run this before 09:15 on a trading day and it
# listens until 15:30, appending every depth update -- stamped with its receive
# time -- to data\ticks\depth-<yyyyMMdd>.jsonl (git-ignored: broker data).
# A week of sessions is a start; a month is better.
#
# RUN THE LOGIN FIRST (a FYERS token is issued per day):
#   .\build\net\app\altair_fyers_login.exe
#
# DRY RUN BY DEFAULT: it prints the ticker command. Pass -Go to record.
# The near-month NIFTY and BANKNIFTY futures symbols are worked out from the
# date (monthly expiry: the last Tuesday, rolled the day after it); check them
# against the FYERS symbol master if a holiday moved the expiry.
#
#   powershell -ExecutionPolicy Bypass -File ops\record_depth.ps1 -Go
#   .\build\net\app\altair_depth_study.exe --in data\ticks

param(
    [switch]$Go,
    [string]$Symbols = '',
    [string]$Exe = 'build\net\app\altair_fyers_ticker.exe',
    [string]$Out = 'data\ticks'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

if (-not (Test-Path $Exe)) {
    Write-Host "no $Exe -- build it first:" -ForegroundColor Red
    Write-Host "  `$env:CMAKE_BUILD_PARALLEL_LEVEL='3'; .\build.bat net"
    exit 1
}

$ist = [System.TimeZoneInfo]::ConvertTimeBySystemTimeZoneId([datetime]::UtcNow, 'India Standard Time')

function LastTuesday([datetime]$d) {
    $last = (Get-Date -Year $d.Year -Month $d.Month -Day 1).AddMonths(1).AddDays(-1)
    while ($last.DayOfWeek -ne 'Tuesday') { $last = $last.AddDays(-1) }
    return $last.Date
}
$month = $ist.Date
if ($ist.Date -gt (LastTuesday $ist)) { $month = $month.AddMonths(1) }
$tag = $month.ToString('yy') + $month.ToString('MMM', [Globalization.CultureInfo]::InvariantCulture).ToUpper()

if ($Symbols -eq '') {
    $Symbols = @(
        "NSE:NIFTY${tag}FUT", "NSE:BANKNIFTY${tag}FUT",
        'NSE:RELIANCE-EQ', 'NSE:HDFCBANK-EQ', 'NSE:ICICIBANK-EQ', 'NSE:INFY-EQ', 'NSE:SBIN-EQ'
    ) -join ','
}

$close = $ist.Date.AddHours(15).AddMinutes(30)
$seconds = [int][Math]::Max(60, ($close - $ist).TotalSeconds)
if ($ist -gt $close) { $seconds = 60; Write-Host "after 15:30 IST: recording 60 s only (a smoke test)" -ForegroundColor Yellow }

if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }
$file = Join-Path $Out ('depth-' + $ist.ToString('yyyyMMdd') + '.jsonl')
$argv = @('--symbols', $Symbols, '--depth', '--jsonl', $file, '--stamp', '--seconds', "$seconds",
          '--out', 'data\fyers_ticks.json')
if ($Go) { $argv += '--go' }

Write-Host "symbols : $Symbols"
Write-Host "until   : 15:30 IST ($seconds s)"
Write-Host "file    : $file"
& $Exe @argv
exit $LASTEXITCODE
