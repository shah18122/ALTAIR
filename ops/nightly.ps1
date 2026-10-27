# ops/nightly.ps1 -- store the day's recordings, sync every input to one cutoff, retrain.
#
# The "ALTAIR Nightly" scheduled task (ops\install_tasks.ps1) runs this at
# 20:00 IST, Monday to Friday. Each step says what it did in the activity log
# (data\logs\altair-<day>.log); a step that fails is logged and the rest go on,
# except that a model run is never promoted unless its leak check passed.
#
#   1. DECODE   data\store\tapes\<day>\  ->  data\store\ticks\<day>\ (1-minute bars of
#               every instrument, the decode report; -Ticks adds every trade, quote
#               and book) and data\store\tape_bars\ (indices, near futures).
#   2. IV       altair_iv_record over the day's tapes -> data\live\iv\ (ATM IV a minute).
#   3. FYERS    the official candles (1, 5, 15, 60 minutes and daily) for NIFTY,
#               BANKNIFTY, INDIA VIX and the NIFTY and BANKNIFTY continuous futures,
#               the day and any of the last -BackfillDays it is missing, into
#               data\store\fyers\ (laid out like dataset\). Needs today's FYERS login;
#               without it this step is skipped and logged.
#   4. SNAPSHOT altair_store snapshot --cutoff <day>: FYERS candles, then the tape
#               bars (fill gaps only); recorded IV cut at the same day. FYERS data
#               only by default: -WithDataset puts dataset\ under FYERS (FYERS wins
#               beyond 1 bp, every such bar listed); -WithBhavcopy adds the NSE bhavcopy.
#               data\store\snapshots\<day>\ and its _snapshot.json (id, rule, inputs).
#   5. RETRAIN  altair_forecast_curriculum on that snapshot only, with --leak-check,
#               into data\store\runs\<day>\. Promoted to data\store\runs\LATEST.txt
#               only when it exits 0 (3 = something leaked: not promoted).
#   6. TRADES   every round trip of the day (paper, and real if any) logged with its
#               reasons, from data\live\paper\trades.csv and data\live_orders\.
#   7. TIDY     snapshots beyond the last -KeepSnapshots removed (runs and tapes kept),
#               logs beyond 30 days removed, free disk reported.
#   8. PUSH     only when config\data_push.local.toml names a data repo (ops\push_data.ps1).
#
#   powershell -ExecutionPolicy Bypass -File ops\nightly.ps1 -Go [-Day 2026-10-26] [-Only daily] [-Ticks]

