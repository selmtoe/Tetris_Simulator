# Download every video listed on https://selmtoe.github.io/Lab/ at 360p (video only).
$ErrorActionPreference = 'Continue'
$yt = 'tools\video-recovery-runtime\tools\yt-dlp\yt-dlp.exe'
$mediaDir = 'scratch\labsite\media'
$logDir = 'scratch\labsite\logs'
New-Item -ItemType Directory -Force $mediaDir, $logDir | Out-Null

$ids = @(
  'IWskrghK-gc','uRZnHoDG508','zFXxWHx0yWo','-FwFgRJbw0Q','r8NZlHTZ0-Q',
  'FMhM5akSQWk','yeiPMErPCjo','J-upNUb6kwE','roDwsl-ppJo','toN1cDiaH58',
  'dXk_Z8lx0mc','5dPPsUOrcpE','4Qu6MEQegeg','6UGCAPzd50M','GXr4f0dXrow',
  'zebCVQwyhdk','pSMZ_7Avwxo','bTL5xEH5OJA'
)

foreach ($id in $ids) {
    $target = Join-Path $mediaDir "$id.mp4"
    if ((Test-Path $target) -and ((Get-Item $target).Length -gt 1000000)) {
        "skip $id (exists $([math]::Round((Get-Item $target).Length/1MB,1))MB)"
        continue
    }
    "start $id"
    $log = Join-Path $logDir "$id.log"
    & $yt -f "bv*[height<=360][ext=mp4]/bv*[height<=360]/b[height<=360]" `
        --no-playlist --no-part --retries 3 --no-warnings `
        -o $target "https://www.youtube.com/watch?v=$id" *> $log
    if ((Test-Path $target) -and ((Get-Item $target).Length -gt 1000000)) {
        "done $id $([math]::Round((Get-Item $target).Length/1MB,1))MB"
    } else {
        "FAILED $id"
    }
}
'download-all-finished'
