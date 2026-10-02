# ops/fetch_pairs.ps1 -- daily closes for every stock pair in config/pairs.csv.
#
# WHY THIS EXISTS
#
# altair_pairs_futures walks every pair in config/pairs.csv forward, but
# dataset/ holds only NIFTY, BANKNIFTY and INDIA VIX. The stock legs come from
# FYERS, on a machine with a session -- broker data, so it lands in data/pairs/
# (git-ignored), never in dataset/ or a commit.
#
# RUN THE LOGIN FIRST. A FYERS access token is issued per day:
#
#   .\build\net\app\altair_fyers_login.exe
#
# DRY RUN BY DEFAULT: it prints the requests. Pass -Go to make them. An
# existing file is kept unless -Force. altair_fyers_history splits the range
# into FYERS-sized requests (366 days for daily candles) and paces them.
#
#   powershell -ExecutionPolicy Bypass -File ops\fetch_pairs.ps1 -Go
#   .\build\net\app\altair_pairs_futures.exe
#
# Legs under dataset/ (NIFTY-BANKNIFTY) are not fetched: they are already there.

param(
    [switch]$Go,
    [switch]$Force,
    [string]$From = '2015-01-01',
    [string]$To   = (Get-Date).ToString('yyyy-MM-dd'),
    [string]$Exe  = 'build\net\app\altair_fyers_history.exe',
    [string]$Pairs = 'config\pairs.csv'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

if (-not (Test-Path $Exe)) {
    Write-Host "no $Exe -- build it first:" -ForegroundColor Red
    Write-Host "  `$env:CMAKE_BUILD_PARALLEL_LEVEL='3'; .\build.bat net"
    exit 1
}

# pair,leg_a,a_fyers,a_dir,leg_b,b_fyers,b_dir,ratio_cap
$legs = @{}
foreach ($line in Get-Content $Pairs) {
    if ($line -match '^\s*(#|$)') { continue }
    $f = $line.Split(',')
    if ($f.Count -lt 7) { continue }
    foreach ($leg in @(@($f[1], $f[2], $f[3]), @($f[4], $f[5], $f[6]))) {
        if ($leg[2] -like 'dataset/*') { continue }   # tracked already
        $legs[$leg[1]] = $leg[2]
    }
}

$failed = 0
foreach ($symbol in ($legs.Keys | Sort-Object)) {
    $dir = $legs[$symbol] -replace '/', '\'
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
