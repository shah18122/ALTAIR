# ops/push_data.ps1 -- send the day's compact data to a PRIVATE data repo, so it is not lost
# and Claude can read it from the cloud.
#
# ops\nightly.ps1 runs this last, and only when config\data_push.local.toml
# (git-ignored) names the local clone of the data repo:
#
#     repo = 'D:\altair-data'
#
# One-time setup (ops\recorder.md has the steps): create a PRIVATE repo on
# GitHub (e.g. shah18122/altair-data), clone it, and put an empty file named
# .altair-data-repo at its root. That marker is what makes a folder a valid
# target: this script refuses any other folder, and refuses a clone whose
# origin is the public ALTAIR repo.
#
# WHAT GOES: only these, for the one day --
#   bars\<yyyy>\<day>.csv.gz        1-minute bars of every recorded instrument (data\store\ticks\<day>\bars_1m.csv)
#   candles\<seg>\<inst>\<tf>\<day>.csv   the FYERS candles fetched that night
#   iv\<INDEX>\<day>.csv            the ATM IV of each minute
#   vol\<yyyy>\<day>.csv.gz         each minute: spot, India VIX, forward, ATM IV and the chain's
#                                   VIX per expiry, realised vol (data\store\ticks\<day>\vol_1m.csv)
#   chain_iv\<yyyy>\<day>.csv.gz    each minute, every option: price, IV, delta, vega, OI
#   reports\<day>\                  decode report, snapshot manifest + conflicts (gz), run.json,
#                                   forecast_curriculum.txt, leak_check.csv
#   logs\altair-<day>.log           the activity log (credentials already masked)
# WHAT NEVER GOES: tapes (GBs a day; they stay on this PC), anything under
# data\live\ other than the IV, session or credential files of any kind. Every
# file is checked again before the commit, by name and by content.
#
# It pushes with your own git login on this PC (Git Credential Manager); no
# token is stored by ALTAIR.

param(
    [Parameter(Mandatory)][string]$Day,
    [switch]$Go
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
. "$PSScriptRoot\altair_log.ps1"

$cfg = 'config\data_push.local.toml'
if (-not (Test-Path $cfg)) { Write-AltairLog push WARN "no ${cfg}: nothing pushed"; exit 0 }
$m = Select-String -Path $cfg -Pattern "^\s*repo\s*=\s*['`"](.+)['`"]\s*$" | Select-Object -First 1
if (-not $m) { Write-AltairLog push ERROR "$cfg has no repo = '...' line"; exit 2 }
$repo = $m.Matches[0].Groups[1].Value
if (-not (Test-Path (Join-Path $repo '.altair-data-repo'))) {
    Write-AltairLog push ERROR "$repo is not a data repo (no .altair-data-repo marker): nothing pushed"
    exit 2
}
$origin = (git -C $repo remote get-url origin 2>$null)
if ($origin -match '(?i)shah18122/altair(\.git)?$' -or -not $origin) {
    Write-AltairLog push ERROR "refusing to push: $repo's origin is '$origin' (must be the private data repo)"
    exit 2
}

# .NET resolves a relative path against the process's directory, not PowerShell's: make it absolute.
function Get-FullPath([string]$P) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($P) }
function Copy-Gz([string]$From, [string]$To) {
    $From = Get-FullPath $From
    $To = Get-FullPath $To
    New-Item -ItemType Directory -Force -Path (Split-Path $To) | Out-Null
    $in = [System.IO.File]::OpenRead($From)
    try {
        $out = [System.IO.File]::Create($To)
        try {
            $gz = New-Object System.IO.Compression.GZipStream($out, [System.IO.Compression.CompressionLevel]::Optimal)
            try { $in.CopyTo($gz) } finally { $gz.Dispose() }
        } finally { $out.Dispose() }
    } finally { $in.Dispose() }
}
function Copy-Plain([string]$From, [string]$To) {
    New-Item -ItemType Directory -Force -Path (Split-Path $To) | Out-Null
    Copy-Item $From $To -Force
}

