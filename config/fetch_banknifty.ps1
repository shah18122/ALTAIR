# config/fetch_banknifty.ps1 -- fill dataset/ for BANKNIFTY, spot and futures.
#
# WHY THIS EXISTS
#
# dataset/ holds NIFTY spot, NIFTY futures and INDIA VIX, and nothing for
# BANKNIFTY -- while desktop/model_status.hpp lists "BANKNIFTY spot" and
# "BANKNIFTY future" as instruments the models cover. A model page naming an
# instrument with no data behind it is the same failure this repo keeps
# catching: a label that outlives the thing it describes.
#
# RUN THE LOGIN FIRST. This needs a session issued TODAY.
#
# Kite access tokens are daily. If the desktop pill says SESSION EXPIRED, or
# this script stops on HTTP 403 TokenException, the fix is a browser login --
# not a retry.
#
#   .\build\net\app\altair_kite_login.exe
#
# TOKENS, READ FROM THE MASTER RATHER THAN TYPED FROM MEMORY
#
#   260105    NIFTY BANK        NSE / INDICES -- an INDEX, so --volume-absent
#   17507842  BANKNIFTY26SEPFUT NFO -- near month, so --oi and --continuous
#
# The index one matters and has bitten this project twice already: an index
# does not trade, so its volume is ABSENT, not zero. Writing 0 there puts a
# measurement in the file where there was none, and every downstream turnover
# and VWAP figure silently believes it.
#
# The futures token is the NEAR MONTH as of 2026-09. --continuous stitches the
# series back across contract rolls, which produces A DIFFERENT SERIES, not a
# longer one -- the step at each roll is a contract change, not a market move.
# Re-check the token against data/instruments.csv after the September expiry:
#
#   Select-String -Path data\instruments.csv -Pattern 'BANKNIFTY\d+FUT'
#
# DRY RUN BY DEFAULT. Pass -Go to make real calls.

param(
    [switch]$Go,
    [switch]$Force,
    [string]$From = (Get-Date).AddYears(-2).ToString('yyyy-MM-dd'),
    [string]$To   = (Get-Date).ToString('yyyy-MM-dd'),
    [string]$Exe  = 'build\net\app\altair_kite_fetch.exe'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

if (-not (Test-Path $Exe)) {
    Write-Host "no $Exe -- build it first:" -ForegroundColor Red
    Write-Host "  `$env:CMAKE_BUILD_PARALLEL_LEVEL='3'; .\build.bat net"
    exit 1
}

$spotToken = 260105
$futToken  = 17507842

# interval  ->  directory name. The names are the ones already on disk for
# NIFTY, so the two symbols partition identically and a fold defined by a path
# keeps working (CLAUDE.md, dataset/ layout).
$intervals = @(
    @{ api = 'minute';   dir = '1m'  },
    @{ api = '5minute';  dir = '5m'  },
    @{ api = '15minute'; dir = '15m' },
    @{ api = '60minute'; dir = '60m' },
    @{ api = 'day';      dir = '1d'  }
)

$common = @('--from', $From, '--to', $To)
if ($Go)    { $common += '--go' }
if ($Force) { $common += '--force' }

if (-not $Go) {
    Write-Host ''
    Write-Host 'DRY RUN. Nothing will be sent. Add -Go to fetch for real.' -ForegroundColor Yellow
    Write-Host ''
}

$failed = 0
foreach ($iv in $intervals) {
    # ---- spot: an INDEX, so volume is absent ------------------------------
    $out = "dataset/spot/banknifty/$($iv.dir)"
    Write-Host "== BANKNIFTY spot  $($iv.api)  -> $out" -ForegroundColor Cyan
    & $Exe --token $spotToken --out $out --interval $iv.api --volume-absent @common
    if ($LASTEXITCODE -ne 0) { $failed++ }

    # ---- futures: volume and OI are real measurements ---------------------
    #
    # --continuous is DAY-ONLY. Kite answers any other interval with 400
    # "invalid interval for continuous data", and the fetcher refuses the
    # combination before sending rather than letting the API say it -- which
    # is how this script's first version was caught, on a dry run.
    #
    # So the intraday futures series is the SEPTEMBER CONTRACT ALONE, roughly
    # three months deep, and the daily series is stitched across rolls. Those
    # are different series with different meanings and they are not
    # interchangeable: the daily one has a step at every roll that is a
    # contract change rather than a market move.
    $out = "dataset/fut/banknifty/$($iv.dir)"
    Write-Host "== BANKNIFTY fut   $($iv.api)  -> $out" -ForegroundColor Cyan
    $futArgs = @('--oi')
    if ($iv.api -eq 'day') { $futArgs += '--continuous' }
    & $Exe --token $futToken --out $out --interval $iv.api @futArgs @common
    if ($LASTEXITCODE -ne 0) { $failed++ }
}

Write-Host ''
if ($failed -gt 0) {
    # NAMED, not swallowed. A partial fetch that reports success leaves holes
    # that only show up as a model quietly training on less data than it says.
    Write-Host "$failed of $($intervals.Count * 2) fetches FAILED." -ForegroundColor Red
    Write-Host 'HTTP 403 TokenException means the session is stale: run'
    Write-Host '  .\build\net\app\altair_kite_login.exe'
    exit 1
}

if ($Go) {
    Write-Host 'All fetches reported success. Now VERIFY rather than assume:' -ForegroundColor Green
    Write-Host '  Get-ChildItem -Recurse dataset\spot\banknifty,dataset\fut\banknifty -Filter *.csv | Measure-Object'
    Write-Host '  Get-Content dataset\spot\banknifty\1d\all.csv -TotalCount 3'
    Write-Host ''
    Write-Host 'Check the volume column on the SPOT files is EMPTY, not 0.'
} else {
    Write-Host 'Dry run complete. Re-run with -Go to fetch.' -ForegroundColor Yellow
}