param(
    [switch]$Go,
    [string]$Day = '',
    [string]$Only = '',
    [switch]$Ticks,
    [switch]$NoRetrain,
    [int]$BackfillDays = 7,
    [int]$KeepSnapshots = 10,
    [switch]$WithDataset,
    [switch]$WithBhavcopy,
    [string]$Bin = 'build\net\app'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
. "$PSScriptRoot\altair_log.ps1"

$ist = Get-AltairIstNow
if ($Day -eq '') { $Day = $ist.ToString('yyyy-MM-dd') }
$date = [datetime]::ParseExact($Day, 'yyyy-MM-dd', [Globalization.CultureInfo]::InvariantCulture)
$store = 'data\store'
$exe = @{
    store = Join-Path $Bin 'altair_store.exe'
    iv = Join-Path $Bin 'altair_iv_record.exe'
    history = Join-Path $Bin 'altair_fyers_history.exe'
    curriculum = Join-Path $Bin 'altair_forecast_curriculum.exe'
}
if (-not $Go) {
    Write-Host "DRY RUN for $Day. With -Go: decode, IV, FYERS candles, snapshot, retrain (leak-checked), trades, tidy, push."
    foreach ($k in $exe.Keys) { Write-Host ("  {0,-11} {1} {2}" -f $k, $exe[$k], $(if (Test-Path $exe[$k]) { '' } else { '(NOT BUILT)' })) }
    exit 0
}
foreach ($k in $exe.Keys) {
    if (-not (Test-Path $exe[$k])) { Write-AltairLog nightly ERROR "no $($exe[$k]) -- build it first: .\build.bat net"; exit 1 }
}
Write-AltairLog nightly INFO "nightly $Day started"
$status = [ordered]@{}

# Run a tool, its output to data\logs\nightly-<day>-<step>.txt; returns the exit code.
function Invoke-Step([string]$Step, [string]$Exe, [string[]]$Arguments) {
    $out = "data\logs\nightly-$Day-$Step.txt"
    $p = Start-Process -FilePath $Exe -ArgumentList $Arguments -NoNewWindow -PassThru -Wait `
        -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    return $p.ExitCode
}

# ---- 1. decode -------------------------------------------------------------------
$tapes = Join-Path $store "tapes\$Day"
if (Test-Path $tapes) {
    $a = @('decode', '--day', $Day)
    if ($Ticks) { $a += '--ticks' }
    $status.decode = Invoke-Step decode $exe.store $a
    # 2. IV from the same tapes
    $status.iv = Invoke-Step iv $exe.iv @('--tapes', $tapes, '--out', 'data\live\iv')
    Write-AltairLog nightly $(if ($status.iv -eq 0) { 'INFO' } else { 'WARN' }) "ATM IV from the day's tapes: exit $($status.iv)"
} else {
    Write-AltairLog nightly WARN "no tapes for $Day (holiday, or the recorder did not run): decode and IV skipped"
}

# ---- 3. FYERS candles ------------------------------------------------------------
. "$PSScriptRoot\nightly_series.ps1"
$series = Get-AltairSeries $date
$tfs = Get-AltairTimeframes
$fetched = 0; $failed = 0; $refused = $false
for ($k = $BackfillDays; $k -ge 0 -and -not $refused; $k--) {
    $d = $date.AddDays(-$k)
    if ($d.DayOfWeek -eq 'Saturday' -or $d.DayOfWeek -eq 'Sunday') { continue }
    $ds = $d.ToString('yyyy-MM-dd')
    foreach ($s in $series) {
        foreach ($tf in $tfs) {
            $out = Join-Path $store ("fyers\{0}\{1}\{2}.csv" -f $s.key, $tf.dir, $ds)
            if ((Test-Path $out) -and $k -gt 0) { continue }   # an older day already fetched stays
            New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
            $a = @('--symbol', $s.sym, '--resolution', $tf.res, '--from', $ds, '--to', $ds, '--out', $out, '--force', '--go')
            if ($s.cont) { $a += '--continuous' }
            $code = Invoke-Step fyers $exe.history $a
            if ($code -eq 0) { $fetched++ } else {
                $failed++
                $why = (Get-Content "data\logs\nightly-$Day-fyers.txt" -Tail 1 -ErrorAction SilentlyContinue)
                if ($why -match 'session|authenticate|-16|HTTP 401|HTTP 403') { $refused = $true; break }
            }
            Start-Sleep -Milliseconds 350
        }
        if ($refused) { break }
    }
}
if ($refused) {
    Write-AltairLog nightly WARN "FYERS candles skipped: no valid FYERS session today (log in on Brokers). The snapshot uses the FYERS candles already on disk and the tapes."
} else {
    Write-AltairLog nightly $(if ($failed -eq 0) { 'INFO' } else { 'WARN' }) "FYERS candles: $fetched file(s) fetched, $failed failed or empty (holidays answer empty)"
}

# ---- 4. snapshot -----------------------------------------------------------------
$snap = Join-Path $store "snapshots\$Day"
if (Test-Path $snap) {
    Write-AltairLog nightly INFO "snapshot $Day already exists: kept (snapshots are never rebuilt over)"
    $status.snapshot = 0
} else {
    # FYERS data only, as Smit asked: dataset\ (other vendors) and the NSE
    # bhavcopy join only with -WithDataset / -WithBhavcopy.
    $a = @('snapshot', '--cutoff', $Day)
    if ($WithDataset) { $a += @('--source', 'dataset=dataset') }
    $a += @('--source', "fyers=$store\fyers", '--fill', "tape=$store\tape_bars", '--iv', 'data\live\iv')
    if ($WithBhavcopy) { $a += @('--bhavcopy', 'data\bhavcopy') }
    $status.snapshot = Invoke-Step snapshot $exe.store $a
}

# ---- 5. retrain ------------------------------------------------------------------
if ($NoRetrain) {
    Write-AltairLog nightly INFO 'retrain skipped (-NoRetrain)'
} elseif ($status.snapshot -ne 0 -or -not (Test-Path (Join-Path $snap '_snapshot.json'))) {
    Write-AltairLog nightly ERROR 'retrain skipped: no snapshot to train on'
} else {
    $snapId = (Get-Content (Join-Path $snap '_snapshot.json') -Raw | ConvertFrom-Json).id
    $run = Join-Path $store "runs\$Day"
    New-Item -ItemType Directory -Force -Path $run | Out-Null
    $a = @('--dataset', $snap, '--out', $run, '--recorded-iv', (Join-Path $snap '_inputs\iv'), '--leak-check')
    if ($WithBhavcopy) { $a += @('--bhavcopy', (Join-Path $snap '_inputs\bhavcopy')) }
    if ($Only -ne '') { $a += @('--only', $Only) }
    Write-AltairLog nightly INFO "retrain on snapshot $snapId (cutoff $Day): altair_forecast_curriculum $($a -join ' ')"
    $t0 = Get-Date
    $status.retrain = Invoke-Step retrain $exe.curriculum $a
    $mins = [int]((Get-Date) - $t0).TotalMinutes
    $rev = (git rev-parse --short=12 HEAD 2>$null)
    [ordered]@{ day = $Day; snapshot_id = $snapId; snapshot = $snap; exit = $status.retrain; minutes = $mins; code = $rev; args = $a } |
        ConvertTo-Json | Set-Content (Join-Path $run 'run.json') -Encoding UTF8
    if ($status.retrain -eq 0) {
        "$Day $snapId $run" | Set-Content (Join-Path $store 'runs\LATEST.txt') -Encoding ASCII
        Write-AltairLog nightly INFO "retrain done in $mins min, leak check clean: promoted (runs\LATEST.txt)"
    } elseif ($status.retrain -eq 3) {
        Write-AltairLog nightly ERROR "retrain: THE LEAK CHECK FAILED (see $run\leak_check.csv). Not promoted."
    } else {
        Write-AltairLog nightly ERROR "retrain exited $($status.retrain) after $mins min. Not promoted."
    }
}

# ---- 6. the day's trades, with their reasons --------------------------------------
foreach ($t in @(@{ file = 'data\live\paper\trades.csv'; kind = 'paper' }, @{ file = 'data\live_orders\strategy_trades.csv'; kind = 'LIVE' })) {
    if (-not (Test-Path $t.file)) { continue }
    $rows = @(Import-Csv $t.file | Where-Object { $_.date -eq $Day })
    $net = 0.0
    foreach ($r in $rows) {
        if ($r.net -ne '') { $net += [double]$r.net }
        Write-AltairLog trades INFO ("{0} {1} {2} {3} x{4} {5}->{6} net {7}; in: {8}; out: {9}" -f $t.kind, $r.model, $r.side, $r.symbol,
            $r.qty, $r.entry, $r.exit, $r.net, $r.why_in, $r.why_out)
    }
    Write-AltairLog trades INFO ("{0}: {1} round trip(s) on {2}, net {3:N0}" -f $t.kind, $rows.Count, $Day, $net)
}

# ---- 7. tidy ---------------------------------------------------------------------
$snaps = @(Get-ChildItem (Join-Path $store 'snapshots') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^\d{4}-\d\d-\d\d$' } | Sort-Object Name)
if ($snaps.Count -gt $KeepSnapshots) {
    $snaps | Select-Object -First ($snaps.Count - $KeepSnapshots) | ForEach-Object {
        Remove-Item $_.FullName -Recurse -Force
        Write-AltairLog nightly INFO "old snapshot removed: $($_.Name) (its run in runs\ is kept)"
    }
}
Remove-OldAltairLogs 30
$drive = (Get-Item .).PSDrive
$freeGb = [math]::Round($drive.Free / 1GB, 1)
$tapeGb = [math]::Round(((Get-ChildItem (Join-Path $store 'tapes') -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum) / 1GB, 1)
Write-AltairLog nightly $(if ($freeGb -lt 20) { 'WARN' } else { 'INFO' }) "disk: $freeGb GB free on $($drive.Name):; tapes hold $tapeGb GB (kept: they are the only tick history)"

# ---- 8. push ---------------------------------------------------------------------
if (Test-Path 'config\data_push.local.toml') {
    & "$PSScriptRoot\push_data.ps1" -Day $Day -Go
} else {
    Write-AltairLog nightly INFO 'push skipped: no config\data_push.local.toml (see ops\recorder.md)'
}

Write-AltairLog nightly INFO ("nightly $Day finished: " + (($status.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', '))
