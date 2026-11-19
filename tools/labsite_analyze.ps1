# Analyze one downloaded Lab-site video with the deployed engine.
param(
    [Parameter(Mandatory = $true)][string]$Id,
    [int]$Limit = 12
)
$ErrorActionPreference = 'Continue'
$exe = 'tools\video-recovery-runtime\bin\TetrisVideoRecovery_stream.exe'
$video = Join-Path 'scratch\labsite\media' "$Id.mp4"
if (-not (Test-Path $video)) { throw "missing video: $video" }
$outDir = Join-Path 'scratch\labsite\batches' $Id
New-Item -ItemType Directory -Force $outDir, 'scratch\labsite\logs' | Out-Null
$log = "scratch\labsite\logs\analysis-$Id.log"
$env:TR_NVDEC_QUEUE_FRAME_STRIDE = '1'
$started = Get-Date
& $exe $video --detect $outDir 'tools\video-recovery-runtime\models\scene-classifier.onnx' `
    'tools\video-recovery-runtime\models\tetris.onnx' 'tools\video-recovery-runtime\config\tetris_recover.ini' `
    $Limit *> $log
"$Id elapsed=$([int]((Get-Date) - $started).TotalSeconds)s"
$warning = Join-Path $outDir 'quality-warnings.json'
if (Test-Path $warning) { "warnings-file=yes" } else { "warnings-file=no" }
