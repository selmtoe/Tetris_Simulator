# Run every downloaded Lab video through the deployed engine (12 matches each).
$ErrorActionPreference = 'Continue'
$ids = @(
  'toN1cDiaH58','4Qu6MEQegeg','zebCVQwyhdk','dXk_Z8lx0mc','5dPPsUOrcpE','IWskrghK-gc',
  'r8NZlHTZ0-Q','6UGCAPzd50M','pSMZ_7Avwxo','-FwFgRJbw0Q','yeiPMErPCjo','zFXxWHx0yWo',
  'roDwsl-ppJo','uRZnHoDG508','bTL5xEH5OJA','J-upNUb6kwE','FMhM5akSQWk'
)
foreach ($id in $ids) {
    $existing = Join-Path 'scratch\labsite\batches' "$id\quality-warnings.json"
    if (Test-Path $existing) { "skip $id"; continue }
    "=== $id $(Get-Date -Format 'HH:mm:ss')"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\labsite_analyze.ps1 -Id $id -Limit 12
}
'labsite-run-all-finished'
