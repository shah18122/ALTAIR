<#
.SYNOPSIS
  Cross-check dataset/ against FYERS and Kite, then write the audit workbook.

.DESCRIPTION
  1. Fetches broker candles with the existing READ-ONLY helpers
       altair_fyers_history  -> data\broker_audit\fyers\<segment>\<instrument>\<tf>\<year>.csv
       altair_kite_fetch     -> data\broker_audit\kite\<segment>\<instrument>\<tf>\<YYYY-MM>.csv
     one year per call, so a year the broker has no history for is logged and
     skipped instead of stopping the run. Already-fetched years are kept
     (the run resumes); -Force fetches them again.
  2. Runs altair_data_audit over dataset\ with both broker trees and writes
       data\verified\data_audit.xlsx   (Summary + one sheet per instrument)
       data\verified\merged\*.csv      (one clean file per instrument per timeframe)
     and opens the workbook.

  Needs a linked FYERS session (data\fyers_session.json) and/or Kite session
  (data\kite_session.json). Nothing here can place an order: both helpers only
  call the historical-candle GET endpoints. data\broker_audit\ and
  data\verified\ are git-ignored.

  Futures: FYERS serves a continuous near-month series at every resolution
  (cont_flag=1); Kite serves continuous futures for daily candles only.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ops\broker_audit.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ops\broker_audit.ps1 -From 2024-01-01 -SkipKite
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ops\broker_audit.ps1 -AuditOnly
  (re-run only the comparison over what is already fetched)
#>
param(
    [string]$Build = "build/net",
    [string]$From = "2015-01-01",
    [string]$To = (Get-Date).ToString("yyyy-MM-dd"),
    [switch]$SkipFyers,
    [switch]$SkipKite,
    [switch]$AuditOnly,
    [switch]$Force,
    [switch]$NoOpen
)

$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)   # the repository root

function Find-Helper([string]$name) {
    foreach ($candidate in @((Join-Path $Build "app/$name.exe"), (Join-Path $Build "app/$name"))) {
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    }
    throw "$name is not built under $Build. Run: build.bat net"
}

New-Item -ItemType Directory -Force -Path "data/broker_audit" | Out-Null
$log = "data/broker_audit/fetch_log.txt"
function Write-Log([string]$message) {
    $line = "{0}  {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $message
    Write-Host $line
    Add-Content -Path $log -Value $line
}

# Kite refuses to rewrite a month it was only partly asked for, so windows
# start on the first of a month.
$fromDate = [datetime]::ParseExact($From, "yyyy-MM-dd", $null)
$fromDate = [datetime]::new($fromDate.Year, $fromDate.Month, 1)
$toDate = [datetime]::ParseExact($To, "yyyy-MM-dd", $null)
$years = for ($y = $fromDate.Year; $y -le $toDate.Year; $y++) {
    $a = [datetime]::new($y, 1, 1); if ($a -lt $fromDate) { $a = $fromDate }
    $b = [datetime]::new($y, 12, 31); if ($b -gt $toDate) { $b = $toDate }
    [pscustomobject]@{ Year = $y; From = $a.ToString("yyyy-MM-dd"); To = $b.ToString("yyyy-MM-dd") }
}

# Series in dataset\ and their broker identities. Kite tokens are the index
# tokens the Terminal already uses (NIFTY 50, NIFTY BANK, INDIA VIX).
$indices = @(
    @{ Seg = "spot"; Inst = "nifty";     Fyers = "NSE:NIFTY50-INDEX";   Kite = 256265 },
    @{ Seg = "spot"; Inst = "banknifty"; Fyers = "NSE:NIFTYBANK-INDEX"; Kite = 260105 },
    @{ Seg = "spot"; Inst = "indiavix";  Fyers = "NSE:INDIAVIX-INDEX";  Kite = 264969 }
)
$timeframes = @(
    @{ Name = "1m";  Fyers = "1";  Kite = "minute" },
    @{ Name = "5m";  Fyers = "5";  Kite = "5minute" },
    @{ Name = "15m"; Fyers = "15"; Kite = "15minute" },
    @{ Name = "60m"; Fyers = "60"; Kite = "60minute" },
    @{ Name = "1d";  Fyers = "D";  Kite = "day" }
)

