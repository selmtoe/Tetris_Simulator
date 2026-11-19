# 状態の復元手順（必読）

「昔うまくいった状態に戻せない」を防ぐための記録と復元方法。
2026-11-10 以降、エンジンのソースは **git 管理下**に入った。

## 0. 現行モデル Tetrace-V1.0-PPT（2026-11-16 以降はまずこれ）

**Tetrace-V1.0-PPT** = エンジン v77（`2b1a04aca1cb`、ソース `7cafc69d`。v76 の開始探しを GO 終わりの前後1秒に固定し、前にもずらせるようにした）＋シーン `55efd493`＋盤面 `da60d1c9`＋NEXT `b32435fa`＋ini `f9edabaf`＋環境変数。
- DLL を除いた単体: `frozen/models/Tetrace-V1.0-PPT/`（1ファイル版 `Tetrace-V1.0-PPT.zip`、実行は `run.ps1`）
- DLL まで含む完全な一式: `frozen/bundles/2026-11-17-v77-queue-move-window-b/`（`python verify.py` で照合）
- 確認: main56 で警告0・検証エラー0（v76 から 1/8210 手が変化）。bTL5 前半72試合は v76 と同一。15先 前半69・最高3300 前半59 で警告0。全23スイートは利用者の判断で未実施。
- 回帰テストの基準は `v77-queue-move-window-b`（main56・half-bTL5・half-uRZn・half--FwFg）。suites.json は全スイートで Tetrace のモデルを使う。
- PPT Lab（tools/ppt-lab/models/fast320 と bin の exe）は同じ中身。

**2026-11-16 整理**: Tetrace-V1.0 より前のエンジン・モデル・解析結果・golden・凍結一式（v74c・v75b を含む）はすべて削除した。
以下の節に出てくる v13〜v74c の exe、旧シーン/NEXT モデル、frozen/runs の旧結果は**もう存在しない**（歴史の記録として残す）。
新しいベストができたら `python toolsreeze_bundle.py --label <REGRESSION-LOG の label>` で同じ一式を作る。

## 1. 何がどこに保存されているか

| 対象 | 場所 | 内容 |
|---|---|---|
| エンジンのソース | `git` の `tools/video-recovery-source/src`, `include` | 版管理済み（初回コミット `68da572`） |
| ビルド済み実行ファイル | `frozen/engines/<sha12>_<label>.exe` | 過去の主要ビルド（v13/v15/v22/v45/v48） |
| ビルドの記録 | `frozen/ENGINE-SNAPSHOTS.jsonl` | 実行ファイル・モデル・設定・gitコミットのSHA |
| ビルドの変遷 | `frozen/ENGINE-CHANGELOG.md` | 各ビルドの変更点と既定挙動 |
| 解析結果 | `frozen/runs/<名前>/` | batch-results / quality-warnings / segments |
| 設定ファイル | `frozen/configs/*.ini` | 全バリエーション |

## 2. ビルドを復元する

```powershell
# 例: v22 に戻す
Copy-Item 'frozen\engines\91f38dcab94d_v22.exe' 'tools\video-recovery-runtime\bin\TetrisVideoRecovery_stream.exe' -Force
```

新しいビルドを作ったら、**必ず**次を実行して記録を残す:

```powershell
python tools\snapshot_engine.py --label v49-<変更内容> --note "<一行説明>"
```

これで exe が `frozen/engines/` に保存され、モデル・ini・gitコミットと一緒に
`ENGINE-SNAPSHOTS.jsonl` に追記される。

## 3. ソースを復元する

```powershell
git log --oneline -- tools/video-recovery-source/src   # 変更履歴
git checkout 68da572 -- tools/video-recovery-source/src tools/video-recovery-source/include
```

**ルール**: ソースを変更したら、ビルド前にコミットする。
そうしないと「どのソースからどの exe ができたか」が失われる。

## 4. モデルを復元する

| モデル | 現在のSHA-256（先頭12） | 保存場所 |
|---|---|---|
| NEXT（校正済み） | `8bbb2479f24f` | `frozen/next_hold-calibrated-2026-11-08.onnx` |
| NEXT（校正前） | `c0cd8801c40e` | `frozen/next_hold-old-20261107.onnx` |
| 盤面 | `da60d1c9a329` | `tools/video-recovery-runtime/models/tetris.onnx`（他と同一） |
| シーン（final-full） | `dc4c652e7ef3` | `frozen/2026-11-06-scene-final-full/model/scene-classifier-final-full.onnx` |
| シーン（旧0.77MB） | `7b62e3e24116` | `tools/ppt-scene-classifier/artifacts/scene-classifier-fp32.old-20260906.onnx` |

