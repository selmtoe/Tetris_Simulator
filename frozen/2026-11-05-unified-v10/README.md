# 凍結スナップショット 2026-11-05（統合モデル v10）

`vsたわ [yD_ZukVrgHY]` を「10クラス統合モデル1本」で解析した時点の一式です。
Lab の解析結果・エンジン・モデル・スクリプト・学習ツールをここに固めています。

## 中身

| パス | 内容 |
|---|---|
| `models/scene-classifier-unified10.onnx` | **本番の10クラス統合モデル**（シーン検出＋終了＋勝敗） |
| `models/scene-classifier-legacy-fast320.onnx` | 旧シーン分類（Tetrace-V1.0-PPT、差し替え前） |
| `models/board-tetris.onnx` | 盤面モデル（従来の320fast、変更なし） |
| `models/next-hold.onnx` | NEXT/HOLDモデル（変更なし） |
| `models/end3-legacy.onnx` | 旧・終了3クラスモデル（現在は未使用） |
| `bin/TetrisVideoRecovery_stream-v10.exe` | 解析エンジン（盤面余白+1.5秒、品質しきい値0.70） |
| `bin/TetrisVideoRecovery_stream-1755.exe` | 差し替え前の本番エンジン（17:55ビルド） |
| `scripts/run-ppt-stream-analysis.ps1` | 解析パイプライン（勝敗は分類ログ方式、end3は既定で未使用） |
| `scripts/ppt-winner-from-runs.py` | 分類ログから勝敗を決める（前方向の結果画面探索入り） |
| `scripts/ppt-preflight.ps1` | 解析前チェック |
| `train/` | データ収集・学習・評価・ビューアのツール一式 |
| `engine-source/` | このエンジンをビルドしたソース（`src` / `include` / `build_cuda.ps1`） |
| `dataset/` | 収集した学習データ（結果画面11,519・シーン45,723・jsonl・ビューア。水増しコピー`aug_*`は除く） |
| `batch/` | この動画の解析結果（56試合、Labが参照する形式） |

アーカイブ: `frozen/2026-11-05-unified-v10.zip`（約9MB、モデル・エンジン・ソース・スクリプト・解析結果）

## ハッシュ（SHA-256）

```
TetrisVideoRecovery_stream-v10.exe  AB9672B0A21891AA17B5C521F46ABCD8A4C98AF738A42194452674C8C90A2737
scene-classifier-unified10.onnx     7A252009342EA660DC0DE24D5B349BDB51D82043172857A058F1BD5083F95762
board-tetris.onnx                   DA60D1C9A32939F0D607A0393DB842B994D801F877C5E24DCFACA5D301D5B7F2
```

## 実測値（`vsたわ [yD_ZukVrgHY]`、56試合）

| 項目 | 旧（15時版・2モデル） | この凍結版（統合1本） |
|---|---|---|
| 検出 | 56 | **56** |
| 品質警告なし | 52 | **54** |
| 要修正 | 0 | **0** |
| 勝敗 | 40番が1p（誤り） | **26 / 30（全問正解、40=2p・17/38=1p）** |
| 終了位置 | 40番が1981秒（画面の途中） | **全試合が結果画面の頭**（17=917.07 / 38=1911.5 / 40=2011.0） |
| モデル数 | 2本 | **1本** |
| 処理時間 | 362秒（6分02秒） | 462.7秒（7分43秒、シーン31.2秒） |

残る品質警告: **14番（P2・0.54）・50番（P2・0.60）** の2件。
旧本番エンジン（17:55）でも同一範囲・同一モデルで同じ比率で失敗することを確認済み。
15:05の旧バッチでは同区間が正常なので、15:05→17:55の盤面復元コードの変更が原因（当時のソースは残っていない）。

## 学習データ
- `dataset/viewer.html` に一覧（結果画面・補正候補・引き分け合成・シーン）。約28,500枚。
- 学習データの作り方は `tools/ppt-train/README.md` を参照。

## 復元手順（この状態に戻す）

1. モデル: `models/scene-classifier-unified10.onnx` → `tools/ppt-lab/models/fast320/scene-classifier.onnx`
2. エンジン: `bin/TetrisVideoRecovery_stream-v10.exe` → `tools/video-recovery-runtime/bin/TetrisVideoRecovery_stream.exe`
3. スクリプト: `scripts/` の2ファイルを `tools/` に戻す
4. Lab: `%LOCALAPPDATA%\PPTLab\pptlab.sqlite3` の `videos.manifest` を
   `...\data\batches\[yD_ZukVrgHY]\batch-results.json` に向け、サーバーを再起動
5. 解析結果: `batch/` を `tools/video-recovery-runtime/data/batches/[yD_ZukVrgHY]/` に置く

## 注意
- `tools/video-recovery-source/` は未コミットの変更を含みます（
  `scene_boundaries.hpp` の黒転ガード、`recovery.cpp` のしきい値0.70と
  `recovery.hpp` の `matchRatioThreshold=.70`、`scene_detector.cpp` の盤面余白）。
- 旧い状態に戻すときは `bin/TetrisVideoRecovery_stream-1755.exe` と
  `models/scene-classifier-legacy-fast320.onnx` を使ってください。
