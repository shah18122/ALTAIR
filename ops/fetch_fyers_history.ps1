# ops/fetch_fyers_history.ps1 -- the whole FYERS price history ALTAIR trains on, once.
#
# The nightly run (ops\nightly.ps1) adds each new day; this fills everything
# before it, as far back as FYERS serves (checked 2026-10-26): NIFTY and
# BANKNIFTY spot 1m from 2018, daily from 2005; INDIA VIX daily from 2010;
# the continuous NIFTY and BANKNIFTY futures 1m from 2019, daily from 2018.
# Expired option contracts are NOT served by FYERS (HTTP 422): option history
# exists only from ALTAIR's own recordings.
#
# Writes data\store\fyers\<seg>\<inst>\<tf>\history.csv beside the nightly's
# day files (the loader collapses duplicate stamps). Existing files are kept
# unless -Force. Needs today's FYERS login. Dry run by default.
#
#   powershell -ExecutionPolicy Bypass -File ops\fetch_fyers_history.ps1 -Go

param(
    [switch]$Go,
    [switch]$Force,
    [string]$To = '',
    [string]$Bin = 'build\net\app'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
. "$PSScriptRoot\altair_log.ps1"

if ($To -eq '') { $To = (Get-AltairIstNow).AddDays(-1).ToString('yyyy-MM-dd') }
$history = Join-Path $Bin 'altair_fyers_history.exe'
if (-not (Test-Path $history)) { Write-AltairLog history ERROR "no $history (build.bat net first)"; exit 2 }
. "$PSScriptRoot\nightly_series.ps1"
$series = Get-AltairSeries ([datetime]::ParseExact($To, 'yyyy-MM-dd', [Globalization.CultureInfo]::InvariantCulture))
$from = @{
    'spot\nifty'     = @{ intraday = '2018-01-01'; daily = '2005-01-01' }
    'spot\banknifty' = @{ intraday = '2018-01-01'; daily = '2005-01-01' }
    'spot\indiavix'  = @{ intraday = '2018-01-01'; daily = '2010-01-01' }
    'fut\nifty'      = @{ intraday = '2019-01-01'; daily = '2018-01-01' }
    'fut\banknifty'  = @{ intraday = '2019-01-01'; daily = '2018-01-01' }
}
$ok = 0; $bad = 0
foreach ($s in $series) {
    foreach ($tf in (Get-AltairTimeframes)) {
        $start = if ($tf.res -eq 'D') { $from[$s.key].daily } else { $from[$s.key].intraday }
        $out = "data\store\fyers\$($s.key)\$($tf.dir)\history.csv"
        if ((Test-Path $out) -and -not $Force) { Write-Host "kept $out"; continue }
        $a = @('--symbol', $s.sym, '--resolution', $tf.res, '--from', $start, '--to', $To, '--out', $out, '--force')
        if ($s.cont) { $a += '--continuous' }
        if (-not $Go) { Write-Host "would fetch: $history $($a -join ' ')"; continue }
        New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
        & $history @($a + '--go') 2>&1 | Select-Object -Last 2 | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -eq 0 -and (Test-Path $out)) {
            $ok++
            Write-AltairLog history INFO "$($s.sym) $($tf.dir) $start..$To -> $out ($((Get-Content $out | Measure-Object -Line).Lines) lines)"
        } else {
            $bad++
            Write-AltairLog history WARN "$($s.sym) $($tf.dir) $start..$To failed (exit $LASTEXITCODE)"
        }
    }
}
if (-not $Go) { Write-Host "`nDry run. Add -Go to fetch."; exit 0 }
Write-AltairLog history $(if ($bad -eq 0) { 'INFO' } else { 'WARN' }) "FYERS history: $ok series fetched, $bad failed"
exit $(if ($ok -gt 0) { 0 } else { 1 })
