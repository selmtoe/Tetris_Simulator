# Prints the port PPT Lab should use (called by launch.cmd).
# 1) a port where PPT Lab is already running, else
# 2) the first free port among the preferred port and the spares.
# A port held by another program (for example another local web server) is skipped.
param([int]$Port = 8765)

$candidates = @($Port, 8790, 8791, 8792) | Select-Object -Unique
foreach ($candidate in $candidates) {
    try {
        $probe = Invoke-RestMethod "http://127.0.0.1:$candidate/api/health" -TimeoutSec 2
        if ($probe.ok -eq $true -and $probe.version) { Write-Output $candidate; exit 0 }
    } catch { }
}
foreach ($candidate in $candidates) {
    $busy = Get-NetTCPConnection -LocalPort $candidate -State Listen -ErrorAction SilentlyContinue
    if (-not $busy) { Write-Output $candidate; exit 0 }
}
Write-Output $Port
