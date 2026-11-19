# 引き継ぎ書（ぷよテト解析パイプライン / PPT Lab）

作成: 2026-11-11 / 前担当エージェントより
対象リポジトリ: `C:\Users\hirom\Documents\tetris ai`
HEAD: `477a9b7e`

---

## 0. 最初に読むもの

1. この文書
2. `frozen/RESTORE.md`（復元手順）
3. `frozen/ENGINE-CHANGELOG.md`（ビルドと実験の全履歴・失敗も全部載っている）
4. `review/labsite-360p30-results.md`（直近の外部動画での検証結果）
5. **`HANDOFF-FORENSICS.md`（品質警告の原因究明を繰り返すための手順と専用ツール）**

---

## 1. プロジェクト概要

ぷよぷよテトリス2の動画を解析して、**試合の切り出し**と**両者の盤面・NEXT・操作の復元**を行う。
復元結果はブラウザ用のシミュレータ（`https://selmtoe.github.io/Tetris_Simulator/`）で再生できる形式で出力する。

構成:

| 層 | 場所 | 役割 |
|---|---|---|
| エンジン（C++/CUDA） | `tools/video-recovery-source/` | シーン検出 → キュー(NEXT/HOLD)復元 → 盤面復元 → 品質評価 |
| 実行ファイル | `tools/video-recovery-runtime/bin/TetrisVideoRecovery_stream.exe` | 上記のビルド成果物（**配備先**） |
| 設定 | `tools/video-recovery-runtime/config/tetris_recover.ini` | 盤面ROI・ビーム幅・しきい値 |
| モデル | `tools/video-recovery-runtime/models/*.onnx` | シーン/盤面/NEXT の3種 |
| 学習・評価ツール | `tools/next-cnn/`, `tools/*.py` | NEXT CNN の学習、比較、レビュー生成 |
| 閲覧UI | `tools/video-recovery-runtime/bin/TetrisStudy.exe` | 最新バッチ結果を表示 |
| 記録（凍結） | `frozen/` | 過去ビルド・設定・解析結果・変更履歴 |

---

## 2. 現在の状態（これを壊さないこと）

### 配備中

| 項目 | 値 |
|---|---|
| エンジン | v48 / SHA `d05916a22e15`（`frozen/engines/d05916a22e15_v48.exe`） |
| NEXTモデル | `8bbb2479f24f`（校正済み, `frozen/next_hold-calibrated-2026-11-08.onnx`） |
| 盤面モデル | `da60d1c9a329` |
| シーンモデル | `dc4c652e7ef3` |
| ini | `f9edabafcb4f`（`shift_threshold=0.60`） |
| git | `477a9b7e` |

### ベンチマーク（360p30・全56試合・`TR_NVDEC_QUEUE_FRAME_STRIDE=1`）

- **品質警告 8件**（試合 `5,9,12,14,21,26,27,50`）／区間 42件／所要 約1,815秒
- 結果一式: `frozen/runs/goexit-360p30-stride1-8warn/`
- 参考: 過去の v22 は 6件（`10,12,14,19,26,27`）だったが、**現存するどのビルドでも再現しない**（後述）

### 直近の外部検証（Lab サイトの18本）

- 216試合（各動画の先頭12試合）で **警告56件（25.9%）**、区間227
- 区間検出は公開データとほぼ一致（1,459/1,460、開始時刻の差の中央 0.000秒）
- 詳細: `review/labsite-360p30-results.md`、生データ `frozen/runs/labsite-360p30-12matches/`

---

## 3. 絶対に守るルール（過去に事故が起きている）

1. **モデルを上書きしない**。学習し直したら別名で保存し、`frozen/` に控えを残す。
2. **ソースを変えたらビルド前にコミット**。ビルド後に `python tools\snapshot_engine.py --label <名前> --note "<説明>"` を必ず実行（exe・モデルSHA・ini・gitコミットが `frozen/ENGINE-SNAPSHOTS.jsonl` に1行で残る）。
3. **大きなファイルを git に入れない**。以前 `git add frozen` で 80,942ファイル・1GB を巻き込み、`.git` が 15.8GB になった。`frozen/**/*.exe|onnx|zip` と `review/**/clips/` は `.gitignore` 済み。`git add -A` を安易に使わない。
4. **品質警告を減らすためだけの変更をしない**。評価側だけ緩める変更（除外・猶予・しきい値）は「復元そのものが変わっていない」ことを必ず確認する。実際に消去猶予を2.4秒にしたら警告0件になったが、**8,279手のうち2,540手が別の復元**になっていた。
5. **必ず全56試合で検証する**。6試合だけの予備検証は、これまでに3回、全編の結果と逆の結論を出している。
6. **比較は同じ実行モードで**。`--detect`（自動検出）と マニフェスト指定は**結果が違う**。混ぜて比較すると「ラベルがずれた」等の誤診になる。

