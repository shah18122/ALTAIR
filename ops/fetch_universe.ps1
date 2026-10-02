# ops/fetch_universe.ps1 -- daily closes for every stock in config/universe_nifty50.csv.
#
# altair_resid_reversion trades the whole universe against the market and
# each stock's sector peers. The stock data is broker data, so it lands in
# data/pairs/ (git-ignored) -- the same directories config/pairs.csv reads, so
# one fetch serves both.
#
# RUN THE LOGIN FIRST (a FYERS token is issued per day):
#   .\build\net\app\altair_fyers_login.exe
#
# DRY RUN BY DEFAULT: it prints the requests. Pass -Go to make them. An
# existing file is kept unless -Force.
#
#   powershell -ExecutionPolicy Bypass -File ops\fetch_universe.ps1 -Go
#   .\build\net\app\altair_resid_reversion.exe --unverified-costs

param(
    [switch]$Go,
    [switch]$Force,
    [string]$From = '2015-01-01',
    [string]$To   = (Get-Date).ToString('yyyy-MM-dd'),
    [string]$Exe  = 'build\net\app\altair_fyers_history.exe',
    [string]$Universe = 'config\universe_nifty50.csv'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

if (-not (Test-Path $Exe)) {
    Write-Host "no $Exe -- build it first:" -ForegroundColor Red
    Write-Host "  `$env:CMAKE_BUILD_PARALLEL_LEVEL='3'; .\build.bat net"
    exit 1
}

$failed = 0
foreach ($line in Get-Content $Universe) {
    if ($line -match '^\s*(#|$)' -or $line -like 'symbol,*') { continue }
    $f = $line.Split(',')
    if ($f.Count -lt 4) { continue }
    $symbol = $f[1]
    $dir = $f[3] -replace '/', '\'
    $out = Join-Path $dir 'fyers.csv'
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $argv = @('--symbol', $symbol, '--resolution', 'D', '--from', $From, '--to', $To, '--out', $out)
    if ($Force) { $argv += '--force' }
    if ($Go) { $argv += '--go' }
    Write-Host "$symbol -> $out"
    & $Exe @argv
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  failed ($LASTEXITCODE): $symbol" -ForegroundColor Yellow
        $failed++
    }
    if ($Go) { Start-Sleep -Milliseconds 400 }
}

if (-not $Go) { Write-Host "`nDry run. Pass -Go to fetch." -ForegroundColor Cyan }
if ($failed -gt 0) { Write-Host "$failed symbol(s) failed" -ForegroundColor Yellow; exit 1 }
