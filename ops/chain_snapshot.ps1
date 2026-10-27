# ops/chain_snapshot.ps1 -- a REST snapshot of the whole NIFTY and BANKNIFTY option chains.
#
# FYERS history refuses expired options and futures, so the only option data
# ALTAIR will ever have is what it records itself. The tape recorder
# (record_session.ps1) records every tick, but only while it runs. This is
# the cheap second line: two scheduled snapshots a day, independent of the
# recorder, the desktop and the socket:
#
#   ALTAIR Chain Open    Mon-Fri 09:20   (the intraday weekly test's entry)
#   ALTAIR Chain Close   Mon-Fri 15:27   (end of day)
#
# Each one quotes, through FYERS /data/quotes (read-only, 50 symbols a call):
#   NIFTY 50, NIFTY BANK, INDIA VIX; the NIFTY and BANKNIFTY futures;
#   the nearest -Expiries option expiries of each index plus the nearest
#   monthly (NIFTY: the weeklies and the monthly; BANKNIFTY: the monthlies),
#   ATM +/- -Strikes, calls and puts.
# The chain comes from altair_price_service's own universe builder (dry run:
# no socket), centred on the index levels just quoted.
#
# WRITES (data/store/ is git-ignored):
#   data/store/chain_snap/<day>/<HHMM>.csv
#       snap_time,fyers,underlying,kind,expiry,strike,ltp,bid,ask,volume,
#       open,high,low,prev_close,exch_time
#   data/store/chain_snap/<day>/<HHMM>-<n>.json   the FYERS responses as received
# The nightly push sends the CSVs (gzipped) to the private data repo.
#
# Needs today's FYERS login, like the recorder. Without one it logs and exits.
#
#   powershell -ExecutionPolicy Bypass -File ops\chain_snapshot.ps1        # dry run: the symbols it would quote
#   powershell -ExecutionPolicy Bypass -File ops\chain_snapshot.ps1 -Go

param(
    [switch]$Go,
    [string]$Tag = '',
    [int]$Strikes = 40,
    [int]$Expiries = 3,
    [string]$Bin = 'build\net\app'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
. "$PSScriptRoot\altair_log.ps1"

$ist = Get-AltairIstNow
$day = $ist.ToString('yyyy-MM-dd')
if (-not $Tag) { $Tag = $ist.ToString('HHmm') }
$quotes = Join-Path $Bin 'altair_fyers_quotes.exe'
$service = Join-Path $Bin 'altair_price_service.exe'
foreach ($exe in @($quotes, $service)) {
    if (-not (Test-Path $exe)) { Write-AltairLog chain ERROR "no $exe (build.bat net first)"; exit 2 }
}
$root = 'data/store/chain_snap'
$uniDir = "$root/_universe"
$dir = "$root/$day"
New-Item -ItemType Directory -Force -Path $uniDir, $dir | Out-Null

# Rupees from a FYERS quotes response, by symbol.
function Read-Quotes([string]$Path) {
    $out = @{}
    if (-not (Test-Path $Path)) { return $out }
    $j = Get-Content $Path -Raw | ConvertFrom-Json
    foreach ($r in $j.responses) {
        if ($null -eq $r.body) { continue }
        foreach ($d in $r.body.d) { if ($d.s -eq 'ok' -and $d.v) { $out[$d.n] = $d.v } }
    }
    return $out
}

# 1. The index levels, to centre the chains on.
$atm = @()
if ($Go) {
    & $quotes --symbols 'NSE:NIFTY50-INDEX,NSE:NIFTYBANK-INDEX' --out "$uniDir/spots.json" --go | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-AltairLog chain WARN "snapshot $Tag skipped: FYERS quotes did not answer (no login today?)"
        exit 1
    }
    $spot = Read-Quotes "$uniDir/spots.json"
    if ($spot['NSE:NIFTY50-INDEX']) { $atm += @('--atm-nifty', "$($spot['NSE:NIFTY50-INDEX'].lp)") }
    if ($spot['NSE:NIFTYBANK-INDEX']) { $atm += @('--atm-banknifty', "$($spot['NSE:NIFTYBANK-INDEX'].lp)") }
}

# 2. The chain: the price service's universe builder, dry (no socket, no --go).
& $service --fyers --live-dir $uniDir --strikes "$Strikes" --depth-strikes 0 --expiries "$Expiries" --monthly --no-stocks @atm | Out-Null
if (-not (Test-Path "$uniDir/universe.csv")) { Write-AltairLog chain ERROR "snapshot ${Tag}: no universe built"; exit 2 }
$rows = Import-Csv "$uniDir/universe.csv" | Where-Object {
    ($_.underlying -in @('NIFTY', 'BANKNIFTY') -and $_.kind -in @('index', 'future', 'call', 'put') -and $_.fyers -like 'NSE:*') -or
    $_.fyers -eq 'NSE:INDIAVIX-INDEX'
}
$meta = @{}
foreach ($r in $rows) { $meta[$r.fyers] = $r }
$symbols = @($meta.Keys | Sort-Object)
$expiryList = ($rows | Where-Object { $_.kind -in @('call', 'put') } | ForEach-Object { "$($_.underlying) $($_.expiry)" } | Sort-Object -Unique) -join ', '
if (-not $Go) {
    Write-Host "DRY RUN: would quote $($symbols.Count) symbols ($expiryList) into $dir/$Tag.csv"
    exit 0
}

# 3. Quote them, 500 a call (the quotes tool's own limit; 50 a request inside it).
$n = 0
for ($at = 0; $at -lt $symbols.Count; $at += 500) {
    $n++
    $list = "$uniDir/symbols-$n.txt"
    $symbols[$at..([Math]::Min($at + 499, $symbols.Count - 1))] | Set-Content $list
    & $quotes --symbols-file $list --out "$dir/$Tag-$n.json" --go | Out-Null
}

# 4. One CSV row a symbol.
$stamp = $ist.ToString('yyyy-MM-ddTHH:mm:ss') + '+05:30'
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('snap_time,fyers,underlying,kind,expiry,strike,ltp,bid,ask,volume,open,high,low,prev_close,exch_time')
$got = 0
for ($k = 1; $k -le $n; $k++) {
    $q = Read-Quotes "$dir/$Tag-$k.json"
    foreach ($sym in $q.Keys) {
        $v = $q[$sym]
        $m = $meta[$sym]
        $tt = ''
        if ($v.tt) { $tt = ([DateTimeOffset]::FromUnixTimeSeconds([long]$v.tt)).ToOffset([TimeSpan]::FromMinutes(330)).ToString('yyyy-MM-ddTHH:mm:ss') + '+05:30' }
        $lines.Add((@($stamp, $sym, $m.underlying, $m.kind, $m.expiry, $m.strike, $v.lp, $v.bid, $v.ask, $v.volume,
                      $v.open_price, $v.high_price, $v.low_price, $v.prev_close_price, $tt) -join ','))
        $got++
    }
}
[System.IO.File]::WriteAllLines((Join-Path (Get-Location) "$dir/$Tag.csv"), $lines)
$level = if ($got -eq 0) { 'ERROR' } elseif ($got -lt $symbols.Count) { 'WARN' } else { 'INFO' }
Write-AltairLog chain $level "snapshot ${Tag}: $got of $($symbols.Count) quoted ($expiryList) -> $dir/$Tag.csv"
if ($got -eq 0) { exit 1 }
exit 0
