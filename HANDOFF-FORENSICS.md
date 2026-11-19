# 品質警告の原因究明を「繰り返す」ための引き継ぎ

対象: 品質警告が出た試合を、1試合ずつ深掘りして原因を特定し、修正→全56試合で検証、を繰り返す作業。
前提: `HANDOFF.md`（プロジェクト全体）を先に読むこと。

---

## 1. 「品質警告」の定義（ここを間違えると全部ずれる）

`tools/video-recovery-source/src/recovery.cpp` の `evaluateRecoveryQuality` が、
**プレイヤーごと・フェーズごと**に「復元盤面」と「映像から読んだ盤面」を比較し、
一致率が **0.70 未満のフェーズが 2 連続以上**続いた区間を `issue` として記録する。
試合に `issue` が 1 つでもあれば `suspicious=true`（＝品質警告）。

- 比較しているのは**可視20行（row 20〜39）×10列 = 200セル**
- 一致率 = 一致セル数 / 200
- 出力: `quality-warnings.json` の `matches[].quality_warning.issues[]`
  - `player`, `first_phase`, `last_phase`, `consecutive_phases`, `minimum_match_ratio`
  - `source_start_seconds` / `source_end_seconds`（元動画の絶対秒）
- **`first_phase`/`last_phase` は「そのプレイヤーのタイムライン」の添字**（p1とp2で長さが違う）

---

## 2. 究明のループ（これを回す）

```
(1) どの試合か選ぶ
      ↓
(2) フォレンジックページを開く（観測と復元のセル単位差分＋映像フレーム）
      ↓
(3) 「最初にずれたフェーズ」を特定する（最悪フェーズではない）
      ↓
(4) ずれ方の型を分類する（下の表）
      ↓
(5) 映像フレームで「観測が正しいのか、復元が正しいのか」を確定する
      ↓
(6) メカニズム（コード上の原因）を仮説立てして、1つだけ変える
      ↓
(7) 全56試合で検証（モードを揃える）＋合法手の連続性を確認
      ↓
(8) 記録（ENGINE-CHANGELOG.md、frozen/runs/、スナップショット）
      ↓ (1) に戻る
```

### (2) フォレンジックページ

```powershell
# 1試合だけ
python tools\forensics_match.py `
  --run frozen\runs\goexit-360p30-stride1-8warn `
  --data 'tools\video-recovery-runtime\data\batches\[yD_ZukVrgHY]' `
  --match 14 --out review\forensics\match-014 `
  --video 'C:\Users\hirom\AppData\Local\PPTLab\media\video-799b6882cb7d40f0\[yD_ZukVrgHY].mkv' `
  --frames 3

# 警告が出た試合を全部
python tools\forensics_match.py --run <run> --data <match_XXXがある場所> --all --out review\forensics\<名前> --video <動画> --frames 2
```

ページに載るもの:

- 区間ごとの**フェーズ表**（時刻・action・配置・消去数・garbage行数・一致率・obsG/recG/other）
- **観測グリッドと復元グリッドの並置**（不一致セルは赤枠）＝最悪フェーズ
- 区間の**映像フレーム**（等間隔）
- 閲覧は `python tools\serve_range.py 8766 review\forensics`

### (3) 「最初にずれたフェーズ」の探し方

区間の `first_phase` は「0.70を2回連続で割った最初」であって、**ずれ始めではない**。
表の中で一致率が 0.95 を初めて割ったフェーズ、あるいは `recG`/`other` が急に増えたフェーズを見る。
だいたい「ライン消去の直後」「ゴミが降ってきた直後」に始まっている。

### (4) ずれ方の型と、疑うべき場所

| 型 | 見え方 | 疑う場所 |
|---|---|---|
| **復元だけゴミ行が多い** | `recG` が 15以上。復元の山が実物より高い | ゴミ上昇の受理（`sourceBoardShift` / `garbageShift`）、`shift_threshold` |
| **ゴミ行の穴位置が全部違う** | 同じ行数だが G の穴がずれる | ゴミ穴の読み取り（`holesFromObserved`, `garbageHolePosteriors`） |
| **色の並びが数行上にずれる** | 形は同じで全体が上下にシフト（`other` が均等） | ライン消去の取りこぼし（消去判定・盤面1セルの誤読） |
| **左右が違う／別物** | 山の位置がまるで違う（`other` が大きい） | ピース識別（NEXT）か配置探索（`legalMoves` / ビーム） |
| **観測が白飛び・半透明** | 映像フレームが明るい／盤面が透ける | 観測側（エフェクト）。復元を合わせるのではなく**観測を捨てる**話 |

参考値（360p30・全56試合）:
- 一致率の中央 **0.990**、非警告試合 0.995、警告試合 0.960
- 警告試合の `recG` は 15〜28セル/フェーズ、非警告は 0〜4
- `other` は全試合で 17〜29（＝落下中ミノ等の常時ノイズ床）

### (5) 映像で確定させる

**必ずフレームを見る。** 復元JSONだけでは「観測が正しい」のか「復元が正しい」のか決められない。
実例: 試合14 P2 `t=68.09` は、映像では**ゴミ3行**なのに復元は**8行**だった（＝復元が誤り）。

```powershell
# 任意の秒のフレーム
& tools\video-recovery-runtime\bin\ffmpeg.exe -ss 744.82 -i "<動画>" -frames:v 1 -vf "crop=118:238:417:52,scale=354:714" -y scratch\diag\f.png
```
（P2盤面の切り出し座標。P1は `crop=117:238:105:52`。360p動画での値）

### (6) 変えるのは「メカニズム」だけ

しきい値・猶予・除外をいじるのは最後の手段。過去に **警告0件になったが復元が2,540手変わった**例がある。
変更は1つずつ。`TR_*` 環境変数で切り替えられる形にしておくと、A/Bと切り分けができる。


> **注意（2026-11-11 追記）**: 下の実行例が渡す `tools\video-recovery-runtime\models\scene-classifier.onnx` は**旧シーンモデル `7b62e3e24116`**。
> 8件ベースラインは `frozen6-11-06-scene-final-full\model\scene-classifier-final-full.onnx`（`dc4c652e7ef3`）で作られている。
> 旧モデルで回すと 53/56 試合の境界が動き 757 手が変わる。検証は `python tools
egress.py run ...` を使うこと（正しいモデルを自動で渡す。`frozen/golden/README.md`）。

### (7) 検証のルール

- **全56試合**で実行（6試合の予備検証は3回裏切られている）
- 比較は**同じ実行モード**（`--detect` と マニフェスト指定は結果が違う）
- 警告数だけでなく、**盤面トレースが変わっていないか**も見る（`scratch/diag/jumpcheck.py` が雛形）
- 合法手で説明できない盤面変化が出たら、その変更は**不正**として捨てる

```powershell
$env:TR_NVDEC_QUEUE_FRAME_STRIDE='1'
& tools\video-recovery-runtime\bin\TetrisVideoRecovery_stream.exe "<動画>" --detect <出力> `
  tools\video-recovery-runtime\models\scene-classifier.onnx `
  tools\video-recovery-runtime\models\tetris.onnx `
  tools\video-recovery-runtime\config\tetris_recover.ini
python tools\compare_warnings.py <A>\quality-warnings.json <B>\quality-warnings.json
```

