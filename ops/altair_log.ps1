# ops/altair_log.ps1 -- the PowerShell side of ALTAIR's one activity log.
#
# Dot-source it:   . "$PSScriptRoot\altair_log.ps1"
# then:            Write-AltairLog nightly INFO "snapshot built"
#
# Same file and the same rules as app/activity_log.hpp:
#   data\logs\altair-YYYY-MM-DD.log (IST day), one line an event,
#   "2026-10-26T20:00:01.123+05:30 [nightly] INFO ...".
# The file is opened with writers denied (FileShare.Read), as the C++ side
# opens it with _SH_DENYWR, so two writers wait for each other rather than
# interleave. Credential-looking values are masked before the line is written.

$script:AltairRoot = Split-Path $PSScriptRoot -Parent

# IST is UTC+05:30 all year (no daylight saving), so no time-zone database is needed.
function Get-AltairIstNow {
    [datetime]::SpecifyKind([datetime]::UtcNow.AddMinutes(330), 'Unspecified')
}

function Hide-AltairSecrets([string]$Text) {
    $t = ($Text -replace "[`r`n]+", ' ')
    # The key, any of ' ":=' after it, then the value up to a space, comma, quote or brace.
    # "Bearer" first, so "Authorization: Bearer X" masks X and not only the word Bearer.
    foreach ($keys in @('bearer', 'access_token|refresh_token|secret|password|pin|authorization|auth_code|api_key')) {
        $t = [regex]::Replace($t, "(?i)\b($keys)\b([\s`"':=]+)(?!<redacted>)([^\s,`"}]+)", '$1$2<redacted>')
    }
    return $t
}

function Write-AltairLog {
    param(
        [Parameter(Mandatory)][string]$Component,
        [Parameter(Mandatory)][ValidateSet('INFO', 'WARN', 'ERROR')][string]$Level,
        [Parameter(Mandatory)][string]$Message
    )
    $ist = Get-AltairIstNow
    $dir = Join-Path $script:AltairRoot 'data\logs'
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $file = Join-Path $dir ('altair-' + $ist.ToString('yyyy-MM-dd') + '.log')
    $line = $ist.ToString('yyyy-MM-ddTHH:mm:ss.fff') + "+05:30 [$Component] $Level " + (Hide-AltairSecrets $Message) + "`n"
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($line)
    for ($i = 0; $i -lt 80; $i++) {
        try {
            $fs = [System.IO.File]::Open($file, 'Append', 'Write', 'Read')
            try { $fs.Write($bytes, 0, $bytes.Length) } finally { $fs.Dispose() }
            Write-Host $line.TrimEnd()
            return
        } catch [System.IO.IOException] {
            Start-Sleep -Milliseconds 25
        }
    }
    Write-Host "activity log busy; line dropped: $line"
}

function Remove-OldAltairLogs([int]$KeepDays = 30) {
    $dir = Join-Path $script:AltairRoot 'data\logs'
    if (-not (Test-Path $dir)) { return }
    $oldest = (Get-AltairIstNow).Date.AddDays(-$KeepDays).ToString('yyyy-MM-dd')
    Get-ChildItem $dir -Filter 'altair-????-??-??.log' | Where-Object { $_.Name.Substring(7, 10) -lt $oldest } |
        Remove-Item -ErrorAction SilentlyContinue
}
