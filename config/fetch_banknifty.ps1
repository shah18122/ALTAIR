# config/fetch_banknifty.ps1 -- BANKNIFTY spot and futures history from FYERS.
#
# WHY THIS EXISTS
#
# desktop/model_status.hpp lists "BANKNIFTY spot" and "BANKNIFTY future" as
# instruments the models cover. A model page naming an instrument with no data
# behind it is a label that outlives the thing it describes.
#
# RUN THE LOGIN FIRST. This needs a FYERS session issued TODAY: the 08:30
# 'ALTAIR FYERS Login' task (broker/tools/fyers_auto_login.py) makes one, or
# run it by hand:
#
#   py -3 broker\tools\fyers_auto_login.py
#
# SPOT goes through altair_dataset_update, which MERGES into
# dataset/spot/banknifty/<tf>/ and never drops a stored bar. NIFTY BANK is an
# index: its volume is ABSENT, not zero, and the updater leaves it empty.
#
# FUTURES go through altair_fyers_history with --continuous (cont_flag=1): the
# stitched near-month series. That is A DIFFERENT SERIES, not a longer one --
# the step at each roll is a contract change, not a market move. FYERS serves
# no expired contracts, so the continuous series is the only futures history
# there is. Each timeframe is written to one file under
# data/fyers_history/banknifty_fut/, never into dataset/ directly.
#
# DRY RUN BY DEFAULT. Pass -Go to make real calls.

param(
    [switch]$Go,
    [switch]$Force,
    [string]$From   = (Get-Date).AddYears(-2).ToString('yyyy-MM-dd'),
    [string]$To     = (Get-Date).ToString('yyyy-MM-dd'),
    [string]$Bin    = 'build\net\app',
    # Any live BANKNIFTY future; --continuous stitches the near month.
    [string]$Future = ''
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

$update  = Join-Path $Bin 'altair_dataset_update.exe'
$history = Join-Path $Bin 'altair_fyers_history.exe'
foreach ($exe in @($update, $history)) {
    if (-not (Test-Path $exe)) {
        Write-Host "no $exe -- build it first:" -ForegroundColor Red
        Write-Host "  .\build.bat net"
        exit 1
    }
}

if (-not $Future) {
    # The nearest BANKNIFTY future in data/instruments.csv (built from the
    # FYERS masters by altair_instrument_master / the price service).
    $rows = Select-String -Path data\instruments.csv -Pattern ',BANKNIFTY\d{2}[A-Z]{3}FUT,' -ErrorAction SilentlyContinue
    if ($rows) {
        $Future = 'NSE:' + (($rows | Select-Object -First 1).Line.Split(',')[2])
    } else {
        Write-Host 'no BANKNIFTY future in data\instruments.csv: run altair_instrument_master --go, or pass -Future NSE:BANKNIFTY..FUT' -ForegroundColor Red
        exit 1
    }
}

$failed = 0

# ---- spot: merged into dataset/ -------------------------------------------
$days = [int]((Get-Date $To) - (Get-Date $From)).TotalDays
$spotArgs = @('--symbols', 'banknifty', '--days', [string][Math]::Max(1, $days))
if ($Go) { $spotArgs += '--go' }
Write-Host "== BANKNIFTY spot  -> dataset/spot/banknifty" -ForegroundColor Cyan
& $update @spotArgs
if ($LASTEXITCODE -ne 0) { $failed++ }

# ---- futures: FYERS continuous series ---------------------------------------
$intervals = @(
    @{ res = '1';  dir = '1m'  },
    @{ res = '5';  dir = '5m'  },
    @{ res = '15'; dir = '15m' },
    @{ res = '60'; dir = '60m' },
    @{ res = 'D';  dir = '1d'  }
)
$outDir = 'data\fyers_history\banknifty_fut'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
foreach ($iv in $intervals) {
    $out = Join-Path $outDir "$($iv.dir).csv"
    Write-Host "== BANKNIFTY fut ($Future, continuous) $($iv.dir) -> $out" -ForegroundColor Cyan
    $a = @('--symbol', $Future, '--resolution', $iv.res, '--from', $From, '--to', $To,
           '--out', $out, '--continuous')
    if ($Force) { $a += '--force' }
    if ($Go)    { $a += '--go' }
    & $history @a
    if ($LASTEXITCODE -ne 0) { $failed++ }
}

Write-Host ''
if ($failed -gt 0) {
    # NAMED, not swallowed. A partial fetch that reports success leaves holes.
    Write-Host "$failed fetch(es) FAILED. An expired session is the usual cause: run the FYERS login." -ForegroundColor Red
    exit 1
}
if ($Go) {
    Write-Host 'All fetches reported success. Check the spot volume column is EMPTY, not 0.' -ForegroundColor Green
} else {
    Write-Host 'Dry run complete. Re-run with -Go to fetch.' -ForegroundColor Yellow
}