---

## 4. 再現・復元の手順


> **注意（2026-11-11 追記）**: 下の実行例が渡す `tools\video-recovery-runtime\models\scene-classifier.onnx` は**旧シーンモデル `7b62e3e24116`**。
> 8件ベースラインは `frozen6-11-06-scene-final-full\model\scene-classifier-final-full.onnx`（`dc4c652e7ef3`）で作られている。
> 旧モデルで回すと 53/56 試合の境界が動き 757 手が変わる。検証は `python tools
egress.py run ...` を使うこと（正しいモデルを自動で渡す。`frozen/golden/README.md`）。

```powershell
# ビルド
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\video-recovery-source\build_cuda.ps1 `
  -RangeBatch -NextGeneration -PerfectClearGuard `
  -OutputDirectory scratch\end3\build -OutputName v55-<名前>.exe

# 配備（bin にコピー）→ 記録
Copy-Item scratch\end3\build\v55-<名前>.exe tools\video-recovery-runtime\bin\TetrisVideoRecovery_stream.exe -Force
python tools\snapshot_engine.py --label v55-<名前> --note "<一行>"

# 全56試合の解析（自動検出モード）
$env:TR_NVDEC_QUEUE_FRAME_STRIDE = '1'
& tools\video-recovery-runtime\bin\TetrisVideoRecovery_stream.exe `
  'C:\Users\hirom\AppData\Local\PPTLab\media\video-799b6882cb7d40f0\[yD_ZukVrgHY].mkv' `
  --detect <出力ディレクトリ> `
  tools\video-recovery-runtime\models\scene-classifier.onnx `
  tools\video-recovery-runtime\models\tetris.onnx `
  tools\video-recovery-runtime\config\tetris_recover.ini

