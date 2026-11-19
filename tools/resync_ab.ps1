# A/B the visible-board resync lane on the eight warning matches (manifest mode).
$ErrorActionPreference = 'Continue'
$exe = 'tools\video-recovery-runtime\bin\TetrisVideoRecovery_stream.exe'
$video = 'C:\Users\hirom\AppData\Local\PPTLab\media\video-799b6882cb7d40f0\[yD_ZukVrgHY].mkv'
$manifest = 'scratch\resync-test\matches-8warn.json'
$ini = 'tools\video-recovery-runtime\config\tetris_recover.ini'
$board = 'tools\video-recovery-runtime\models\tetris.onnx'
$env:TR_NVDEC_QUEUE_FRAME_STRIDE = '1'

foreach ($case in @('control', 'resync')) {
    $out = "scratch\resync-test\$case"
    New-Item -ItemType Directory -Force $out | Out-Null
    if ($case -eq 'resync') { $env:TR_RESYNC = '1' } else { Remove-Item Env:\TR_RESYNC -ErrorAction SilentlyContinue }
    "=== $case $(Get-Date -Format 'HH:mm:ss')"
    $started = Get-Date
    & $exe $video $manifest $out $board $ini *> "scratch\resync-test\$case.log"
    "$case elapsed=$([int]((Get-Date) - $started).TotalSeconds)s"
}
Remove-Item Env:\TR_RESYNC -ErrorAction SilentlyContinue
'resync-ab-finished'
