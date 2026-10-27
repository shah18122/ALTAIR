# ops/fetch_fyers_meta.ps1 -- the FYERS history the meta-trader reads, and the run.
#
# `altair_trader combos` (app/trader_combos.cpp) reads FYERS data only. This
# fetches it, one year per request file, into data/fyers_meta/ (git-ignored
# broker data), writes data/fyers_meta/SOURCE.txt, and with -Run runs the
# meta-trader on it:
#
#   data/fyers_meta/NIFTY/1m          NSE:NIFTY50-INDEX, 1 minute
#   data/fyers_meta/BANKNIFTY/1m      NSE:NIFTYBANK-INDEX, 1 minute
#   data/fyers_meta/INDIAVIX/1d       NSE:INDIAVIX-INDEX, daily
#   data/fyers_meta/INDIAVIX/1m       NSE:INDIAVIX-INDEX, 1 minute (where FYERS has it)
#   data/fyers_meta/NIFTY_FUT/1m      the near future, continuous (cont_flag=1)
#   data/fyers_meta/BANKNIFTY_FUT/1m  the same for BANKNIFTY
#
# FYERS keeps no expired option contracts, so there is no option history to
# fetch: the meta-trader prices option legs with a model and says so.
#
# RUN THE LOGIN FIRST (the token is issued per day):
#
#   .\build\net\app\altair_fyers_login.exe
#   powershell -ExecutionPolicy Bypass -File ops\fetch_fyers_meta.ps1 -Go -Run
#
# DRY RUN BY DEFAULT: it prints the requests. A year already fetched is kept
# unless -Force (the current year is always fetched again). A year FYERS has no
# history for is logged and skipped.

param(
    [switch]$Go,
    [switch]$Force,
    [switch]$Run,
    [switch]$WithCurriculum,
    [switch]$UnverifiedCosts,
    [string]$From  = '2018-01-01',
    [string]$To    = (Get-Date).ToString('yyyy-MM-dd'),
    [string]$Build = 'build\net',
    [string]$Out   = 'data\fyers_meta'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

function Find-Helper([string]$name) {
    foreach ($c in @((Join-Path $Build "app\$name.exe"), (Join-Path $Build "app\$name"))) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    throw "$name is not built under $Build. Run: build.bat net"
}
$exe = Find-Helper 'altair_fyers_history'

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $Out 'fetch_log.txt'
function Write-Log([string]$m) {
    $line = '{0}  {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m
    Write-Host $line
    Add-Content -Path $log -Value $line
}

# The near futures, from the instrument master in git (expiry on or after today).
function Near-Future([string]$name) {
    if (-not (Test-Path 'data\instruments.csv')) { return $null }
    $today = (Get-Date).Date
    $f = Import-Csv 'data\instruments.csv' |
        Where-Object { $_.exchange -eq 'NFO' -and $_.name -eq $name -and $_.instrument_type -eq 'FUT' -and $_.expiry -and ([datetime]$_.expiry) -ge $today } |
        Sort-Object { [datetime]$_.expiry } | Select-Object -First 1
    if ($f) { return 'NSE:' + $f.tradingsymbol }
    return $null
}

$series = @(
    @{ Dir = 'NIFTY\1m';     Symbol = 'NSE:NIFTY50-INDEX';   Res = '1'; Cont = $false },
    @{ Dir = 'BANKNIFTY\1m'; Symbol = 'NSE:NIFTYBANK-INDEX'; Res = '1'; Cont = $false },
    @{ Dir = 'INDIAVIX\1d';  Symbol = 'NSE:INDIAVIX-INDEX';  Res = 'D'; Cont = $false },
    @{ Dir = 'INDIAVIX\1m';  Symbol = 'NSE:INDIAVIX-INDEX';  Res = '1'; Cont = $false }
)
foreach ($n in @('NIFTY', 'BANKNIFTY')) {
    $sym = Near-Future $n
    if ($sym) { $series += @{ Dir = "${n}_FUT\1m"; Symbol = $sym; Res = '1'; Cont = $true } }
    else { Write-Log "no current $n future in data\instruments.csv: ${n}_FUT skipped (the index price is used)" }
}

$fromDate = [datetime]::ParseExact($From, 'yyyy-MM-dd', $null)
$toDate = [datetime]::ParseExact($To, 'yyyy-MM-dd', $null)
$failed = 0
foreach ($s in $series) {
    $dir = Join-Path $Out $s.Dir
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    for ($y = $fromDate.Year; $y -le $toDate.Year; $y++) {
        $a = [datetime]::new($y, 1, 1);   if ($a -lt $fromDate) { $a = $fromDate }
        $b = [datetime]::new($y, 12, 31); if ($b -gt $toDate) { $b = $toDate }
        $file = Join-Path $dir "$y.csv"
        if ((Test-Path $file) -and -not $Force -and $y -lt $toDate.Year) { continue }
        $argv = @('--symbol', $s.Symbol, '--resolution', $s.Res, '--from', $a.ToString('yyyy-MM-dd'),
                  '--to', $b.ToString('yyyy-MM-dd'), '--out', $file, '--force')
        if ($s.Cont) { $argv += '--continuous' }
        if ($Go) { $argv += '--go' }
        $output = & $exe @argv 2>&1
        $last = ($output | Select-Object -Last 1) -join ''
        if ($LASTEXITCODE -eq 0) { Write-Log "ok      $($s.Symbol) $($s.Res) $y" }
        else { Write-Log "skipped $($s.Symbol) $($s.Res) $y -- $last"; $failed++ }
    }
}
if (-not $Go) { Write-Host 'DRY RUN: nothing fetched. Add -Go.'; exit 0 }

Set-Content -Path (Join-Path $Out 'SOURCE.txt') -Value ("FYERS history API, fetched {0} by ops/fetch_fyers_meta.ps1" -f (Get-Date -Format 'yyyy-MM-dd HH:mm'))
Write-Log "done, $failed request files skipped"

if ($Run) {
    $trader = Find-Helper 'altair_trader'
    $argv = @('combos', '--fyers', ($Out -replace '\\', '/'), '--leak-check')
    # The curriculum's logs are built from dataset/ (not FYERS) today: opt in only.
    if ($WithCurriculum) { $argv += @('--logs', 'data/verified/forecast_log') }
    if ($UnverifiedCosts) { $argv += '--unverified-costs' }
    & $trader @argv
    if ($LASTEXITCODE -ne 0) { throw "altair_trader combos failed (exit $LASTEXITCODE)" }
    Write-Host 'Report: data\verified\altair_trader\combos\report.html'
}
