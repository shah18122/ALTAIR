# ops/nightly_series.ps1 -- the FYERS price series ALTAIR keeps, shared by
# ops\nightly.ps1 (each new day) and ops\fetch_fyers_history.ps1 (everything before).

function Get-NearFuture([string]$Under, [datetime]$Date) {
    $day = $Date.ToString('yyyy-MM-dd')
    $u = 'data\live\universe.csv'
    if (Test-Path $u) {
        $f = Import-Csv $u | Where-Object { $_.kind -eq 'future' -and $_.underlying -eq $Under -and $_.fyers -like 'NSE:*' -and $_.expiry -ge $day } |
            Sort-Object expiry | Select-Object -First 1
        if ($f) { return $f.fyers }
    }
    # The monthly expiry is the last Tuesday; the day after it, the next month is near.
    $last = (Get-Date -Year $Date.Year -Month $Date.Month -Day 1).AddMonths(1).AddDays(-1)
    while ($last.DayOfWeek -ne 'Tuesday') { $last = $last.AddDays(-1) }
    $m = if ($Date.Date -gt $last.Date) { $Date.AddMonths(1) } else { $Date }
    return "NSE:$Under" + $m.ToString('yy') + $m.ToString('MMM', [Globalization.CultureInfo]::InvariantCulture).ToUpper() + 'FUT'
}

function Get-AltairSeries([datetime]$Date) {
    @(
        @{ key = 'spot\nifty'; sym = 'NSE:NIFTY50-INDEX'; cont = $false },
        @{ key = 'spot\banknifty'; sym = 'NSE:NIFTYBANK-INDEX'; cont = $false },
        @{ key = 'spot\indiavix'; sym = 'NSE:INDIAVIX-INDEX'; cont = $false },
        @{ key = 'fut\nifty'; sym = (Get-NearFuture 'NIFTY' $Date); cont = $true },
        @{ key = 'fut\banknifty'; sym = (Get-NearFuture 'BANKNIFTY' $Date); cont = $true }
    )
}

function Get-AltairTimeframes {
    @(@{ res = '1'; dir = '1m' }, @{ res = '5'; dir = '5m' }, @{ res = '15'; dir = '15m' }, @{ res = '60'; dir = '60m' }, @{ res = 'D'; dir = '1d' })
}
