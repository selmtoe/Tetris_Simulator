# Analyze the 720p downloads so the 360p vs 720p warning rates can be compared.
$ErrorActionPreference = 'Continue'
$exe = 'tools\video-recovery-runtime\bin\TetrisVideoRecovery_stream.exe'
$env:TR_NVDEC_QUEUE_FRAME_STRIDE = '1'
foreach ($id in @('IWskrghK-gc', '4Qu6MEQegeg')) {
    $video = "scratch\labsite\media720\$id.mp4"
    $out = "scratch\labsite\batches720\$id"
    New-Item -ItemType Directory -Force $out | Out-Null
    "=== $id $(Get-Date -Format 'HH:mm:ss')"
    $started = Get-Date
    & $exe $video --detect $out 'tools\video-recovery-runtime\models\scene-classifier.onnx' `
        'tools\video-recovery-runtime\models\tetris.onnx' 'tools\video-recovery-runtime\config\tetris_recover.ini' `
        12 *> "scratch\labsite\logs\analysis720-$id.log"
    "$id elapsed=$([int]((Get-Date) - $started).TotalSeconds)s"
}
'analysis720-finished'