# The NIFTY near-month future, from the Kite instrument master in git.
$future = $null
if (Test-Path "data/instruments.csv") {
    $today = (Get-Date).Date
    $future = Import-Csv "data/instruments.csv" |
        Where-Object { $_.exchange -eq "NFO" -and $_.name -eq "NIFTY" -and $_.instrument_type -eq "FUT" -and $_.expiry -and ([datetime]$_.expiry) -ge $today } |
        Sort-Object { [datetime]$_.expiry } | Select-Object -First 1
}
if (-not $future) {
    Write-Log "no current NIFTY future in data\instruments.csv; futures skipped (refresh it: altair_kite_fetch --dump-instruments data\instruments.csv)"
}

function Invoke-Helper([string]$exe, [string[]]$argv, [string]$what) {
    $output = & $exe @argv 2>&1
    $last = ($output | Select-Object -Last 1) -join ""
    if ($LASTEXITCODE -eq 0) { Write-Log "ok      $what" ; return $true }
    Write-Log "skipped $what -- $last"
    return $false
}

function Get-Fyers([string]$seg, [string]$inst, [string]$symbol, $tf, [bool]$continuous) {
    $dir = "data/broker_audit/fyers/$seg/$inst/$($tf.Name)"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    foreach ($w in $years) {
        $out = Join-Path $dir "$($w.Year).csv"
        if ((Test-Path $out) -and -not $Force) { continue }
        $argv = @("--symbol", $symbol, "--resolution", $tf.Fyers, "--from", $w.From, "--to", $w.To,
                  "--out", $out, "--force", "--go")
        if ($continuous) { $argv += "--continuous" }
        [void](Invoke-Helper $script:fyersExe $argv "FYERS $symbol $($tf.Name) $($w.Year)")
    }
}

function Get-Kite([string]$seg, [string]$inst, [string]$token, $tf, [bool]$index, [bool]$continuous) {
    $dir = "data/broker_audit/kite/$seg/$inst/$($tf.Name)"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    foreach ($w in $years) {
        $marker = Join-Path $dir ".done_$($w.Year)"
        if ((Test-Path $marker) -and -not $Force) { continue }
        $argv = @("--token", $token, "--out", $dir, "--interval", $tf.Kite, "--from", $w.From, "--to", $w.To,
                  "--force", "--go")
        if ($index) { $argv += "--volume-absent" }
        if ($continuous) { $argv += @("--continuous", "--oi") }
        if (Invoke-Helper $script:kiteExe $argv "Kite $token $($tf.Name) $($w.Year)") {
            New-Item -ItemType File -Force -Path $marker | Out-Null
        }
    }
}

if (-not $AuditOnly) {
    if (-not $SkipFyers) {
        $script:fyersExe = Find-Helper "altair_fyers_history"
        foreach ($s in $indices) { foreach ($tf in $timeframes) { Get-Fyers $s.Seg $s.Inst $s.Fyers $tf $false } }
        if ($future) {
            foreach ($tf in $timeframes) { Get-Fyers "fut" "nifty" ("NSE:" + $future.tradingsymbol) $tf $true }
        }
    }
    if (-not $SkipKite) {
        $script:kiteExe = Find-Helper "altair_kite_fetch"
        foreach ($s in $indices) { foreach ($tf in $timeframes) { Get-Kite $s.Seg $s.Inst $s.Kite $tf $true $false } }
        if ($future) {
            Get-Kite "fut" "nifty" $future.instrument_token ($timeframes | Where-Object { $_.Name -eq "1d" }) $false $true
        }
    }
}

$audit = Find-Helper "altair_data_audit"
# Forward slashes: std::filesystem accepts them on Windows too.
$auditArgs = @("--dataset", "dataset", "--out", "data/verified")
if (Test-Path "data/broker_audit/fyers") { $auditArgs += @("--broker", "FYERS=data/broker_audit/fyers") }
if (Test-Path "data/broker_audit/kite") { $auditArgs += @("--broker", "Kite=data/broker_audit/kite") }
& $audit @auditArgs
if ($LASTEXITCODE -ne 0) { throw "altair_data_audit failed (exit $LASTEXITCODE)" }
Write-Host ""
Write-Host "Report: data\verified\data_audit.xlsx   Merged series: data\verified\merged\"
Write-Host "Fetch log: $log"
if (-not $NoOpen) { Invoke-Item "data/verified/data_audit.xlsx" }