# 警告の比較
python tools\compare_warnings.py <A>\quality-warnings.json <B>\quality-warnings.json
```

**注意（ハマりどころ）**

- `build_cuda.ps1` は**成功しても終了コード1**を返すことがある（PowerShellがstderrの警告をエラー扱いするため）。`Built ...exe` の行と、ファイルの更新時刻で判断する。
- エンジンは出力ディレクトリに **フラットに** `batch-results.json` / `quality-warnings.json` / `match_001...` を書く。アプリ（TetrisStudy）は `data/batches/<動画ID>/batch-results.json` を読むので、**出力先は `data/batches/<動画ID>` を指定する**。
- 出力先に前回の `match_XXX` が残っていると混ざる。実行前に退避するか、別ディレクトリへ出す。
- 動画IDに `[ ]` が入る（例 `[yD_ZukVrgHY]`）。PowerShell は `-LiteralPath`、Python は `glob` ではなく `os.listdir` を使う（`glob` は `[]` を文字クラスとして解釈して一致しない）。
- 旧ビルド（v13/v15）は**現在の環境では全編を完走できない**（途中で停止）。「exeを戻せば再現」ではない。**結果は `frozen/runs/` から復元する**のが正しい。

---

## 5. 既知のバグ・負債（優先度順）

### P0: 盤面の「説明のつかない変化」を検出できていない
復元タイムライン上で、**どの配置でもゴミ上昇でも説明できない盤面の変化**を作れてしまう。
実際に試験実装（v54, `TR_RESYNC`）が1試合あたり2箇所のワープを挿入したが、`recoveryUnresolved=0` のまま出力された。
→ 「盤面が説明なく変わるフェーズ」を不正として弾く検証を入れるべき（品質警告を増やすためではなく、嘘の合法手を作らせない安全装置）。

### P0: 復元できない状態が生まれる経路が残っている
- ソースが git 管理下に入ったのは 2026-11-10（初回コミット `68da572`）。それ以前のビルドは**ソースから再現不可**。
- そのため v22 の「6件」は exe を保存してあっても再現しない（環境依存の可能性）。
- 対策: 以後のビルドは必ずスナップショット＋ソースコミット。**結果も `frozen/runs/` に保存**する。

### P1: 文字コード事故（PowerShell 5.1）
`Get-Content -Raw | Set-Content -Encoding utf8` は **UTF-8 ファイルを cp932 として読み、二重エンコードで破壊する**（実際に1ファイル壊した）。
- テキスト編集は Python（`encoding="utf-8"`）かエディタ経由で行う。
- 読み込みは `utf-8-sig`（BOM付きファイルで `json.load` が落ちる）。

### P1: 閲覧ページの動画がシークできない（解決済み・要周知）
`python -m http.server` は **Rangeリクエスト非対応**。先頭数秒しか再生できず、シークも効かない。
→ `python tools\serve_range.py 8765 review\warning-matches` を使う（206 Partial Content 対応）。

### P1: 修復ラダーが特定試合向けの特例の寄せ集め
`recovery.cpp` / `tetris_engine.cpp` には「match 130 では…」「match 11 では…」といった**個別試合のコメント付き特例**が多数ある。根拠は残っているが、変更のたびに別の試合が壊れる。
→ 特例は**回帰テスト（ゴールデンファイル）に落としてから**整理する。闇雲なリファクタは危険。

### P2: 巨大ファイル
`recovery.cpp` 486KB / `tetris_engine.cpp` 119KB / `main.cpp` 178KB。1ファイル1責務に割れていない。

### P2: 設定の散在
環境変数が60個以上 `getenv` で散らばっている（`TR_*`）。既定値の一覧がコードからしか分からない。

### P2: 1試合が異常に遅くなることがある
修復ラダーが発火すると1試合で10分以上かかる（720p検証で観測）。`TR_REPAIR_TIME_BUDGET_SECONDS` で上限はあるが、既定値の根拠が不明。

---

## 6. 解析品質の現状と、これまでの結論（数字）

### 品質警告の内訳（360p30・8件）
- 区間42件のうち **36件がP2**（P1は6件）
- 不一致の分解（`scratch/diag/mismatch_decomp.py`）:
  - `recG`（復元だけがゴミ行を持つ）が警告8試合で **15〜28セル/フェーズ**、非警告試合は 0〜4
  - `other`（色・空の不一致）は全試合で 17〜29セル/フェーズの一定値（＝毎フレームのノイズ床）
- 盤面一致率の分布（全8,142フェーズ）: 中央 **0.990**。非警告試合 0.995 / 警告試合 0.960
  → 通常はほぼ完全一致。警告試合だけが**持続的にずれる**

### 試したが効かなかったこと（再試験不要）
| 変更 | 結果 |
|---|---|
| 消去猶予2.4秒（TR_CLEAR_GRACE_SECONDS） | 警告0件だが**復元が2,540手変化**（不正な改善） |
| 境界ルール `first` | 8→11件（悪化） |
| P2盤面幅 354→351px | 6試合では改善、全56で8→13件（悪化） |
| confirm=3/4、beam 96→3000、信頼度しきい値 | ほぼ無変化 |
| エフェクト（白飛び）除外 v49〜v53 | 6試合では区間30→25、全56では8→9件（悪化） |
| ゴミ上昇しきい値 `shift_threshold` 0.60→0.85 | 8→8件だが**別の試合が壊れる**（27が消え7が出る） |
| 観測ボード再同期 v54（`TR_RESYNC`） | 見かけ上 7→6件、実際は**盤面ワープ**（不正） |
| 720p30 で再解析 | 7→7件、5→7件（改善せず） |

### 有力な仮説（未解決）
1. **ライン消去の取りこぼし**: 盤面の1〜2セルの読み違いでラインが揃わず、消去されずに差が残る。以後のミノが全部1〜2行上に積まれて永久にずれる（試合14で実映像と突き合わせて確認済み）。
2. **ゴミ行の過剰挿入**: 復元だけがゴミ行を多く持つ（`recG`）。実映像では観測が正しく、復元が誤り（試合14 P2、`t=68.09`）。
3. どちらも「**盤面デコーダの1セル精度**」に帰着するが、解像度を4倍にしても改善しないため、単純な画素不足ではない。

---

## 7. 依頼したい作業（優先順）

### 最優先: 安全網（壊さない仕組み）
1. **リプレイ検証器**: 各フェーズ間で「合法手 or ゴミ上昇 or 消去」で説明できない盤面変化を検出し、`recoveryUnresolved`（または専用フラグ）にする。現状の出力はこれを検出できない。
2. **ゴールデン回帰テスト**: 動画4本×各12試合を固定入力にし、変更前後で
   - 警告数だけでなく **盤面トレース（フェーズごとの board/fullBoard のハッシュ）** を比較する。
   警告数が同じでも復元が別物になっている事例が何度もあった。
3. **ワンコマンド化**: 「ビルド → 配備 → スナップショット → 全56実行 → 比較 → 記録」を1スクリプトに。人が手順を踏むと抜ける。
4. **復元ドリル**: `ENGINE-SNAPSHOTS.jsonl` のハッシュと `frozen/engines/` の実ファイルを照合する検査（過去に1回実施、0件不一致）。

### 次点: 品質改善（研究要素あり）
5. 盤面デコーダの精度向上（1セル単位の誤読を減らす）。現在の盤面モデルは `da60d1c9a329`。
   - 時刻方向の安定化（同一盤面が複数フェーズ続くときの投票）
   - 消去アニメ中の観測を「捨てる」のではなく「消去と確定して扱う」
6. P2偏重の原因究明（P2側だけ36/42）。座標は左右対称なので、映像圧縮以外の要因（UIの重なり等）を疑う。
7. 上記が難しければ、**「0件」を目標にしない**判断もあり。現状は 8/56（14.3%）、Lab動画は 25.9%。

### リファクタリング（品質改善と切り離してやる）
8. `recovery.cpp` / `tetris_engine.cpp` / `main.cpp` の分割。ただし**必ずゴールデンテストを先に**用意する。
9. `TR_*` 環境変数の一覧化（既定値・意味・影響範囲を1枚の表に）。
10. 個別試合向け特例をテストケース化してから整理する。

---

## 8. 私（前担当）の失敗と、そこから言えること

- クリップの切り出しに余白を付け、全試合が「ゴー！」から始まる状態にした（利用者に2回指摘された）。**出力物は必ず先頭フレームを目視確認する**。
- `python -m http.server` で動画を配信し、シークできない状態にした。**動画を配るなら Range 対応が必須**。
- PowerShellでテキストを書き換えて1ファイルを文字化けで破壊した。**非ASCIIはPythonで扱う**。
- 6試合だけの検証で「改善」と判断しかけ、全56で悪化したことが複数回あった。**判断は全編で**。
- 「盤面を画像に合わせる」案を、合法手の連続性を確認せず採用しかけた（A/Bの数字だけ見ると改善に見えた）。**数字が良くても復元が嘘なら価値はマイナス**。

いちばん伝えたいこと: **この案件の最大の敵は「精度が出ないこと」ではなく「良く見えるが再現できない/嘘の状態が残ること」**。
復元基盤（git・スナップショット・結果保存）と検証器を先に固めてから、精度の話をしてほしい。

---

## 9. 最初の30分でやること

1. `python tools\snapshot_engine.py --label check` は**実行しない**（記録が増えるだけ）。代わりに `frozen/ENGINE-SNAPSHOTS.jsonl` の最後の行と `bin/TetrisVideoRecovery_stream.exe` のSHAを照合する。
2. `frozen/runs/goexit-360p30-stride1-8warn/quality-warnings.json` を読み、警告8件の形を確認。
3. `tools\compare_warnings.py` と `tools\build_warning_matches.py` を動かして、比較と閲覧の流れを掴む（閲覧は `python tools\serve_range.py 8765 review\warning-matches`）。
4. 変更を加える前に、まず**リプレイ検証器**を書く（P0）。それが無い状態での改善は、過去に何度も「別の試合を壊す」結果になっている。

---

## 附録: 主要ファイル

| パス | 内容 |
|---|---|
| `frozen/ENGINE-CHANGELOG.md` | ビルド一覧（v13〜v54）、環境変数、実験の全記録、失敗例 |
| `frozen/RESTORE.md` | 復元手順、再現に必要な4点（エンジン/モデル/シーン/ini） |
| `frozen/ENGINE-SNAPSHOTS.jsonl` | ビルドごとのSHA（exe/next/board/scene/ini/git） |
| `frozen/runs/` | 解析結果（calib-full-v22, ctl-legacy, goexit, shift085, labsite） |
| `review/labsite-360p30-results.md` | Lab 18本の検証結果 |
| `review/warning-matches/` | 警告試合のクリップ＋閲覧ページ（64試合） |
| `tools/snapshot_engine.py` | ビルド記録 |
| `tools/compare_warnings.py` | 警告の差分比較 |
| `tools/build_warning_matches.py` | 試合クリップ生成＋ページ |
| `tools/serve_range.py` | Range対応の静的サーバ |
| `tools/labsite_summary.py` / `labsite_compare.py` | Lab動画の集計・公開データとの比較 |
| `scratch/diag/*.py` | 調査用スクリプト（mismatch_decomp, ratio_dist, jumpcheck など） |
