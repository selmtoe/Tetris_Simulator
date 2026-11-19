# Tetrace-V1.0-PPT で1本の動画を解析する（GPU、1本ずつ）
#   .\run.ps1 -Video <動画> -Out <出力先> [-Runtime <DLL のあるフォルダ>]
param(
  [Parameter(Mandatory)] [string] $Video,
  [Parameter(Mandatory)] [string] $Out,
  [string] $Runtime = (Join-Path $PSScriptRoot '..\..\..\tools\video-recovery-runtime\bin')
)
$ErrorActionPreference = 'Stop'
$env:TR_NVDEC_QUEUE_FRAME_STRIDE = '1'
$env:TR_OPENING_PIECE_FLEXIBLE = '1'
$env:TR_OPENING_RESOLVE = '1'
$env:TR_BATCH_WORKERS = '2'
$env:TR_BATCH_RAM_RESERVE_MB = '2048'
$env:PATH = (Resolve-Path $Runtime).Path + ';' + $env:PATH
$m = Join-Path $PSScriptRoot 'models'
& (Join-Path $PSScriptRoot 'engine\TetrisVideoRecovery_stream.exe') $Video --detect $Out `
  (Join-Path $m 'scene-classifier.onnx') (Join-Path $m 'tetris.onnx') (Join-Path $PSScriptRoot 'engine\tetris_recover.ini')
