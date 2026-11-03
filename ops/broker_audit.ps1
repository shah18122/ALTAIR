<#
.SYNOPSIS
  Cross-check dataset/ against FYERS, then write the audit workbook.

.DESCRIPTION
  1. Fetches FYERS candles with the existing READ-ONLY helper
       altair_fyers_history  -> data\broker_audit\fyers\<segment>\<instrument>\<tf>\<year>.csv
     one year per call, so a year the broker has no history for is logged and
     skipped instead of stopping the run. Already-fetched years are kept
     (the run resumes); -Force fetches them again.
  2. Runs altair_data_audit over dataset\ with the FYERS tree and writes
       data\verified\data_audit.xlsx   (Summary + one sheet per instrument)
       data\verified\merged\*.csv      (one clean file per instrument per timeframe)
     and opens the workbook.

  Needs a linked FYERS session (data\fyers_session.json). Nothing here can
  place an order: the helper only calls the historical-candle GET endpoint. data\broker_audit\ and
  data\verified\ are git-ignored.

  Futures: FYERS serves a continuous near-month series at every resolution
  (cont_flag=1).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ops\broker_audit.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ops\broker_audit.ps1 -From 2024-01-01
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ops\broker_audit.ps1 -AuditOnly
  (re-run only the comparison over what is already fetched)
#>
param(
    [string]$Build = "build/net",
    [string]$From = "2015-01-01",
    [string]$To = (Get-Date).ToString("yyyy-MM-dd"),
    [switch]$SkipFyers,
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

# Windows start on the first of a month.
$fromDate = [datetime]::ParseExact($From, "yyyy-MM-dd", $null)
$fromDate = [datetime]::new($fromDate.Year, $fromDate.Month, 1)
$toDate = [datetime]::ParseExact($To, "yyyy-MM-dd", $null)
$years = for ($y = $fromDate.Year; $y -le $toDate.Year; $y++) {
    $a = [datetime]::new($y, 1, 1); if ($a -lt $fromDate) { $a = $fromDate }
    $b = [datetime]::new($y, 12, 31); if ($b -gt $toDate) { $b = $toDate }
    [pscustomobject]@{ Year = $y; From = $a.ToString("yyyy-MM-dd"); To = $b.ToString("yyyy-MM-dd") }
}

# Series in dataset\ and their FYERS symbols (NIFTY 50, NIFTY BANK, INDIA VIX).
$indices = @(
    @{ Seg = "spot"; Inst = "nifty";     Fyers = "NSE:NIFTY50-INDEX" },
    @{ Seg = "spot"; Inst = "banknifty"; Fyers = "NSE:NIFTYBANK-INDEX" },
    @{ Seg = "spot"; Inst = "indiavix";  Fyers = "NSE:INDIAVIX-INDEX" }
)
$timeframes = @(
    @{ Name = "1m";  Fyers = "1" },
    @{ Name = "5m";  Fyers = "5" },
    @{ Name = "15m"; Fyers = "15" },
    @{ Name = "60m"; Fyers = "60" },
    @{ Name = "1d";  Fyers = "D" }
)

# A NIFTY future from data\instruments.csv (built from the FYERS masters by
# altair_instrument_master). With --continuous FYERS returns the near-month
# series whichever contract is named.
$future = $null
if (Test-Path "data/instruments.csv") {
    $row = Select-String -Path "data/instruments.csv" -Pattern ',NIFTY\d{2}[A-Z]{3}FUT,' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($row) { $future = $row.Line.Split(',')[2] }
}
if (-not $future) {
    Write-Log "no NIFTY future in data\instruments.csv; futures skipped (build it: altair_instrument_master --go)"
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

if (-not $AuditOnly) {
    if (-not $SkipFyers) {
        $script:fyersExe = Find-Helper "altair_fyers_history"
        foreach ($s in $indices) { foreach ($tf in $timeframes) { Get-Fyers $s.Seg $s.Inst $s.Fyers $tf $false } }
        if ($future) {
            foreach ($tf in $timeframes) { Get-Fyers "fut" "nifty" ("NSE:" + $future) $tf $true }
        }
    }
}

$audit = Find-Helper "altair_data_audit"
# Forward slashes: std::filesystem accepts them on Windows too.
$auditArgs = @("--dataset", "dataset", "--out", "data/verified")
if (Test-Path "data/broker_audit/fyers") { $auditArgs += @("--broker", "FYERS=data/broker_audit/fyers") }
& $audit @auditArgs
if ($LASTEXITCODE -ne 0) { throw "altair_data_audit failed (exit $LASTEXITCODE)" }
Write-Host ""
Write-Host "Report: data\verified\data_audit.xlsx   Merged series: data\verified\merged\"
Write-Host "Fetch log: $log"
if (-not $NoOpen) { Invoke-Item "data/verified/data_audit.xlsx" }
