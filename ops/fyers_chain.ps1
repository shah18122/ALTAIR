# ops/fyers_chain.ps1 -- save the day's NIFTY / BANKNIFTY option chains from FYERS.
#
# FYERS history refuses expired options and futures, so a chain not saved
# before its expiry is gone for good. altair_fyers_chain saves every listed
# future and the calls and puts of the nearest expiries (NIFTY: three nearest
# plus the monthly; BANKNIFTY: two monthlies), ATM +/- -Strikes, as one-minute
# bars and a close per day (app/fyers_chain.hpp), into
#   data/store/fyers_chain/<day>/<UNDERLYING>.csv and <UNDERLYING>_1m.csv
# It fetches the last -Days days, so a missed day is filled while its
# contracts still trade; a day already saved is kept.
#
#   ALTAIR FYERS Chain   Mon-Fri 15:45   (ops\install_tasks.ps1 registers it)
#
# Needs today's FYERS login. Without one it logs and exits 0, so the
# scheduled task does not show as failed for a missed login.
#
#   powershell -ExecutionPolicy Bypass -File ops\fyers_chain.ps1        # dry run: the contracts it would save
#   powershell -ExecutionPolicy Bypass -File ops\fyers_chain.ps1 -Go

param(
    [switch]$Go,
    [int]$Days = 7,
    [int]$Strikes = 40,
    [string]$Bin = 'build\net\app'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
. "$PSScriptRoot\altair_log.ps1"

$exe = Join-Path $Bin 'altair_fyers_chain.exe'
if (-not (Test-Path $exe)) { Write-AltairLog chain ERROR "no $exe (build.bat net first)"; exit 2 }
$ist = Get-AltairIstNow
$to = $ist.ToString('yyyy-MM-dd')
$from = $ist.AddDays(1 - $Days).ToString('yyyy-MM-dd')
$a = @('--from', $from, '--to', $to, '--root', (Get-Location).Path, '--strikes', "$Strikes")
if ($Go) { $a += '--go' }

$out = & $exe @a 2>&1 | Out-String
$code = $LASTEXITCODE
Write-Host $out
switch ($code) {
    0 { Write-AltairLog chain INFO "FYERS chains $from to $to saved" }
    3 { Write-AltairLog chain WARN "FYERS chains $from to ${to}: skipped, the FYERS login has expired (log in, then rerun)"; exit 0 }
    1 { Write-AltairLog chain WARN "FYERS chains $from to ${to}: nothing saved (no bars from FYERS, or every day already saved)" }
    default { Write-AltairLog chain ERROR "FYERS chains $from to ${to}: altair_fyers_chain exit $code" }
}
exit $code
