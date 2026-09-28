param(
    [Parameter(Mandatory = $true)][string]$Source,
    [int]$Start = 1,
    [int]$MaxLines = 400,
    [int]$MaxChars = 24000
)
$ErrorActionPreference = 'Stop'
$auditRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$auditManifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'CX01_MANIFEST.json') -Raw | ConvertFrom-Json
$auditKey = $Source.Replace('\', '/')
$auditEntry = @($auditManifest.files | Where-Object { $_.path -eq $auditKey })
if ($auditEntry.Count -ne 1) { throw 'Source must be an exact frozen-manifest path.' }
$auditPath = Join-Path $auditRoot $auditKey
$auditHash = (Get-FileHash -LiteralPath $auditPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($auditHash -ne $auditEntry[0].sha256) { throw "Source changed since inventory: $auditKey" }
$auditLines = [System.IO.File]::ReadAllLines($auditPath)
if ($Start -lt 1 -or $MaxLines -lt 1 -or $MaxChars -lt 1000) { throw 'Invalid slice bounds.' }
$auditText = [System.Text.StringBuilder]::new()
$auditEnd = $Start - 1
for ($auditIndex = $Start - 1; $auditIndex -lt $auditLines.Length -and $auditIndex -lt $Start - 1 + $MaxLines; ++$auditIndex) {
    $auditLine = '{0}: {1}' -f ($auditIndex + 1), $auditLines[$auditIndex]
    if ($auditText.Length -gt 0 -and $auditText.Length + $auditLine.Length -gt $MaxChars) { break }
    [void]$auditText.AppendLine($auditLine)
    $auditEnd = $auditIndex + 1
}
Write-Output "SOURCE=$auditKey SHA256=$auditHash TOTAL=$($auditLines.Length) RANGE=$Start-$auditEnd"
Write-Output $auditText.ToString()
Write-Output "END_SOURCE=$auditKey NEXT=$($auditEnd + 1) EOF=$($auditEnd -ge $auditLines.Length)"
# Deliberately does not record any reading/review claim. A reviewer records only
# fully received, inspected ranges separately using apply_patch.