## 5. 解析結果の再現に必要なもの

同じ結果を出すには、次の4つを揃える必要がある。

1. エンジンのビルド（SHA）
2. NEXTモデル（SHA）
3. シーンモデル（SHA）
4. `tetris_recover.ini`（SHA）＋環境変数

`ENGINE-SNAPSHOTS.jsonl` の該当行にすべて記録されている。

### 特に注意する環境変数

| 変数 | 既定 | 効果 |
|---|---|---|
| `TR_NVDEC_QUEUE_FRAME_STRIDE` | 2 | 1にすると毎フレーム走査（v22相当）。**結果が変わる** |
| `TR_START_RULE` | go-exit | `legacy` で従来の開始判定 |
| `TR_QUEUE_BOUNDARY` | confirmed | `first` でフェーズ境界を最初の観測に |
| `TR_CLEAR_GRACE_SECONDS` | 0.6 | 品質判定で消去直後を無視する秒 |
| `TR_BOARD_TAIL_SECONDS` | 0.85 | 盤面サンプリングの連鎖除外 |

## 6. 既知の基準値（2026-11-10 時点）

| 実行 | エンジン | 条件 | 360p30の警告 |
|---|---|---|---|
| `calib-full` | v22 | stride=1 | **6件**（10,12,14,19,26,27） |
| `ctl-360p30-stride1-legacy` | v48 | stride=1, legacy開始 | 8件（5,9,12,14,21,26,27,50） |
| `goexit-360p30-stride1` | v48 | stride=1, go-exit開始 | 8件（同上・開始位置のみ6試合で早くなる） |
| `v720p60`（11/06） | v15相当 | 当時のini | 720p60で0件（**再現不可**） |

v22 と v48 は stride=1・legacy でも一致しない（v48 は 8件）。
v22 のソースは残っていないため、v22 の完全再現はできない。

## 追記（2026-11-10 復元検証の結果）

### 再現性テスト
- **同一設定で2回実行 → 完全一致**（試合1/2 の全フェーズで読み・時刻が一致）。
  エンジンは決定的。
- **ただし自動検出モードとマニフェスト指定モードでは結果が変わる**（同一ビルドでも）。
  比較するときは必ず同じモードで実行すること。

### 過去結果の再現テスト（失敗）
- `frozen/engines/91f38dcab94d_v22.exe` を復元し、calib-full と同じ条件
  （自動検出・stride=1・同じモデル・同じ動画）で全56試合を再実行
- 結果: **8件 [5,9,12,14,21,26,27,50]**（保存済み calib-full は 6件 [10,12,14,19,26,27]）
- v48 でも同じ 8件 → **v22 と v48 の差ではなかった**

### 入力の同一性
| 入力 | 記録（calib-full） | 現在 |
|---|---|---|
| シーンモデル | 1,500,731 bytes | 1,500,731 bytes（`dc4c652e`） |
| 動画 | 179,551,658 bytes | 179,551,658 bytes |
| 盤面モデル | — | `da60d1c9`（全コピー同一） |
| NEXTモデル | 記録なし | `8bbb2479`（校正済み） |

入力は一致しているのに結果が違う。

### 分かったこと
- calib-full の `segments.matches.json` には `queue_confirm` / `onnx_samples` /
  `queue_cnn` が**記録されていない**。これは v22 以降のビルドが記録する項目であり、
  **calib-full を作ったのは v22 ではない可能性が高い**（v13〜v15 系）。
- つまり「v22で6件」という前提自体が誤りで、**6件を出したビルドは特定できていない**。
- 6件の結果は `frozen/runs/calib-full-v22-6warn/` に保存済みだが、
  同じ結果を出すビルド・設定の組み合わせは現時点で不明。

### 今後のルール
1. 解析を実行したら、**その場で** `python tools\snapshot_engine.py --label <名前>` を実行し、
   エンジン・モデル・ini・gitコミットを記録する（結果ファイルだけ残しても再現できない）。
