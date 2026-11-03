# ops/fyers_login.ps1 -- today's FYERS login, with no browser (TOTP + PIN).
#
# The "ALTAIR FYERS Login" scheduled task (ops\install_tasks.ps1) runs this at
# 08:30 IST, Monday to Friday, before the recorder (08:50) and the market open.
# It runs broker\tools\fyers_auto_login.py, which writes data\fyers_session.json
# exactly as the browser login does; everything else picks the token up.
# It does nothing when today's session already works.
#
# One-time setup (values are typed hidden into Windows Credential Manager):
#   py -3 broker\tools\fyers_auto_login.py --store
#   py -3 broker\tools\fyers_auto_login.py --check     # what is found, never the values
#
#   powershell -ExecutionPolicy Bypass -File ops\fyers_login.ps1          # dry run
#   powershell -ExecutionPolicy Bypass -File ops\fyers_login.ps1 -Go
#   powershell -ExecutionPolicy Bypass -File ops\fyers_login.ps1 -Go -Force

param(
    [switch]$Go,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
. "$PSScriptRoot\altair_log.ps1"

$script = Join-Path $root 'broker\tools\fyers_auto_login.py'
$python = $null
foreach ($cand in @(@('py', '-3'), @('python'), @('python3'))) {
    if (Get-Command $cand[0] -ErrorAction SilentlyContinue) { $python = $cand; break }
}
if (-not $python) {
    Write-AltairLog fyers-login ERROR 'no Python 3 found (install it from python.org, tick "Add to PATH")'
    exit 2
}
$pyArgs = @()
if ($python.Count -gt 1) { $pyArgs += $python[1..($python.Count - 1)] }
$pyArgs += @($script, '--root', $root)
if ($Force) { $pyArgs += '--force' }

if (-not $Go) {
    Write-Host "DRY RUN. With -Go this runs:"
    Write-Host "  $($python[0]) $($pyArgs -join ' ')"
    exit 0
}
# The script writes its own activity-log lines; this only adds the exit code.
& $python[0] @pyArgs
$code = $LASTEXITCODE
if ($code -ne 0) { Write-AltairLog fyers-login ERROR "auto-login exited with code $code (2 credentials, 3 refused, 4 network, 5 write)" }
exit $code