$copied = New-Object System.Collections.Generic.List[string]
$yyyy = $Day.Substring(0, 4)
$s = 'data/store'
if (Test-Path "$s/ticks/$Day/bars_1m.csv") { Copy-Gz "$s/ticks/$Day/bars_1m.csv" "$repo/bars/$yyyy/$Day.csv.gz"; $copied.Add("bars/$yyyy/$Day.csv.gz") }
if (Test-Path "$s/ticks/$Day/vol_1m.csv") { Copy-Gz "$s/ticks/$Day/vol_1m.csv" "$repo/vol/$yyyy/$Day.csv.gz"; $copied.Add("vol/$yyyy/$Day.csv.gz") }
if (Test-Path "$s/ticks/$Day/chain_iv_1m.csv") { Copy-Gz "$s/ticks/$Day/chain_iv_1m.csv" "$repo/chain_iv/$yyyy/$Day.csv.gz"; $copied.Add("chain_iv/$yyyy/$Day.csv.gz") }
if (Test-Path "$s/ticks/$Day/_decode.json") { Copy-Plain "$s/ticks/$Day/_decode.json" "$repo/reports/$Day/decode.json"; $copied.Add("reports/$Day/decode.json") }
Get-ChildItem "$s/fyers" -Recurse -File -Filter "$Day.csv" -ErrorAction SilentlyContinue | ForEach-Object {
    $rel = $_.FullName.Substring((Resolve-Path "$s/fyers").Path.Length + 1)
    Copy-Plain $_.FullName "$repo/candles/$rel"; $copied.Add("candles/$rel")
}
Get-ChildItem 'data/live/iv' -Recurse -File -Filter "$Day.csv" -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Plain $_.FullName "$repo/iv/$($_.Directory.Name)/$Day.csv"; $copied.Add("iv/$($_.Directory.Name)/$Day.csv")
}
$snap = "$s/snapshots/$Day"
if (Test-Path "$snap/_snapshot.json") { Copy-Plain "$snap/_snapshot.json" "$repo/reports/$Day/snapshot.json"; $copied.Add("reports/$Day/snapshot.json") }
if (Test-Path "$snap/_conflicts.csv") { Copy-Gz "$snap/_conflicts.csv" "$repo/reports/$Day/conflicts.csv.gz"; $copied.Add("reports/$Day/conflicts.csv.gz") }
foreach ($f in @('run.json', 'forecast_curriculum.txt', 'leak_check.csv')) {
    if (Test-Path "$s/runs/$Day/$f") { Copy-Plain "$s/runs/$Day/$f" "$repo/reports/$Day/$f"; $copied.Add("reports/$Day/$f") }
}
if (Test-Path "data/logs/altair-$Day.log") { Copy-Plain "data/logs/altair-$Day.log" "$repo/logs/altair-$Day.log"; $copied.Add("logs/altair-$Day.log") }

if ($copied.Count -eq 0) { Write-AltairLog push WARN "nothing to push for $Day"; exit 0 }

# The second fence: no credential by name or by content, whatever was copied.
foreach ($rel in $copied) {
    $p = Join-Path $repo $rel
    if ($rel -match '(?i)session|secret|token|credential|\.env|api_key|password') {
        Write-AltairLog push ERROR "refusing to push: $rel has a credential-like name"; exit 3
    }
    if ($rel -notmatch '\.gz$') {
        if (Select-String -Path $p -Pattern '(?i)"?(access_token|refresh_token|client_secret|secret_key)"?\s*[:=]\s*"?[A-Za-z0-9._-]{12,}' -Quiet) {
            Remove-Item $p -Force
            Write-AltairLog push ERROR "refusing to push: $rel contains a credential-like value (file removed from the data repo)"
            exit 3
        }
    }
}

if (-not $Go) { Write-Host "DRY RUN: $($copied.Count) file(s) copied into $repo, nothing committed"; exit 0 }
git -C $repo add -- $copied 2>&1 | Out-Null
git -C $repo diff --cached --quiet
if ($LASTEXITCODE -eq 0) { Write-AltairLog push INFO "data $Day already pushed: nothing new"; exit 0 }
git -C $repo commit -q -m "data $Day" 2>&1 | Out-Null
$ok = $false
foreach ($wait in @(0, 2, 4, 8, 16)) {
    if ($wait -gt 0) { Start-Sleep -Seconds $wait }
    git -C $repo push -q origin HEAD 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { $ok = $true; break }
}
if ($ok) {
    Write-AltairLog push INFO "pushed $($copied.Count) file(s) for $Day to $origin"
} else {
    Write-AltairLog push ERROR "push for $Day failed after 5 tries (committed locally; tomorrow's push carries it)"
    exit 4
}