2. 結果を比較するときは**実行モードを揃える**（--detect か マニフェスト指定か）。
3. 過去結果と比較する前に、`segments.matches.json` の settings を見て
   同じ世代のビルドか確認する。

## 訂正（2026-11-10 08:00）

前項で「calib-full の settings に queue_confirm が無いので v22 ではない」と書いたが、
**この推測は撤回する**。理由: 全ビルドのバイナリを文字列検索したが
`queue_confirm` という literal はどこにも含まれておらず、
設定JSONのキーは別の方法で組み立てられているため、記録の有無でビルドを判別できない。

### 現時点で確実なこと
- 同一ビルド・同一設定・同一モードの再実行は**完全一致**（決定的）
- **自動検出モードとマニフェスト指定モードでは結果が変わる**
- calib-full の6件は、v22 でも v48 でも再現しない（どちらも8件）
- 6件を出したビルドは**未特定**

### 検証中
- v13 の自動検出モード全編（結果待ち）

## 追記（2026-11-10 08:20）旧ビルドの実行不能

- v13 を自動検出モードで全編実行 → **5試合で停止**（658秒、batch-results 無し）
- v15 を同モードで全編実行 → **1試合で停止**（36秒）
- 当時の0件実行（11/06 08:19、v15相当）は56試合を865秒で完走している

→ **旧ビルドは現在の環境では全56試合を完走できない**。
   したがって「当時の条件」を現在再現して比較することは不可能。

## 帰結
- calib-full の6件を出したビルド・条件は、**もう再現できない**
- 以後の改善は「現在のv48（8件）」を基準に進めるしかない
- 記録は `frozen/ENGINE-SNAPSHOTS.jsonl` に自動で残す

## 現行ベスト（2026-11-10 10:45 時点）

| 項目 | 値 |
|---|---|
| エンジン | v48（SHA `d05916a22e154688…`） `/frozen/engines/d05916a22e15_v48.exe` |
| NEXTモデル | 校正済み（`8bbb2479f24f…`） |
| 盤面モデル | `da60d1c9a329…` |
| シーンモデル | `dc4c652e7ef3…`（fast320） |
| ini | `f9edabafcb4f…`（`frozen/configs/tetris_recover.ini`） |
| 実行条件 | 自動検出モード、`TR_NVDEC_QUEUE_FRAME_STRIDE=1` |
| 結果 | **56試合中8件の品質警告**（5,9,12,14,21,26,27,50）、約30分 |
| 開始ルール | GO最終フレーム+1（`TR_START_RULE=legacy` で従来） |

### 復元コマンド
```powershell
Copy-Item 'frozen\engines\d05916a22e15_v48.exe' 'tools\video-recovery-runtime\bin\TetrisVideoRecovery_stream.exe' -Force
Copy-Item 'frozen\next_hold-calibrated-2026-11-08.onnx' 'tools\video-recovery-runtime\models\next_hold.onnx' -Force
Copy-Item 'frozen\configs\tetris_recover.ini' 'tools\video-recovery-runtime\config\tetris_recover.ini' -Force
$env:TR_NVDEC_QUEUE_FRAME_STRIDE='1'
```

### 試して駄目だったもの（記録済み）
エフェクト除外（v53）・境界=first・P2ボード幅351px・confirm=3/4・
ビーム幅強化・確信度変更・連鎖除外時間の変更。
いずれも6試合では改善したが**全56試合で悪化**。

### 残る課題
- P2側に不一致が偏る（300 vs 51）。幅では説明できず未解明
- ライン消去区間の観測不能
- キュー消費と盤面手数の対応ずれ

## 復元基盤の完全性チェック（2026-11-10 11:10）

`frozen/ENGINE-SNAPSHOTS.jsonl` の記録と実ファイルのSHA-256を照合した結果:

| 項目 | 結果 |
|---|---|
| スナップショット記録 | 6件 |
| ハッシュ不一致・欠落 | **0件** |
| 保存エンジン | 11個 |
| 保存した解析結果 | 4セット |
| 保存した設定 | 20個 |

すべて一致。**復元基盤は健全**。

現行ベストを `BEST-v48-8warn` として記録した
（エンジン `d05916a22e15`、ini 354px、校正NEXT、360p30で8件）。
