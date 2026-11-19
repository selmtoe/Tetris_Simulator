# 凍結: v77-queue-move-window-b

凍結日時: 2026-11-17T11:53:42+09:00
エンジン SHA-256: `2b1a04aca1cb7a0a0af28d59c086c4bcbab15391e45be192f116b48f98f26e1b`
実行時の git コミット: `7cafc69db0d4e857be4f0f763353119cbc05b424`

この解析で **実際に使った** エンジン・実行環境・モデル・設定・ソース・手順の一式。
最初に `python verify.py` で全ファイルの SHA-256 を照合すること。

## 中身

| フォルダ | 内容 |
|---|---|
| engine/ | 実行ファイル |
| runtime/ | bin の DLL 一式（CUDA/cuDNN/onnxruntime/ffmpeg）。engine と同じフォルダに置いて使う |
| models/next | NEXT/HOLD モデル（この実行で使ったもの） |
| models/scene-main56 | main56（vsたわ 56試合）で使ったシーンモデル |
| models/scene-suite-* | Lab 動画のスイートで使ったシーンモデル（suites.json の指定） |
| models/board | 盤面モデル |
| models/run | そのまま実行できる組（tetris.onnx と、この実行の next_hold.onnx） |
| models/ppt-lab | PPT Lab アプリのモデル（勝敗判定 end3 / 解析 fast320）の作業コピー |
| config/ | tetris_recover.ini |
| source/ | エンジンのソース（実行時コミット）と PPT Lab のソース（凍結時 HEAD） |
| tools/ | 回帰テストと解析パイプラインのスクリプト、suites.json |
| results/ | この実行の golden トレースと軽量結果、REGRESSION-LOG の行 |

## 再実行（GPU、1本ずつ）

```powershell
$env:TR_NVDEC_QUEUE_FRAME_STRIDE = '1'
$env:TR_BATCH_WORKERS = '2'
$env:TR_BATCH_RAM_RESERVE_MB = '512'
$env:TR_OPENING_PIECE_FLEXIBLE = '1'
$env:TR_OPENING_RESOLVE = '1'
# runtime の DLL を engine と同じフォルダにそろえてから:
Copy-Item runtime\*.dll engine\
& engine\TetrisVideoRecovery_stream.exe <動画> --detect <出力先> `
  models\scene-main56\<ファイル名> models\run\tetris.onnx config\tetris_recover.ini
```

エンジンは盤面モデル（tetris.onnx）と同じフォルダの `next_hold.onnx` を NEXT モデルとして読む。
`models\run\` にはこの実行で使った2つを並べてある（regress.py の --next-model と同じ扱い）。
Lab 動画のスイートは、シーンモデルを `models\scene-suite-*` に替えて実行する（suites.json の指定）。

リポジトリ内で回帰テストとして再実行する場合:

```powershell
python tools\regress.py run --label recheck-v77-queue-move-window-b --skip-build --no-deploy `
  --exe <このフォルダ>\engine\TetrisVideoRecovery_stream.exe `
  --next-model <このフォルダ>\models\next\next_hold.onnx `
  --scene-model <このフォルダ>\models\scene-main56\scene-classifier.onnx `
  --env TR_BATCH_WORKERS=2 --env TR_BATCH_RAM_RESERVE_MB=512 --env TR_OPENING_PIECE_FLEXIBLE=1 --env TR_OPENING_RESOLVE=1 `
  --baseline v77-queue-move-window-b --suites main56
```

## 注意

- なし