### (8) 記録

- `frozen/ENGINE-CHANGELOG.md` に「何を変えたか／結果／なぜ採用・不採用か」
- 結果一式を `frozen/runs/<名前>/` に保存（`quality-warnings.json` + `batch-results.json` + `segments.matches.json`）
- ビルドしたら `python tools\snapshot_engine.py --label vXX-<名前> --note "<一行>"`

---

## 3. 現在の警告8試合と、分かっていること

| 試合 | 区間 | 主な型 | 分かっていること |
|---|---|---|---|
| 5 | 3（P1 1, P2 2） | 混在 | P2の`recG`22.3 |
| 9 | 5（全てP2） | 復元が高い | `recG`21.7 |
| 12 | 9（P2 7, P1 2） | 混在 | P1`recG`23.5、P2は`other`35 |
| 14 | 7（全てP2） | 復元が高い | `t=68.09`で映像=ゴミ3行／復元=8行。`t≈24秒`で2ライン消去を取りこぼし、以降追随不能 |
| 21 | 4（全てP2） | 復元が高い | `recG`19.6 |
| 26 | 8（全てP2） | 復元が高い | `recG`19.8、区間288フェーズ |
| 27 | 3（P1 3） | 上下シフト | 盤面が1行上にずれる（`other`61.6） |
| 50 | 3（全てP2） | 観測がゴミ多 | `obsG`16.5（画像にゴミ、復元に無い） |

試したが効かなかったもの（再試験不要）は `HANDOFF.md` の表を参照。

---

## 4. よくある罠

1. **モード混在**: `--detect` とマニフェスト指定を比較する。→ 同じモードで。
2. **6試合で判断**: 全56で判断する。
3. **評価側だけ緩める**: 警告は減るが復元は変わらない＝意味がない。必ず盤面トレースを見る。
4. **「盤面を画像に合わせる」**: 一見改善するが、合法手で説明できないワープを作る（v54で実証）。禁止。
5. **`[ ]` を含む動画ID**: PowerShellは `-LiteralPath`、Pythonは `glob` ではなく `os.listdir`。
6. **文字コード**: PowerShell 5.1 の `Get-Content -Raw | Set-Content -Encoding utf8` は日本語を壊す。Pythonで書く。

---

## 5. 次の担当への依頼文（コピペ用）

> `C:\Users\hirom\Documents\tetris ai` の品質警告の原因究明を引き継いでください。
> まず `HANDOFF.md` と `HANDOFF-FORENSICS.md` を読んでください。
> 品質警告が出た試合（現在は360p30の全56試合で8件）について、
> `tools\forensics_match.py` で1試合ずつ「観測と復元のセル単位差分＋映像フレーム」を出し、
> **最初にずれたフェーズ**を特定して、ずれ方の型（ゴミ行の過剰／ゴミ穴の誤読／消去の取りこぼし／ピース識別）を分類してください。
> 仮説は必ず映像フレームで確認し、変更はメカニズム単位で1つずつ。
> 検証は全56試合・同一モードで行い、警告数だけでなく**盤面トレースが合法手で説明できるか**を確認してください。
> 結果は `frozen/ENGINE-CHANGELOG.md` と `frozen/runs/` に記録し、ビルドは `tools\snapshot_engine.py` で残してください。
> 「警告を減らすためだけの変更」は禁止です。

---

## 6. 使うツール一覧

| ツール | 用途 |
|---|---|
| `tools/forensics_match.py` | 1試合の詳細ページ（差分グリッド＋フレーム＋分解表） |
| `tools/build_warning_matches.py` | 警告試合のクリップ＋一覧ページ |
| `tools/serve_range.py` | ページ閲覧用サーバ（Range対応） |
| `tools/compare_warnings.py` | 2つの実行の警告差分 |
| `scratch/diag/mismatch_decomp.py` | obsG/recG/other の集計 |
| `scratch/diag/ratio_dist.py` | 一致率の分布 |
| `scratch/diag/jumpcheck.py` | 合法手で説明できない盤面変化の検出 |
| `scratch/diag/lagtest.py` | 復元が何秒前の観測に一致するか（追随遅れの検出） |
