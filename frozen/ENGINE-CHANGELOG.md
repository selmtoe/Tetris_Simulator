# エンジンビルド記録（2026-11-05 〜 2026-11-10）

対象: `TetrisVideoRecovery_stream.exe`（映像解析エンジン）
置き場: `scratch/end3/build/`（ビルド成果物）、`tools/video-recovery-runtime/bin/`（配備先）

## 重要な前提
- **ソースはGit管理外**（`tools/video-recovery-source/src` と `include` は未追跡）。
  そのため「このビルドはどのソースか」を後から厳密に再現できない。
- ビルドは `build_cuda.ps1 -RangeBatch -NextGeneration -PerfectClearGuard`。
- **既定動作がビルドごとに変わる**ものがあり、比較時は必ずビルドを揃える必要がある。

## ビルド一覧（2026-11-05 以降）

| ビルド | 作成日時 | 主な変更 | 既定の挙動 |
|---|---|---|---|
| v13 | 11/05 04:53 | （当時の安定版） | キュー走査 stride=1、開始=analysis_start/gameplay |
| v14 | 11/06 04:43 | GOマージ・開始境界の実験 | 同上 |
| v15 | 11/06 05:24 | 開始境界の調整 | 同上（**11/06 08:19の「720p60警告0件」実行時の最新ビルド**） |
| v16〜v20 | 11/06 09:58〜11:55 | 境界・修復の調整 | 同上 |
| v21/v22 | 11/06 19:26/19:27 | 修復・キュー確定の調整 | **stride=1（毎フレーム走査）** |
| v23 | 11/08 09:21 | `TR_QUEUE_LABEL_MODEL`（ラベル専用モデル） | v22同等 |
| v24 | 11/08 09:45 | 上記を非同期経路にも適用 | 同上 |
| v25 | 11/08 09:56 | `TR_QUEUE_SCHEDULE`（旧の変更時刻で更新） | 同上 |
| v26〜v33 | 11/08 10:02〜10:16 | スケジュール/強制採用の調整 | 同上 |
| v40 | 11/09 06:55 | **fps適応のキュー確定回数**（`confirm × fps/30`） | stride=1 |
| v41 | 11/09 07:30 | stride/重複除去を設定化。**既定を stride=2 に変更** | **stride=2（v22と異なる）** |
| v42 | 11/09 09:23 | `TR_QUEUE_BOUNDARY`（first/confirmed）。既定=first | stride=2 |
| v43 | 11/09 10:07 | 上記の既定を confirmed に戻す | stride=2 |
| v44 | 11/09 12:52 | `TR_BOARD_TAIL_SECONDS`（連鎖除外時間） | stride=2 |
| v45 | 11/09 13:15 | 品質判定に「消去アニメ猶予」を追加（既定0.6秒） | stride=2 |
| v46 | 11/09 14:10 | 修復用/出力用で判定を分離 | stride=2 |
| v47 | 11/09 14:40 | `TR_BOARD_MIN_CONFIDENCE`（既定0=無効） | stride=2 |
| v48 | 11/10 02:08 | **開始位置を GO最終フレーム+1 に**（`TR_START_RULE=legacy`で従来） | stride=2 / go-exit |

## 環境変数（実験用フック）

| 変数 | 意味 | 既定 |
|---|---|---|
| `TR_NVDEC_QUEUE_FRAME_STRIDE` | キューの走査間隔。1で毎フレーム | **2**（v41以降） |
| `TR_QUEUE_DEDUPE` | 同一PTSフレームの除去 | 0（無効） |
| `TR_QUEUE_BOUNDARY` | フェーズ境界を first / confirmed のどちらで置くか | confirmed |
| `TR_BOARD_TAIL_SECONDS` | 連鎖（ライン消し）末尾の除外秒 | 0.85 |
| `TR_CLEAR_GRACE_SECONDS` | 品質判定で消去直後を無視する秒 | 0.6（v46以降は出力用のみ） |
| `TR_BOARD_MIN_CONFIDENCE` | 盤面観測の平均自信度がこれ未満のフェーズを判定から除外 | 0（無効） |
| `TR_START_RULE` | `legacy` で従来の開始判定 | go-exit |
| `TR_QUEUE_CONFIRM_DEBUG` | fpsと確定回数をログ出力 | 0 |

## 実行と結果の対応（このセッションで確認できたもの）

| 実行 | ビルド | 主な設定 | 動画 | 警告 | 時間 |
|---|---|---|---|---|---|
| `v720p60`（11/06 08:19） | v15相当 | 不明（当時のini） | 720p60 | **0件** | 865秒 |
| `calib-full` | **v22** | stride=1、既定 | 360p30 | **6件**（10,12,14,19,26,27） | 957秒 |
| `v22-new-idle` | v22 | 未校正NEXT | 360p30 | 10件 | 2,570秒 |
| `hybrid-full` | v33 | スケジュール | 360p30 | 6件 | 1,290秒 |
| `q-360p30-grace24` | v45 | 消去猶予2.4秒 | 360p30 | 0件（※復元が変化） | 1,461秒 |
| `r-360p30-split` | v46 | 判定分離 | 360p30 | 12件 | 1,587秒 |
| `k-720p60-detect` | v40 | 校正NEXT | 720p60 | 10件（53試合まで） | 中断 |
| `t-v13-720p60` | v13 | 旧NEXT | 720p60 | 10件 | 1,730秒 |
| `goexit-360p30` | v48 | go-exit、**stride=2** | 360p30 | 12件 | 2,284秒 |

## 注意（重要）
- **v22とv41以降は既定のキュー走査間隔が違う**（1 vs 2）。
  両者を比較するときは `TR_NVDEC_QUEUE_FRAME_STRIDE=1` を明示すること。
- `calib-full`（6件）は **v22 + stride=1** の結果。
- v41以降の既定（stride=2）では、同じ動画でも結果が変わる。

## 未解決
- 11/06 08:19の720p60 0件の条件は未特定。v15・v13・全NEXTモデル候補・
  ini候補（confirm4/queue）・サンプル数2/3 のいずれでも再現しなかった。
  `.tstudy` には設定が残っていない（manifest参照と判定のみ）。

## 追記（2026-11-10 のA/B）

条件を揃えた比較（v48・stride=1・同一ini・同一動画360p30）:

| 開始ルール | 警告 | 区間数 | 時間 |
|---|---|---|---|
| legacy（analysis_start/gameplay） | 8件（5,9,12,14,21,26,27,50） | 42 | 1,581秒 |
| **go-exit（GO最終フレーム+1）** | **8件（同一）** | **42** | 1,815秒 |

開始位置は6試合で早くなった（試合2 −1.13s / 10 −0.93s / 19 −0.60s / 37 −1.00s / 39 −1.03s / 42 −1.00s）。
**品質警告は変化なし**＝GO終了基準は安全に適用できる。

stride=2 で go-exit を試した場合は12件（[4,5,9,12,14,21,24,26,27,28,45,50]）だったため、
比較時は stride を必ず揃えること。

v22（6件）と v48（8件）は stride=1・legacy でも一致しない。
ソース未追跡のため v22 の完全再現は不可。

## 追記（2026-11-10 08:30）エフェクト除外フィルタ

### 実装（v49→v53）
- `BoardObservation.effectRatio` を追加（盤面画像の白飛び率）
- `aggregate()` で `effectRatio >= TR_EFFECT_REJECT_RATIO`（既定0.25）のサンプルを除外
- 保持フレーム経路（`analyzeRetainedBoard`）にも同じ計測を追加
  ※ v50 ではこの経路に入れておらず**効果ゼロ**だった（実測で判明）

### 効果率の実測（試合10・294サンプル）
| 指標 | 値 |
|---|---|
| 中央 | 0.046 |
| 90%点 | 0.534 |
| 最大 | 0.726 |
| 0.25超 | 53件（18%） |

→ ライン消去中は実際に白飛び率が跳ね上がる（フィルタの妥当性を確認）

### 結果（警告6試合・マニフェスト指定モード）
| 設定 | 警告 | 区間数 |
|---|---|---|
| v48（フィルタなし） | 2,3,5,6 | 30 |
| v52（0.25） | 2,3,5,6 | **25** |
| v53（0.10） | 2,3,5,6 | 33 |
| v53（0.05） | 2,3,5,6 | **25** |

- 区間数は 30→25 に減少（17%）
- ただし**警告される試合は変わらない**
- しきい値は 0.25（既定）が最も無難（0.05と同等で、除外が少ない）

### 現在の配備
v53（エフェクト除外・既定0.25）＋校正NEXT。
ビルドは `frozen/engines/<sha>_v53-effect-filter.exe` に保存済み。

## 追記（2026-11-10 09:10）エフェクト除外の全編検証 → 不採用

| 実行 | エンジン | 警告 | 区間数 |
|---|---|---|---|
| 基準（go-exit） | v48 | **8件**（5,9,12,14,21,26,27,50） | 42 |
| エフェクト除外 | v53 | **9件**（5,9,12,14,21,26,27,38,45） | 40 |

- 試合50の警告は消えたが、38と45に新たな警告が出た
- 区間数は42→40と微減だが、**警告数は増加**
- → **エフェクト除外は採用せず、v48へロールバック**

教訓: 一部の試合（マニフェスト指定・6試合）で改善しても、
全56試合では別の試合が悪化することがある。**必ず全編で検証する**。

現在の配備: v48（`d05916a22e15`）＋校正NEXT。

## 追記（2026-11-10 09:40）境界=first の全編検証 → 不採用

| 実行 | 警告 | 区間数 |
|---|---|---|
| 基準 v48（confirmed） | **8件**（5,9,12,14,21,26,27,50） | 42 |
| TR_QUEUE_BOUNDARY=first | **11件**（+40,42,45） | 47 |

6試合の予備検証では区間数が30→16と改善したが、全56試合では悪化。
→ **不採用**。既定（confirmed）のまま。

## ここまでの全試行（すべて全編で悪化または変化なし）
| 施策 | 6試合 | 全56試合 |
|---|---|---|
| エフェクト除外（v53） | 30→25区間 | 8→9件（不採用） |
| 境界=first | 30→16区間 | 8→11件（不採用） |
| confirm=3 / beam強化 / 確信度変更 | 微減 | 未検証（予備で効果薄） |

## 結論
残る8件は**設定・サンプリング・境界の調整では動かない**。
ライン消去区間の観測不能と、キュー/盤面の手数ずれという
**構造的な問題**であり、直すには復元ロジック自体の再設計が必要。

現行配備: v48（`d05916a22e15`）。これが現在の最良。

## 追記（2026-11-10 10:10）P2ボード幅の3px誤りを修正

### 発見
`tetris_recover.ini` のボード矩形が左右で不一致だった。

| プレイヤー | 定義 | 幅 |
|---|---|---|
| P1 | `board=316,157,351,713` | **351px** |
| P2 | `board=1253,157,354,713` | **354px**（3px広い） |

同一の盤面を左右に並べているので、幅は同じでなければならない。
P2は `1920 - 316 - 351 = 1253` が正しい位置で、幅も351が正しい。

### 影響（警告6試合・マニフェスト指定）
| 指標 | 修正前 | 修正後 |
|---|---|---|
| 不一致フェーズ P1 | 71 | 71（変化なし） |
| **不一致フェーズ P2** | **173** | **131（−24%）** |
| 区間数 | 30 | 27 |

全56試合でも **不一致フェーズの85%がP2**（300 vs 51）だったため、
この3pxがP2偏重の主因と考えられる。

### 対応
- 本設定 `tetris_recover.ini` を修正（`board=1253,157,351,713`）
- 旧設定は `frozen/configs/tetris_recover.ini.before-p2fix` に保存

### 未検証
- 全56試合での検証は未実施（次ラウンドで実施する）

## 追記（2026-11-10 10:40）P2ボード幅の「修正」は不採用 → 元に戻した

| 実行 | 警告 | 不一致フェーズ P2 |
|---|---|---|
| 基準（354px） | **8件** | 300 |
| 幅を351pxに変更 | **13件**（+4,28,40,43,45） | 383 |

6試合では P2 173→131 と改善したが、全56試合では**悪化**。
→ **351pxへの変更は誤り。354pxが正しい**。設定を元に戻した
（`frozen/configs/tetris_recover.ini.before-p2fix` から復元）。

## 教訓（再確認）
6試合の予備検証は、全56試合の結果と**相関しないことが多い**。
設定変更は必ず全編で検証する（今回で3回目の裏切り）。

## P2偏重について
幅の変更では説明がつかなかった。P2に不一致が偏る理由は**未解明**。
次の候補: P2側のNEXTパネル/ボードの**縦方向**のサンプリング、
または2P側だけ映像の圧縮ノイズが強い可能性。

## 次に実装する内容（2026-11-10 11:00 記録）

### 目的
P2に偏る「一度ずれたら戻らない」不一致を直す。
根拠: 不一致区間の位置は P2 が中央0.72・平均0.65（早期から連続）、
P1 は中央0.82（終盤のみ）。試合14で37連続、試合12で20/19連続。

### 実装案: キュー再同期パス
不一致が N 手以上連続したら、その区間で
「キューの消費を +1 / -1 手シフトした仮説」を作り、
`visibleBoardMatchRatio` の平均が最も良くなるものを採用する。
改善幅が閾値（例 +0.10）を超えた場合のみ適用する。

### 触る場所（現行ソース）
| 役割 | ファイル | 目安の行 |
|---|---|---|
| フェーズ生成（place / place_skipped） | `tools/video-recovery-source/src/recovery.cpp` | 650〜760 |
| 論理リプレイからのフェーズ生成 | 同 | 1320〜1400 |
| 盤面一致率 | 同 | 6237 `visibleBoardMatchRatio` |
| 品質評価（区間検出） | 同 | 7161 `evaluateRecoveryQuality` |
| 既存の盤面修復 | 同 | 6395以降 `queue_board_repair` 周辺 |

### 実装手順
1. 新しい関数 `resyncQueueByHandCount(p1, p2, settings)` を作る
2. `evaluateRecoveryQuality` の後、レポート出力の前に適用
3. 環境変数 `TR_RESYNC=1` で有効化（既定OFF、検証後に既定ONを判断）
4. ビルド → `python tools\snapshot_engine.py --label v54-resync`
5. **全56試合で検証**（`TR_NVDEC_QUEUE_FRAME_STRIDE=1`、約30分）
6. 8件より減れば採用、増えれば破棄してv48へ戻す

### 注意
- 6試合の予備検証は当てにならない（3回裏切られている）。必ず全編で判断する。
- 現在のベストは v48・8件。壊したら `frozen/engines/d05916a22e15_v48.exe` に戻す。

---

## 追記（2026-11-10 11:05）残り8件の内訳を数値で分解した

`goexit-360p30-stride1-8warn` の復元JSON（56試合）から、フェーズごとの
`observed`（画像から読んだ盤面）と `board`（復元盤面）の10×20を比較し、
不一致セルを3種に分解した。ツール: `scratch/diag/mismatch_decomp.py`。

- `obsG` = 観測がゴミなのに復元が違うセル
- `recG` = **復元だけがゴミ**（実物には無いゴミ行）のセル
- `other` = どちらもゴミでない色/空の不一致

### 結果1: 警告8試合は「復元だけゴミが多い」でほぼ説明できる

| 試合 | プレイヤー | 区間数 | 延べフェーズ | obsG | **recG** | other |
|---|---|---|---|---|---|---|
| 5 | P1 | 3 | 32 | 3.5 | 7.1 | 23.0 |
| 5 | P2 | 2 | 51 | 6.0 | **22.3** | 19.5 |
| 9 | P2 | 2 | 131 | 6.8 | **21.7** | 20.9 |
| 12 | P1 | 3 | 89 | 7.8 | **23.5** | 22.6 |
| 12 | P2 | 1 | 163 | 6.1 | **15.6** | 35.0 |
| 14 | P2 | 3 | 225 | 5.9 | **27.9** | 22.3 |
| 21 | P2 | 2 | 191 | 3.7 | **19.6** | 22.0 |
| 26 | P2 | 1 | 288 | 3.4 | **19.8** | 23.3 |
| 27 | P1 | 1 | 18 | 7.8 | 1.9 | **61.6** |
| 27 | P2 | 1 | 26 | 0.0 | 0.0 | 23.8 |
| 50 | P2 | 2 | 47 | 16.5 | 7.3 | 29.2 |

非警告試合の `recG` は大半が 0〜4（例外は 40 の 10.9）。
**8試合のうち6試合は `recG` が 15〜28** で、警告の有無とほぼ一致する。
`other` は全試合で 17〜29 の一定値（＝落下中ミノ・境界の常時ノイズ）で、
警告の有無を分けない。

### 結果2: P2偏重の正体
品質区間42件のうち**36件がP2**。上の表でも `recG` が大きいのは P2 に集中する。

### 結果3: 試合14を実映像と突き合わせた（決定的）
- `t=68.09`（動画744.82秒）のP2盤面: 実物は**ゴミ3行**＋右寄りの山、
  復元は**ゴミ8行**＋左寄りの山 → **観測が正しく、復元が誤り**（フレーム確認済み）
- 試合14 P2は `t≈24秒`（フェーズ54）から一貫して復元が 20〜70セル多い（2〜7行ぶん高い）。
  フェーズ54で観測は23セル消えた（＝2ライン消去）が、**復元は消していない**。
- `t=82.55` の復元盤面は `t=46.59` の観測盤面と一致率 0.95
  → 以降ほぼ追随できず、そのまま終盤までずれる。

### 結論（仮説の差し替え）
残る警告の主因は「キューの1手ずれ」ではなく、
**盤面の1〜2セルの読み違いがライン消去の成立を妨げ、その差が永久に残る**
「消去取りこぼしの累積」。これが「復元だけゴミ行が多い」形で現れる。
（キュー再同期パスより先に、こちらを潰す価値が高い）

### 実験E1（shift_threshold 0.60 → 0.85）
`shift_threshold` は 0.60 で、ゴミ上昇を「保持ブロックの60%一致」でも受理する。
これが偽のゴミ行挿入＝`recG` を生む一因と考え、**0.85** に上げて全56試合で検証した。

| 実行 | 警告 | 区間数 | 警告された試合 | 所要 |
|---|---|---|---|---|
| 基準（0.60・v48） | 8件 | 42 | 5,9,12,14,21,26,27,50 | 1,815秒 |
| **E1（0.85）** | **8件** | **44** | 5,**7**,9,12,14,21,26,50（27が消え7が出た） | 2,235秒 |

- 試合27（P1区間、`other`=61.6 の構造不一致）は**消えた**
- 代わりに試合7に新しい区間が3件出た（基準では0件）
- 総数は 42→44 で微増、所要は +23%
→ **採用せず**。`shift_threshold` は 0.60 に戻した。
結果は `frozen/runs/shift085-360p30-stride1-8warn-shifted/` に保存。

**示唆**: 試合27と7は「ゴミ上昇の受理しきい値」に対して逆方向に反応する。
単一のしきい値では両立しない＝`recG` の主因は「偽の上昇」だけではない
（E1で改善しなかった `other` 側＝消去取りこぼしが本丸という上の結論と整合）。

---

## 追記（2026-11-10 15:00）観測ボード再同期v54 → **不採用（不正な手筋だった）**

### 実装
`repairSuspiciousTrajectoryForPlayer` の最後に、深刻な不一致が残ったとき
いちばん悪いフェーズを「観測盤面そのもの」に固定（`manuallyFixed` アンカー）して
ビームを再探索するレーンを追加（`TR_RESYNC=1` で有効、既定OFF）。
ビルド `v54-resync`（`0F29599F43E9`）。

### A/B（マニフェスト8試合・360p30・stride=1）
| 実行 | 警告 | 区間数 | 所要 |
|---|---|---|---|
| control（v54・TR_RESYNCなし） | 7件 | 37 | 772秒 |
| **resync（TR_RESYNC=1）** | **6件** | **28** | 774秒 |

一見改善（試合12が9→3、試合27が3→0）。

### しかし出力が不正だった
`scratch/diag/jumpcheck.py` で全フェーズを検査すると、resync が採用された試合で
**「どの配置でもゴミ上昇でも説明できない盤面変化」が2箇所ずつ**発生していた。

| 実行 | 説明できない盤面変化 |
|---|---|
| control | 0 |
| resync | match_001:2 / match_003:2 / match_007:2（採用された3試合） |

つまり盤面を画像に**ワープ**させて一致率を上げただけで、合法手の連続になっていない。
しかも `recoveryUnresolved=0` のままなので、**現行の検証はこの不連続を検出しない**。

→ **不採用**。ソースは `git revert f680c0a2`、配備は v48（`d05916a22e15`）へ戻した。
`frozen/engines/0f29599f43e9_v54-resync.exe` は**診断用としてのみ**保存。

### 副産物（重要）
1. 強制的に盤面を画像へ合わせても、8試合中**6試合は警告のまま**残る。
   ＝残りの警告は「盤面がずれている」だけでは説明できず、
   区間の途中で**別の要因**（キュー・観測・消去判定）も効いている。
2. **未検出の不連続**が作れてしまうことが分かった。
   将来「盤面が説明なく変わるフェーズ」を `recoveryUnresolved` にする検証を入れるべき
   （品質警告を減らすのではなく、嘘の合法手を作らせないための安全装置）。

---

## 追記（2026-11-10 15:40）解像度仮説E3（360p30 vs 720p30）→ **効果なし**

「360p30は盤面セルが小さいから読み違えるのでは」という仮説を、
Lab動画2本で 720p（1280×720・30fps）に上げて検証した。
設定・エンジン（v48）・先頭12試合は360pのときと同一。

| 動画 | 360p30 | 720p30 | 差 |
|---|---|---|---|
| IWskrghK-gc | 7件（1,2,5,7,9,11,12） | **7件**（1,2,4,9,10,11,12） | ±0 |
| 4Qu6MEQegeg | 5件（7,8,9,11,12） | **7件**（2,5,6,7,9,10,11） | **+2（悪化）** |
| 所要（12試合） | 237〜521秒 | 742秒 / 1,022秒 | 約2倍 |

- 解像度を4倍にしても警告は減らない（片方は増えた）。
- したがって**盤面読みの空間分解能は主因ではない**。
- 過去の「720p60で0件」は、解像度ではなく**60fps（時間方向のサンプル数）**か
  別の条件（当時のini・NEXTモデル）による可能性が高い。
  ただし今回のLab動画は全て30fps配信で、60fps版は存在しなかったため未検証。

**結論**: 残る8件は入力画質では説明できない。リプレイ（キュー確定・盤面再構成）側の問題。





---

## 追記（2026-11-11 09:00）フォレンジック再調査：警告の起点は「HOLD誤読」が多数

### 前提の訂正（重要）
- 8件ベースライン（`goexit-360p30-stride1-8warn`）は **シーン `dc4c652e7ef3`（final-full）** で作られている。
  HANDOFF §4 / HANDOFF-FORENSICS §2(7) の実行例と `labsite_analyze.ps1` が渡す
  `tools/video-recovery-runtime/models/scene-classifier.onnx` は **旧モデル `7b62e3e24116`**。
- 旧モデルで v48 を再実行すると 53/56 試合の境界が動き 757/8254 手が変わる（警告 8→8、+7 −27）。
  **E1（shift 0.85）で見えた「27が消え7が出た」は、閾値ではなくシーンモデル差でほぼ説明できる**
  （E1 の出力と旧モデル再実行の差は 29 手）。E1 の結論は無効扱いとする。
- 正しいモデルで v48 を再実行すると **56/56 一致・0/8254 手変化**（エンジンは決定的）。
  記録: `frozen/runs/recheck-v48-finalfull-20261111/`、`frozen/REGRESSION-LOG.jsonl`。
- 以後の検証は `python tools
egress.py run ...`（`frozen/golden/README.md`）で行う。

### 方法
`scratch/regress/recheck-v48-finalfull-20261111/main56` を対象に、各警告区間から遡って
不一致セル数がノイズ床（4〜8セル）を超え続ける最初のフェーズを特定
（`scratch/diag/mcount.py`, `around.py`, `gstack.py`, `holdcheck.py`）。
フォレンジックページ: `review/forensics/v48-finalfull/`。起点は全て映像フレームで確認。

### 分類（映像で確認済みのもの）

| 試合 | 起点 | 型 | 映像で確認したこと |
|---|---|---|---|
| 9 P2 | #33 t=14.48 | **ピース識別：HOLD誤読** | 14.10s HOLD=Z → 14.30s HOLD=**I**。復元は HOLD=Z のまま（`holdUnclear=True` 14.04–15.97s）で、実際は Z を出して置いたのに I を井戸へ置いた |
| 26 P2 | #15 t=7.82 | **ピース識別：HOLD誤読** | 1316.5s〜 HOLD=**J**。復元は HOLD=T（unclear 継続）で J を置き、TSTを1フェーズ遅らせた → J の残骸 |
| 14 P2 | #23 t=9.22 | **ピース識別：HOLD誤読** → 後段で消去取りこぼし | 685.7s HOLD=**J**、復元は HOLD=L（unclear 8.0–9.3s）。TETRIS を 2+2 に分割。以前「t≈24秒の2ライン消去取りこぼし」とした現象はこの下流 |
| 27 P1 | #1 t=0.68 | **ピース識別：開始直後の初手欠落** | 観測は #1 から列2に縦I。init の active が Empty のため初回遷移は配置を作らない（`tetris_engine.cpp:1832 skipPlacement`）→ 最初の I が消え、#10 で後続の I を流用 → 列9の I が欠落 → #38 の T-SPIN Double を Single と誤認 |

### P2偏重の正体（新しい説明）
HOLD の CNN が「不明」を出す割合: **P2 31〜35% / P1 4〜5%**（NEXT先頭はどちらも1〜2%）。
不明フレームでは `stabilizeQueueCnnObservation`（vision.cpp:822）が直前の HOLD を流用するが、
`logicalQueueRuns`（recovery.cpp）は `reliableHold = true` 固定で、流用値を**実測と同じ重み**
（run長の log2、最大6 → ±24点）で採点していた。正しい経路が約48点負け、盤面の証拠が覆せない。

### 次の候補（未着手）
- 初手欠落（開始時点で既に出現しているミノ）: 8/112 プレイヤーで発生、警告になったのは 27 P1 のみ。

### 実験v55（`TR_HOLD_UNCLEAR_NOT_EVIDENCE=1`）→ **不採用（狙いの経路に効かなかった）**
- 変更: `logicalQueueRuns` で holdUnclear のサンプルを HOLD の証拠に数えず、run の重みも明瞭サンプル数で計算（既定OFF・ブランチ `engine/v55-hold-unclear-not-evidence` コミット `2a3079e3`）
- ビルド: `c7e3365f1a2b`（`frozen/engines/c7e3365f1a2b_v55-hold-unclear.exe`、配備せず、スナップショット記録済み）
- 全56試合・`--detect`・シーン final-full（`python tools
egress.py run --label v55-hold-unclear --env TR_HOLD_UNCLEAR_NOT_EVIDENCE=1 --no-deploy`）

| 項目 | v48 | v55 |
|---|---|---|
| 警告 | 8（5,9,12,14,21,26,27,50） | **10**（+38,43,45 / −50） |
| 一致した試合 | — | 22/56 |
| 変わった手 | — | **1,392/8,254** |
| 合法手で説明できない変化 | 0 | 0 |
| 所要 | 1,866秒 | 2,276秒 |

- **狙った3例（9 P2 #33、26 P2 #15、14 P2 #23）は1手も変わらなかった**。変化は開始直後（#1〜#2）など別の場所の副作用。
- 理由: 「Iをホールドして Z を置く」と「Iをそのまま置く」は NEXT の消費が同じで、**キューだけでは区別できない**。
  HOLD の証拠を弱めてもキュー復号は単純な経路（ホールドなし）を選ぶだけ。区別できるのは盤面観測だけだが、
  盤面ビームは各フェーズで「キューが決めたミノ1種」しか試さず、HOLD 状態もビームに持っていない
  （全ミノを試すのは修復パス `pieceIdentityFlexible` のみ、tetris_engine.cpp:1830）。
- 開始直後は空の HOLD 枠も「不明」と判定されており、その情報まで捨てたことが副作用の主因と見られる。
- 結果: `frozen/runs/v55-hold-unclear/`、`frozen/golden/v55-hold-unclear/`。

**次の仮説（設計変更が必要）**: HOLD が不明な区間では、盤面ビームのノードに HOLD 状態を持たせ、
「スワップした／しない」の両方を盤面で採点する（キューと盤面の結合復号）。

## 追記（2026-11-11 11:30）残り4試合の分類と、HOLD誤読の根本原因（観測側）

### 分類（全て映像で確認。証拠ページ: `review/evidence-hold/index.html`）
| 試合 | 起点 | 型 |
|---|---|---|
| 5 P2 | 17.4–19.0s | HOLD誤読（実物 I／エンジン T） |
| 12 P2 | 10.25s | HOLD誤読（実物 I／エンジン Z）→ 12.2s の TETRIS を取り違え |
| 21 P2 | 11.0–13.1s | HOLD誤読（実物 I／エンジン Z） |
| 50 P2 | 56.0s | HOLD誤読（実物 I／エンジン Z） |
| 5 P1 | 19.45s | 0.16秒で置かれた T がフェーズ分割から欠落（キュー上は T 区間 19.283–19.443 が存在） |
| 12 P1 | 25.62s | S で起きた2ライン消去を次の J に誤帰属 → 2セル差が残り #54 のダブル不成立 |

→ 警告8試合の P2 側の起点は**7件全てが HOLD誤読**（実物 I が5件、J が2件）。P1 側は3件とも別メカニズム（ミノ欠落2・消去帰属1）。

### 根本原因: P2 の HOLD 枠の I/J を CNN が分類できない
- エンジンと同一の前処理で `next_hold.onnx`（8bbb2479）を直接実行（`scratch/diag/hold_probe.py`、エンジンの holdUnclear と 24/27・17/18・15/16 一致）。
  HOLD=I の区間で**確信度 0.19〜0.21**（8クラスの一様分布は 0.125）。不明確率は 0.05 程度、暗画素率 0.60 前後
  → 「確信度 < 0.5」で不明になっている。
- 全56試合の集計（エンジンが最終決定した HOLD 値ごとの不明率）:
  P1: I 6.0%・J 9.4%・他 1〜2% ／ **P2: I 97.6%・J 97.1%**・他 27〜33%。
  P2 の HOLD が I と確定したのは全56試合で **82サンプルのみ**（P1 は 36,465）。
- 否定した仮説: 明るさ（暗画素率 0.6 ≫ 0.0794）、P2 枠が 3px 狭い（P1 幅にしても確信度 0.25〜0.29 で閾値未満）、
  学習データ欠如（`metadata.tsv` に P2 HOLD の I は 1,175 枚。ただし全て別動画 tetobo 由来）。
- P1 の HOLD の I も確信度 0.57 前後（閾値ぎりぎり）。モデル自体が HOLD の I に弱い。
- 下流: 不明フレームでは直前の HOLD を流用（vision.cpp:822）→ キューだけでは「I をホールドして Z」と「I を置く」を区別できず、
  盤面ビームは1種類のミノしか試さないため、誤ったミノで確定する。

### 修正方針
1. **観測（本命）**: HOLD 用の学習データに、この動画系の P2 HOLD（特に I/J）を加えて**別名で**再学習（ルール1）。
   ラベルは目視でなく論理で付ける（「操作中 X で別のミノ Y が盤面に置かれた ⇒ その時点の HOLD は X」）。
   評価は試合単位で分けた検証、全56試合 + Lab 4本（別動画への汎化）で golden 比較。
2. **復号（安全網）**: HOLD が不明な区間だけ盤面ビームに HOLD 状態を持たせ「スワップした／しない」を盤面で採点。
   1 で直らない残りと、将来の別の誤読に効く。設計変更なので 1 の後に。
3. 別メカニズム（P1 の3件）は 1・2 と独立に後で。

## 追記（2026-11-11 12:30）HOLD 誤読への対策：学習データ自動収集・新モデル・v56 修復レーン

### 配備中 NEXT/HOLD モデル（8bbb2479）の出自
- 元は `scratch/next360/final-v3/next_hold.candidate.onnx`（`tools/next-cnn/train_simple.py`、arch=dual, width=24, aug=2, 6 epoch）を
  `calibrate_next_onnx.py`（temperature 4.47 / unclear bias 4.2）で校正したもの。
- **学習データ 40,255 枚は全て NEXT 枠（HOLD 枠 0 枚）、全て 1 本の動画（`vs360p60.mkv`）由来**。
  HOLD は NEXT からの汎化だけで読んでいた。P2 HOLD の I/J が読めない直接の理由と考えられる。
- `tools/next-cnn/train_next_cnn.py` は旧世代アーキテクチャ用で、この配備モデルは扱えない（重み名が一致しない）。

### 学習データ自動収集（`tools/next-cnn/harvest_queue_labels.py`）
- 前提（オーナー確認済み）: 品質警告の無い試合は、終盤約10手を除き最終配置が正しい。
- HOLD ラベルはデコーダの hold/holdUsed 欄を使わず、「NEXT からの出現順」と「配置順」だけから推論
  （同じ試合でも P2 の hold/holdUsed 欄は誤っていることがあるため）。
  さらに「その HOLD のミノが後で実際にスワップされて置かれた」区間だけを採用（自己検証）。
- 収集元: ベンチマーク動画の警告なし 48 試合（警告 8 試合は除外）、Lab 18 本（360p）＋ 2 本（720p）。
- 結果 20,097 枚（`tools/video-recovery-runtime/data/next-cnn-training/harvest-20261111/`）。
  P2 HOLD の I 2,002 枚・J 1,206 枚を含む（旧モデルが正しく読めた P2 HOLD の I は 0 枚）。
- ラベル誤り推定: 旧モデルが確信度≥0.9 で反対する率 HOLD 0〜3.8%、NEXT 0.7〜6.9%。該当 91 枚は自動隔離、
  エンジンの明瞭な読みと食い違う 61 枚は人の判定待ち（学習不使用）。レビューページ `review.html`。
- 旧モデルの検証分（学習に使わない試合）での成績: P2 HOLD 正解採用 10.7%／不明扱い 89.3%、P1 HOLD 93.2%。

### v56（`TR_REPAIR_HOLD_UNCLEAR_LANE=1`）
- 盤面の異常を検出した後だけ動く論理修復レーンで、不明 HOLD を証拠にしない再復号を行い、
  通常経路と異なる候補（新たな選択経路を含む）を盤面で採点（既存 TrajectoryHealth ゲートで採否）。
- 配線は DeepSeek V4.1 Flash が仕様書から実装、Claude がレビューし候補比較の不具合（index 0 が評価されない）を修正。
- ブランチ `engine/v56-hold-unclear-lane`（bc3b331b）、ビルド `82bb4beabb97`（配備せず）。
- 1回目の全56試合は並行実行していた別のエンジンに VRAM を取られて全試合失敗（コードではなく資源）。GPU ジョブは直列化して再実行中。

### 新モデル v4-hold-v1（`run-v4-hold-20261111`）→ **不採用**
- `train_simple.py`（final-v3 と同設定: dual/w24/aug2/6epoch/lr0.002、CPU、ゼロから学習）。
  データ = next360 40,255 枚 + 収集 17,914 枚（学習分）。評価は収集データの検証用試合（学習不使用）と next360 の test。
- 第1位の正解率（閾値なし）: P2 HOLD 39%→**87%**、収集 NEXT 93.7%→95.0%、next360 test 96.5%→94.2%。
- しかし出力が鈍く、配備モデルと同じ校正（T=4.47）では 60% が不明扱い。T<1 で鋭くすると採用されるが、
  エンジンの採用規則（確信度≥0.5）での「採用したのに誤り」が P2 HOLD 0%→5〜11%、next360 P2 NEXT 3.0%→8.6〜9.8% に悪化。
  採用した誤読はデコーダがそのまま信じるため不採用。
- 次: 配備モデルの重みから微調整（元データも混ぜて忘却を防ぐ）、GPU で十分な epoch。
### 運用上の教訓
- エンジン（VRAM 約2.2GB・RAM 5GB を要求）と学習・評価を並行させると、エンジンの全試合実行が資源不足で失敗する
  （v56 の r1 は VRAM、r2 は RAM 不足で失敗。いずれも `regress.py` が失敗と判定し配備は無し）。重い処理は必ず直列に。

### v56 の全56試合（r3、単独実行）→ **判定保留**
- 警告 8→10（+38, 43, 45 / −50）、変化 914/8,254 手、検証器エラー 0。論理修復の採用 17→26 回（P2 8→16）。
- **v55 と警告の増減が完全に同じ（+38, 43, 45 / −50）**。仕組みの違う2つの変更で同じ差が出るのは不自然で、
  「現在のソースを再ビルドしたこと自体」の差（v48 の exe `d05916` は git 管理前のビルド）が混ざっている疑い。
- 対照実験として、現在のソースを全スイッチ OFF でビルドしたもの（`v57-control-defaults`）を全56試合で実行中。
  v56 / H1 はこの対照と比べて判定する（H1 は `--baseline v57-control-defaults`）。
### v57（`TR_QUEUE_HOLD_MODEL`）
- HOLD 枠（6枚ごとの先頭）だけ別モデルの出力に差し替えるフック。DeepSeek 実装・Claude レビュー（読み込み失敗の報告を追加）。
  ブランチ `engine/v57-hold-model`（bc61b0c7）。
- 第1世代 vs 第2世代（収集データの検証分）: P2 HOLD の I 95.7% vs 0.0%、J 99.4% vs 1.1%（正しく採用された割合）。
  一方 NEXT の「採用したのに誤り」は第1世代が多い（P2 6.2% vs 1.5%）。→ H1: HOLD だけ第1世代。
### 未解決メモ
- 試合5 P1 #34–35: キュー観測では 19.283–19.443 に「操作中 T・HOLD T」があるのに、フェーズ分割では L が2回置かれ T が消える。
  操作中と HOLD が同じミノのときの論理デコーダの扱いを疑う（未特定）。

### 試合5 P1「L が2回・T が消える」の原因（確定）
- DeepSeek の静的解析（論理キューデコーダでは S→L は作れない、盤面主導の identity 修復が上書きする）を、
  v48 再実行ログで確認: `queue_board_repair=1 player=1 attempt=3 ... corrections=7`
  `pieces_before=...OOSTL` → `pieces_after=...OOSLL`（severe 26→13）。NEXT は該当区間すべて明瞭・安定だった。
- つまり**盤面一致を上げるために、明瞭に読めていた NEXT と矛盾するミノ列を採用**している（合法手としては成立するため検証器では検出できない）。
- 全56試合で盤面主導 identity 修復の採用 107 回、うち**ミノ種類ごとの個数が変わるもの 59 回**。
  ただし開始位置（`-`）を埋める正当な修正（例 `{'I': +1}`, severe 78→0）を含むため、全てが不正ではない。
- 次の仕組み（v59 候補）: identity の書き換えを「そのミノの NEXT 観測が不明だった手番」に限る。
  あわせて検証器に「NEXT の出現順と置かれたミノ列の整合（HOLD 規則込み）」の検査を追加する
  （`harvest_queue_labels.py` の推論チェーンを流用できる）。

### 対照 `v57b-control-defaults`（現在のソースを全スイッチ OFF でビルド）→ **再ビルド差を確認**
- v48 基準（exe `d05916`）比: 一致 35/56、変化 706/8,254 手、警告 8→9（+38, 45 / −50）、検証器エラー 0。
- **現在の git ソースからビルドしたものは v48 exe と同じ結果にならない**（v48 は git 管理前のビルド）。
  v55・v56 の警告差（+38, 43, 45 / −50）のうち +38, 45 / −50 は再ビルド由来。v56 固有は +43 のみ。
- 以後の比較基準は `frozen/golden/v57b-control-defaults/`（`--baseline v57b-control-defaults`）。
### H1（v57b）→ **無効（フックが本番経路で動いていなかった）**
- 対照比 変化 4 手のみ、P2 HOLD の観測値が対照と完全一致。原因: 本番は NVDEC/CUDA 経路
  （`nvdec_cuda_preprocessor.cpp` の `inferCudaDevice`、パイプライン／適応バッチ）でキュー CNN を実行しており、
  DeepSeek に渡した仕様（「推論は2箇所」）がこの経路を見落としていた（仕様を書いた Claude の誤り）。
- 修正 `faf0e03c`: NVDEC 経路の2箇所で HOLD 枠を差し替え（適応バッチは元のスロット番号に戻して判定、キャッシュ格納前に適用）。
  H1 を `v58b-h1-hold-gen1` として再実行中。

### H1 やり直し `v58b-h1-hold-gen1`（HOLD 枠だけ第1世代 c0cd8801、NVDEC 経路修正済み）→ **大きく改善、ただし不採用（そのままでは）**
- 対照 `v57b-control-defaults` 比: 警告 **9→4**（−9, 14, 21, 38, 45、増加なし）、変化 2,608/8,254 手、**検証器エラー 1**、NEXT 整合警告 143→113。
- フックが効いたことの確認: `queue_hold_model_cuda=` が 58 回、P2 HOLD が I/J と読まれたサンプル 1,366→19,745、P2 HOLD の不明 約23,700→約3,950。
- 観測と復元の一致率（全フェーズ平均）: **P2 0.911→0.979**（0.9 未満のフェーズ 1,414→242）、P1 0.979→0.970（145→263）。
  プレイヤー単位で改善 33 / 悪化 4。悪化の主は試合26 P1（−0.122）: 第1世代が HOLD のスワップ演出中に 10ms だけ「J」と明瞭に誤読し、デコーダが追従。
- 検証器エラー: 試合51 P2 phase 1（0.32s）に配置なしで Z の4セルが現れる（ワープ）。対照では開始の操作中ミノが Z と推定されていたが、
  HOLD の読みが変わって開始の操作中ミノが不明になり、最初の遷移が配置を作らないのに盤面には Z が入った。
  → 開始ミノ（v58）の問題と同根。加えて「配置を出力せずに盤面だけ更新する経路」が既存コードにある（未特定）。
- 判断: 第1世代は NEXT/HOLD の「採用したのに誤り」が多く（検証分で P2 NEXT 6.2%）、P1 を一部悪化させる。
  本命は第2世代を HOLD データで微調整したモデル v2（job-v2.sh）。H1 は「HOLD を読めれば警告の大半が消える」ことの実証として記録。

### v58（`TR_OPENING_PIECE_FLEXIBLE=1`、開始直後の最初のミノ）→ **狙いは達成、副作用あり（判定保留）**
- 対照比: 警告 9→8（**−27（狙い）**, −38 / +42）、変化 1,089/8,254 手、検証器エラー 0、境界変化 2 試合（24, 47）。
- 一致率: **P1 0.979→0.987**（0.9 未満 145→62）、P2 0.911→0.909（1,414→1,445）。改善 12 / 悪化 5 プレイヤー。
- 変化の多くは「これまで落ちていた開始ミノを明示的に置く」もの（例 `-place → Z0@5,39`）。
- 悪化の主は試合42 P2（−0.144）: 開始は対照と同一だが #25 以降でミノ順が L,J,I→J,I,L に組み替わり、#29 に余分な I が入る。
  開始の柔軟化が直接起こす変化ではなく、盤面主導 identity 修復（試合5 と同系統）の挙動が変わった疑い。v59 で塞ぐ対象。
### v58c（`TR_OPENING_RESOLVE=1`）
- 解析後に phase 0 だけを空にする `forceRecoveryOpeningEmpty` が、phase 1 に配置なしのセルを残すバグ（H1 の試合51 P2 のワープ）を、
  そのプレイヤーの再解析で直す。DeepSeek が経路を特定、ログ `opening_state=51 valid=0 ... p2_occupied=4` で確認。コミット 7c210298。

### モデル v5（第2世代 final-v3 からの微調整、`tools/next-cnn/finetune_from_onnx.py`）
- ONNX（BN 畳み込み済み）から重みを復元し、初期出力が元と一致することを確認（最大差 1.5e-5）。BN は固定、HOLD 行を 3 倍で抽出、8 epoch、lr 3e-4、GPU。
- 検証分（学習不使用の試合）の第1位正解率: 全体 0.894→0.973、HOLD 0.493→0.918。
- 校正: T=4.47・2.0 では出力が鈍すぎてほぼ全て不明扱い。**T=1（unclear bias 4.2）を採用候補**に。
  `frozen/next_hold-v5-finetune-T1-20261111.onnx`（`6000fc02ee36`）として保存。
- エンジンの採用規則での成績（正しく採用 / 採用したのに誤り）:
  P2 HOLD I 100%/0%、J 99.4%/0%（第2世代 0%/0%・1.1%/0%）、P2 HOLD 全体 89.9%/8.9%（第1世代 87.7%/10.3%）、
  収集 NEXT 94〜95%/2.7〜3.1%（第2世代 81〜87%/0.4〜1.5%）、元の NEXT テスト P2 84.5%/5.2%（第2世代 93.1%/3.2%）。
  → HOLD は第1世代並み、NEXT は「採用したのに誤り」がやや増える。全試合で判定する（v5 全枠 / v5 を HOLD 枠のみ）。
### v59（`TR_REPAIR_RESPECT_CLEAR_NEXT=1`）
- 盤面主導 identity 修復で、元のミノが「手番開始前 0.5 秒に NEXT[0] として明瞭に読めていた」（明瞭サンプル 5 以上・80% 以上、
  CNN 自身の読みで判定）場合は書き換えない。DeepSeek 実装、Claude レビュー（証拠をデコーダ補正後の値から CNN の読みに変更）。
  ブランチ `engine/v59-respect-clear-next`（60e7fa52）。

### 組み合わせ `v58c-combo-h1`（HOLD=第1世代 + v58 開始ミノ + v58c 開始再解析）→ **現時点の最良**
- 対照比: **警告 9→2**（−9, 12, 14, 21, 27, 38, 45、増加なし）、変化 3,013/8,254 手、**検証器エラー 0**（H1 単独のワープは v58c で解消）。
- 一致率: **P2 0.911→0.982**（0.9 未満 1,414→185）、P1 0.979→0.979。改善 41 / 悪化 3 プレイヤー。
- 残る警告 2 件の原因はいずれも特定済み: 試合26 P1（第1世代の HOLD 一瞬の誤読）、試合5 P1（盤面主導 identity 修復の上書き → v59）。
- ビルド `53ff5af18c96`（ブランチ `engine/v58-opening-piece`、配備せず）。
- 次: HOLD を第1世代ではなく v5 にした場合、v59 を足した場合を全試合で比較中。

## 2026-11-11 v59 TR_REPAIR_RESPECT_CLEAR_NEXT — rejected (source 60e7fa52, exe sha 3c2afc9e04af, not deployed)
- run frozen/runs/v59-respect-clear-next, all 56, detect, vs v57b-control-defaults, gen2 queue model.
- warnings 9 -> 19 (+1,13,28,29,35,40,44,47,50,54; none removed), validator errors 0.
- observation agreement: P1 0.9785 -> 0.9751, P2 0.9107 -> 0.8614; players better 2 / worse 38.
- Reading: with the deployed gen2 model, "clear" NEXT[0] readings (>=80% clear samples in 0.5 s) are often wrong on P2,
  so keeping them blocks repairs that were right. The m005 P1 case is real but this guard is too broad. Not used in combos.
- v5 model runs same day: v5-model-full warnings 9 -> 3 (left 5,12,27), P2 agreement 0.986, validator 1 (m51 P2 opening,
  the v58c case, switch off in that run); v5-model-hold 9 -> 4 (+47), P1 agreement down -> v5 for all slots preferred.
- Caveat found the same evening: harvest auto labels contain systematic errors (human review fixed 21 of 58 checked,
  e.g. L->Z, O->T; one "easy" control L is an O). v5 was trained on unreviewed labels; retrain after review.

## 2026-11-11 combo final-v5-v58-v58c — best so far (exe v58c-combo-h1, model v5 6000fc02ee36, not deployed)
- env TR_OPENING_PIECE_FLEXIBLE=1 TR_OPENING_RESOLVE=1, v5 model for all queue slots; all 56, detect, vs v57b-control-defaults.
- warnings 9 -> 1 (left: match_005, the P1 identity-repair-over-clear-NEXT case), none added; validator errors 0.
- observation agreement: P1 0.9785 -> 0.9878 (phases <0.9: 145 -> 28), P2 0.9107 -> 0.9890 (1414 -> 65);
  players better 42 / worse 3 (worst m052 P1 -0.054).
- boundary changed m024 (+0.1 s start), m047 (+0.067 s start), same as v58 alone.
- Incident: stopping job-final.sh (TaskStop) did not kill its child regress.py, so a v59-including combo ran in parallel
  and shared scratch/final.log; it ended failed (21/56 matches, frozen/runs/final-v5-v58-v58c-v59) and is void.
  Determinism was confirmed earlier, but this run should be repeated alone before any deployment decision.
- Still pending before deploy: human/DeepSeek review of harvest labels and v5 retrain on reviewed labels.

## 2026-11-12 queue model v6 (frozen/next_hold-v6-reviewed-T1-20261112.onnx, sha 39f4c628308f, not deployed)
- data: next360 + harvest-20261111 after two human review rounds (frozen/next-dataset-2026-11-12-harvest-final:
  3,021 corrections, 546 excluded, 19,551 usable). gen2 (final-v3) fine-tuned 8 epochs, hold-boost 3, T=1 bias 4.2.
- auto-label root cause: harvest copied the engine's own carried HOLD, which is the stale-HOLD bug itself
  (e.g. IWskrghK-gc m7 P2: 61/64 HOLD crops show I while the engine kept T). Warning-free matches still had wrong HOLD content.
  Future harvests must not take HOLD labels from the engine's decoded hold.
- honest accuracy, out-of-fold by match (engine acceptance rule), harvest: P1 HOLD 98.8-99.8% ok, P2 HOLD 97.9-98.7%,
  P1 NEXT 94.7-96.3%, P2 NEXT 96.0-97.1%; weakest NEXT S (87-91%). v5 numbers on harvest are contaminated (trained on it).
- regression on next360 test P2 NEXT: v6 90.8% ok / 7.9% wrong (gen1 97.3/1.6, gen2 93.1/3.2). Errors are a few test
  matches with a red overlay bar at the slot edge: vs360p m50/m48 T read as Z, m54/m51 Z read as L
  (scratch/diag/next360-p2-confusion.png). v5 has the same weakness. To fix before deployment.
- 2026-11-12 correction (user): the next360 P2 frames with the red bar are the game-over animation (the board falls and
  covers the NEXT frame), right at the end of a match. Not a real-play weakness; ignore for deployment. Those test rows
  should be excluded from next360 evaluation.

## 2026-11-12 combo v6-v58-v58c — current best (exe v58c-combo-h1, model v6 39f4c628308f, not deployed)
- run alone (no parallel jobs), all 56, detect, vs v57b-control-defaults; 2062 s wall.
- warnings 9 -> 1 (left: match_005), none added; validator errors 0; boundary m024/m047 (same as v58).
- agreement P1 0.9785 -> 0.9886 (<0.9 phases 145 -> 25), P2 0.9107 -> 0.9893 (1414 -> 65); players better 42 / worse 2
  (worst -0.020 m001 P1). vs v5 combo: P1 28 -> 25 low phases, m052 P1 regression of v5 gone.

## 2026-11-12 combo v7all-v58-v58c (model v7-all eeadceb066e4 = v6 recipe on every reviewed image incl. next360 val/test)
- all 56, run alone: warnings 9 -> 1 (match_005), validator 0, 1458 s wall (v6 run 2062 s; load variance not excluded).
- agreement P1 0.9878 (<0.9 phases 29), P2 0.9896 (65); vs v6 combo: better 1 / worse 1, m052 P1 -0.059
  (same regression as v5). v6 stays the preferred candidate; m052 P1 to be checked on video.

## 2026-11-12 speed profile (single worker, timestamped stderr; scratch/prof/)
- control v57b, matches 9+47 (147 s of video): 301 s. Per match: GPU queue scan + board ~5-8 s, first beam pass <1 s;
  the rest is repair loops (queue_board_repair ~13 s per attempt, trajectory_backtrack beam 32->256 ~22 s,
  queue_same_order_wider_beam ~1-2 s) and the opening guard re-running the WHOLE match (same repairs again, 55-72 s).
- v6 + v58 + v58c, same 2 matches: 37.5 s (match 9 122 -> 32 s, match 47 178 -> 5 s). Correct HOLD/NEXT readings stop
  the repair loops and the guard reanalysis from firing. Full-run counts: queue_board_repair 106 -> 18,
  trajectory_backtrack 53 -> 8, wider_beam 71 -> 16, opening_guard_reanalyze 3 -> 0.
- Wall times of 3-worker full runs are noisy (scene stage 27 / 204 / 378 s in three runs); compare speed with
  single-worker profiles or on an idle PC.
- Remaining cost in match 9 (v6): two queue_board_repair attempts (~16 s + ~8 s) = most of its 32 s.

## 2026-11-12 speed: batch worker count (v6 combo, results identical 56/56 in every run)
| workers | wall (all 56) | note |
|---|---|---|
| 3 (default) | 1,476-2,418 s | GPU queue scan 5 s -> 230-740 s per match; free VRAM ~130 MB with 2 active |
| 1 | 597 s | scene 25 s |
| **2** | **402 s** | 6 min 42 s, faster than the 2026-11-05 build (463 s) |
- cause: scheduler books TR_BATCH_WORKER_VRAM_MB=1400 per worker, real peak ~2.5-3 GB (scene overlap ~4 GB), so 3 workers
  oversubscribe the 8 GB card and CUDA thrashes. Earlier "slow engine" wall times were mostly this, plus repair loops
  that v6 removes.
- tools/regress.py now sets TR_BATCH_WORKERS=2 by default. Production script tools/run-ppt-stream-analysis.ps1 still sets
  TR_BATCH_WORKERS=3 / VRAM 1400 -> change pending the owner's decision.

## 2026-11-12 match 5 (v6 combo) root cause, video-confirmed
- P1, phase 34 (match-relative 19.28 s): NEXT read clearly as TLIJO and active T (0.17 s, fast drop), but the decoded
  timeline has active L and later places an extra L; the T at columns 4-5 is never placed (4 observed-only cells persist).
- Consequences: the T-spin double (~22.4 s) and T-spin triple (~27.9 s, placed with 0 lines) are mis-explained; the stack
  stays 3 rows too high; the TETRIS at ~34.1 s is reconstructed as a 2-line clear -> warning at phases 61-62.
- Same mechanism v59 targeted (board-driven identity repair overriding a clearly read NEXT). v59 failed with gen2 because
  its "clear" readings were often wrong; retest queued with v6 (label v6-v59-respect).
- Also started: v60 (branch engine/v60-gpu-memory, 06dd291c) opt-in TR_CUDA_* ORT memory settings, built, not deployed.

## 2026-11-12 match 5 deeper root cause + attempts
- v59 retried with v6 (frozen/runs/v6-v59-respect): warnings 1 -> 3 (+9, +44), match 5 still warned, agreement worse
  (players better 0 / worse 6). Rejected again.
- Real origin (video + single-match runs, scratch/prof/m5*): at GO, P1 hard-drops Z within 0.05 s while T is already
  falling; the pre-GO NEXT shows Z T J L S, the first in-match queue frame already shows NEXT JLSOI. Two pieces lock before
  the first transition, the timeline has room for one, so the Z is lost. The first board repair then re-labels 8 pieces
  (phases 0-27, e.g. Z->I at phase 15) and the second rewrites the clearly read T at phase 34 to L. Without board repair
  (TR_DISABLE_QUEUE_BOARD_REPAIR=1) identities stay correct and the match takes 15 s instead of 67 s, but the missing Z
  still breaks the T-spin triple at ~6.45 s and the warning remains.
- v61 prototype (branch engine/v61-opening-two-pieces, TR_OPENING_TWO_PIECES=1, off by default): insert a second flexible
  opening step without board evidence when the first board shows two pieces (7-10 cells). The step is inserted, but the
  beam chooses "no lock" for the evidence-free step (greedy per step), so the repair still fires; match 5 got worse
  (warning span 31 phases). Not usable as is.
- Next idea: take the opening identities from the NEXT window read just before GO (Z T J L S here) instead of flexible
  steps, so the two locks have fixed pieces; needs the queue scan to include the pre-GO frames.
- 2026-11-12 correction (user): pieces are controllable only after GO ends. Match 5's frames 4939-4942 are gameplay that
  the scene classifier mislabelled as "40_go"; the real GO end is earlier. The fault is the scene GO label, not the
  start rule and not the engine opening logic. The "start earlier than GO exit" idea is dropped. v61 (opening two pieces)
  is abandoned. Also: matches without a quality warning are to be trusted as correctly analysed.

## 2026-11-12 Lab 4Qu6MEQegeg match 11 P2 warning (v6 combo): root cause, video-confirmed
- Video (review/lab-4Qu-m011/opening-p2.png): HOLD empty at start, L spawns ~0.18 s and is held at ~0.23 s, I spawns.
- Engine: HOLD crop at t<0.23 s unreadable (v6: I 0.35 < 0.5 -> holdUnclear); the opening HOLD fill-in
  (holdCorrected=true) then assumed a GO piece already held and filled it with O (bag elimination of LISTJ). The
  timeline releases that phantom O at phase 2 and places it at the bottom left; those 4 cells never exist in the
  video, the board drifts and breaks at phases 92-99.
- Same crop: gen1 reads empty HOLD at 0.99, gen2 T 0.18 (unreadable), v6 I 0.35 (unreadable). v6 (and gen2) never
  learned the empty HOLD: the reviewed harvest has no empty-HOLD samples.
- The fill-in happens at the start for 17-18 of 112 players on the benchmark (control and v6 alike) and 7 of 26 here;
  those are warning-free and taken as correct (instant hold of the GO piece).
- Fix proposal: add rule-certain empty-HOLD crops (frames before the first hold of each player at match start) to the
  training data so the empty HOLD is read clearly.

## 2026-11-12 training-data prep (for human review)
- Empty HOLD: tools/next-cnn/collect_empty_hold.py -> data/next-cnn-training/empty-hold-20261112, 19,094 crops (label 0),
  rule-certain: READY..GO end of every detected match of the benchmark + 18 Lab videos (18,634) and post-start frames
  before the first hold use in analysed matches without a start fill-in (460). Model reads on this set:
  gen1 empty 19,046 / piece 41 / unreadable 7; gen2 unreadable 19,057; v6 unreadable 19,052 -> gen2 and v6 cannot
  read an empty HOLD at all. Review in next_editor (port 8781), gen1 disagreements: 48.
- GO boundary: dataset-go-boundary/ (scene classes, 640x360 jpg), [yD_ZukVrgHY] match 5 frames 4898-4972 with the
  final-full classifier labels as proposals (READY 9, GO 32, analysis_start 7, gameplay 27). Review in
  tools/ppt-train/dataset_editor_server.py (port 8771).

## 2026-11-12 scene model v2b (frozen/scene-classifier-v2b-goboundary-20261112.onnx, sha ffe9c87dca13, not deployed)
- Found: tools/ppt-train/train_model.py read images with cv2.imread, which fails on non-ASCII paths on Windows;
  9,602 of ~30k images (Japanese stream titles) were trained as all-black frames, also in the deployed final-full
  (its log shows the same warnings). Fixed with np.fromfile + cv2.imdecode.
- v2b = loader fix + dataset-scene-v2 (dataset-balanced + user-reviewed match 5 GO boundary x8), spatial-big, 12 epochs, --no-val.
- 56 matches with v6 queue model (frozen/runs/v6-scene-v2b): warnings 1 -> 0 (match 5 fixed, start 164.767 -> 164.500),
  validator 0, 255 moves changed, 387 s. Boundaries moved in 50 matches (start -0.37..+0.70 s, end -0.6..+0.8 s);
  no match lost a placement (later starts only trimmed idle time before the first piece), match 38 P1 regained its
  opening piece (41 -> 42 placements).
- queue v7b (frozen/next_hold-v7b-emptyhold-T1-20261112.onnx, sha b32435fa64aa): v6 data + 19,030 reviewed empty-HOLD
  crops as clear class '-', --keep-last (v7 kept an early epoch and never learned empty: 1.8%). Empty HOLD 100%,
  pieces read as empty 0, next360 test NEXT ok 0.959.

## 2026-11-12 combo v7b + scene v2b (+ v58/v58c) - current best, not deployed
- benchmark 56 (frozen/runs/v7b-scene-v2b): warnings 0, validator 0, 300 s wall (2 workers); vs original control
  v57b: P1 0.9785 -> 0.9905, P2 0.9107 -> 0.9898, players better 43 / worse 0.
- Lab 4Qu6MEQegeg first 13 matches (scratch/labreview/4Qu6MEQegeg-v7b): warnings 1 (m11) -> 0, validator 0, 108 s.

## 2026-11-12 Lab full-video run 1: FMhM5akSQWk (しゃけごはん配信, frozen/runs/labfull-FMhM5akSQWk)
- setup: v58c-combo-h1 + queue v7b + scene v2b, 2 workers; 169 matches detected (public Lab data: 100), 764 s,
  9 warnings (12,16,32,49,51,52,57,80,83), 52 validator errors.
- Scene v2b regressions (all video-confirmed):
  * 68 false matches (1.4-27 s) after 8,631 s, where the stream switches to another Switch game (lobby screens);
    the old final-full detected none there. Match 1 (40.5-116.6 s) is also outside the public match list.
  * GO ends too early in this stream: v2b gives GO 0.1 s where the old model gave 0.6 s (m49/52/57/80/83 start
    ~0.47 s earlier; review/fm80-start.png shows the big "ゴー!" text still over both boards at the new start).
    The board CNN reads the GO text/streak as cells: Group A (m12,16,32,51,57) first board sample read as 85-101
    occupied cells, opening guard finds no empty run, phase-1 warp -> validator errors (PLACEMENT_FLOATING/
    UNREACHABLE, BOARD_UNEXPLAINED) and phase-1 warnings; Group B (m49,52,80) 11-14 phantom cells in phases 1-5
    lead to a misplaced opening piece (rec-only cells persist) and mid-match warnings.
  * Likely cause: the GO-boundary data came from one stream (vsたわ match 5, x8 weight), so v2b learned
    "GO ends early" and over-applies it to streams whose GO text lasts longer.
- m83 P2 (phase 43+): observed garbage rows missing from the replay (obsG=17) -> garbage rise not detected (engine).
- Decision needed before continuing: v2b should not be used on other streams as is.

## 2026-11-12 scene v3 (restored unrelated / other-PPT data) — not deployed
- Data: dataset-scene-v3 = dataset-balanced (final-full data) + dataset-go-boundary x1 + dataset-unrelated-restore
  (user-reviewed: 4,116 00_unrelated incl. Kirby Air Riders part of FMhM5akSQWk from 8736.2 s, 1,043 10_ppt_other
  re-extracted from the old interval projects; near-duplicates dropped by dHash). Same recipe as final-full.
  Model frozen/scene-classifier-v3-unrelated-20261112.onnx (55efd493f8e0).
- vsたわ 56 (v7b queue, v58c-combo-h1): warnings 1 (match_031 new, match_005 fixed), validator errors 0. match_031 not yet investigated.
- FMhM5akSQWk full: 100 matches (= the 100 public ones; the 68 false Air Riders matches and the stray match 1 are gone),
  warnings 11, validator errors 47, 749 s.
- GO still ends early on this stream: 89/100 starts are >=0.3 s earlier than Lab (median -0.467 s).
  Cause (per-frame dump, match 16 at 1283.3-1284.4 s, review/fm16-go-frames.jpg): the GO text sweeps over P1's board
  and single frames inside the GO animation (1283.633, 1283.933) are classified READY by every model
  (final-full, fixed-loader, v2b, v3). go-exit takes the first non-GO frame, so the start lands mid-GO
  (1283.633 instead of ~1284.03). Not caused by the x8 weighting. Fix candidates: GO-animation frames from many
  streams labelled GO (user review), or an engine rule that bridges 1-2 frame gaps inside a GO run.

## 2026-11-13 夜間実験: GO/終了のすき間つなぎ（v62/v63、すべて opt-in、未配備）

追加したスイッチ（既定はすべて0＝従来と同一。v62 制御実行で vsたわ56 の全手が v7b-scene-v3 と一致）
- `TR_GO_BRIDGE=N`: GO の連続に N フレーム以下の非GOが挟まっても同じ GO とみなす（v62, c6b1b24f）
- `TR_START_OFFSET_MS=X`: go-exit の開始を X ms ずらす（負のみ有効。正は下流の adaptive start で戻される）
- `TR_SCENE_DUMP=path`: 全フレームのシーン判定を TSV で書き出す（診断用, v62b）
- `TR_END_BRIDGE=N`: 結果画面の連続に N フレーム以下の非終了が挟まっても切らない（v63）

結果（シーン v3、キュー v7b、置き方＝piece,x,y,rot,garbage の列で比較）
- GO_BRIDGE=2: vsたわ56 の置き方は制御と全一致（開始だけ GO 終了直後へ移動）。FMhM 警告 11→6。
  FMhM では v3 単独で 19 試合の P1 序盤が 3〜4 手 skipped（うち 11 試合は警告なし＝見えない誤り）。bridge で 17 試合解消。
  GO_BRIDGE=1 は vsたわ で 2 と同一、4 は試合33 の序盤が変わる → 2 を推奨。
- START_OFFSET -100ms: 単独だと vsたわ 試合42 の P1 序盤が壊れる（警告なし）。原因は「ゴー!」の文字が NEXT を隠す区間に開始が入り、キューを誤読（review/m42-start-frames.jpg）。
  bridge2 と併用で vsたわ は同一だが FMhM で 13/51/64/79 の序盤が skipped に → 不採用。
- vsたわ 試合31（v3 で新規警告）の原因: 結果画面の連続中に 1 フレーム（1667.867）だけ gameplay と判定され、終了が 1667.7→1667.9 に遅れ、傾いた盤面を解析（review/m31-end-frames.jpg）。
  GPU 前処理だと 1667.700/.733 も gameplay（CPU 前処理と判定が違う）。同様の「結果画面中の1フレーム gameplay」で終了が 0.1〜1 秒遅れる試合が多数。
- END_BRIDGE=1/2/4（GO_BRIDGE=2 併用）: vsたわ 警告 1→0（試合31解消）、変化は最後の 1〜4 手のみ（試合43 の結果画面中のゴミ手 3 つが消える）。FMhM 警告 6→5（82 解消）。→ 2 を推奨。
- 360p 画質の検証: FMhM の警告試合を 1080p60 で取り直し（scratch/fmhd）。警告が消えたのは 82 のみ、15/31/50/56 は残る → 主因は画質ではない。
  残る警告はどれも「おじゃまが天井近くまで積み上がった REN 合戦」中の長い不一致（review/fm15-hd-p2.jpg）。次の調査対象。

5配信（GO_BRIDGE=2、シーン v3）
- FMhM5akSQWk: 100/公開100、警告 11,15,31,50,56,82（END_BRIDGE 併用で 82 消える）
- J-upNUb6kwE: 121/公開120、警告 18,83,102。余分な試合1 はタイトル画面のデモ（COM同士のぷよ）の「ゴー」を GO と誤判定（シーン分類）
- 5dPPsUOrcpE: 39/39、警告 0、検証エラー 0
- GXr4f0dXrow: 27/27、警告 0、検証エラー 0
- roDwsl-ppJo: 144/公開131、警告 26,32,90,137。余分 14 のうち 2 試合はテトリス vs ぷよ（公開は除外）、残りは通常のテトリス試合（公開側に無いだけ）。
  公開にあって無い 1 試合は Switch のみまもり設定で試合途中に打ち切り（結果画面なし→終了が決まらず捨てられる）
- 残る警告（天井近くまでおじゃま＋REN）の原因候補（DeepSeek のコード読み、未検証）:
  1. `src/recovery.cpp:1487` cleanUpBoard(): 「おじゃまを含まない一番下の行」より上のおじゃまを全部消す。REN の消去フラッシュでおじゃま行が1行でも読めないと、その上のおじゃまが全部消える可能性。
  2. `src/tetris_engine.cpp:158` hasExactVerticalCluster(): せり上がり後の色ブロックが可視20行内（targetRow>=20）に収まる場合しか一致させない → 天井近くの盤面では正しいせり上がり仮説が捨てられる可能性。
  3. 消去アニメの猶予が 0.6 秒（`src/recovery.cpp:7368`）で、長い REN のフラッシュ区間を覆えない。
- v64 `TR_NO_GARBAGE_CLEANUP=1`（cleanUpBoard の浮きおじゃま消去を止める, opt-in）:
  - FMhM 1080p60 クリップ（GO/END bridge 2 併用）: 警告 15・31 が消える。11・50・56 は残る。警告なし 20・60・82 は警告なしのまま。
  - vsたわ56（GO/END bridge 2 併用）: 警告 0、置き方は v63-v3-gob2-endb2 と全一致。
  - 360p の配信全体（FMhM/いんたい/あちみつ）では警告は変わらず（置き方の変化も 0〜2 件）→ 効くのは高画質のときだけ。
  - 360p+bridge 時の見えない序盤 skipped: FMhM 0、いんたい 1（113）、あちみつ 1（92、GO 後なので別原因）。
- いんたい配信の処理時間が 758 → 1162 秒（同条件の再実行で差、原因未確認）。

## 2026-11-13 Queue-move start experiment (v66-start-override, d861e077, not deployed)
- Prototype `scratch/diag/qstart.py`: diffs 8 uncovered slots (P1 HOLD/N1/N2, P2 HOLD/N1-5) against the first GO frames; start = first frame with >=3 slots >15. Found in 156/156 matches (main56 + FMhM100); P1/P2 same frame 155/156; pre-shift noise max 15.2.
- Current go-exit start is 3-8 frames BEFORE the queue move (never after); the gap is 3-5 when GO is detected for 16-19 frames, 7-8 when 13-14 frames (FMhM); main56 m7 GO=3 frames -> 25 frames early.
- TR_START_OVERRIDE_FILE runs vs v65: main56 shift+0 changes 15 matches, -1 changes 8 (+4 validator errors m19/m53), -2 changes 4; all 0 warnings. FMhM -1 and +0: warnings unchanged [11,15,31,50,56], m15 becomes needs_correction.
- Conclusion: type-A warnings are not caused by the start position; the engine prefers starting a few frames before the first spawn.
- Memory (v65 ARENA_SAME, main56): 1 worker 3,803 MiB/464 s; 2 workers 6,695/275; MEM_LIMIT 1024 6,637/451; 512 6,659/426; 3 workers 7,889/968.

## 2026-11-13 Type-A cause found: one-pass board repair served requests out of time order (v67-repair-sort, not deployed)
- Symptom: FMhM 360p 11/15/31/50/56 `opening_state valid=0` (first board 52-160 cells, HOLD set); forceEmptyOpening skipped because both players looked like mid-game stacks; solve started from that stack (bogus line clears in the first 5-10 moves).
- TR_BOARD_PREFETCH_DIAGNOSTICS (m15 clip): the 3 phase-0 requests at 15.036 s were served by the 62.57 s frame (P1) and 51.7 s frame (P2); all other requests within 0.1 s.
- Cause: `repairMissingBoardRequests` (one-pass repair) keeps missing requests in arrival order, but its serve loop assumes time order and hands every request <= the current frame time to that frame. An opening request queued behind a later one was answered by the later cluster's frame.
- Fix (commit "sort one-pass board repair requests by time"): stable_sort pendingP1/P2 by time; TR_ONEPASS_REPAIR_UNSORTED=1 restores old behaviour.
- Clips m15/m31: opening now valid (0 cells), warnings move from 0.3-0.5 s to mid-match (16.7 s, 18 s); m50 opening still 32 cells (separate). No-garbage-cleanup does not change these at 360p.
- Opt-in diag kept: TR_OPENING_STATE_DIAG=1 (first sample times / identical later boards). A 0.25 s max-lag guard was tried and reverted (never fired; wrong path).
- Full validation queued: scratch/day4.sh (FMhM/J-up/roDwsl full + all,labhalf); memory breakdown queued: scratch/day5.sh.

## 2026-11-13 v68b-garbage-alt-stage2 (not deployed)
- J-up 18 (P2) cause: 5 garbage rows arrived (top 2 hole col 6, bottom 3 col 5); the I cleared the top 2 at once under the clear flash. Engine read 3+3 rows with col-5 holes, I one column off; the 2-cell error stayed buried until a 7-REN dig at 37-39 s.
- The garbage retry tried alternate rise counts with one hole guess each. v68 enumerated hole variants for those counts: fixed J-up 18 but +14% on main56 (299 -> 342 s, same exe A/B).
- v68b runs the variant search only as a second-stage retry when the ordinary garbage retry leaves the player unhealthy (TR_GARBAGE_ALT_HOLES=0 disables). J-up 18 clip: warning 0 (severe 33 -> 0). main56: 297 s vs v67 293 s back to back, 0 warnings, stage never fired.
- Quick check (v68): main56 0, half J-up 18 fixed, r8NZ 6/12, yeiP 24/37/38, zeb 12 unchanged (different cause).

## 2026-11-14 v69-refine-keep-identity (c76d31fa, not deployed)
- Remaining-warning triage (half suites, v68): yeiP 37 and 38 are video defects (stream freeze then jump: 38 frozen 2 s right after GO, 37 has 13 freezes 0.3-1.9 s; `scratch/diag/freeze_scan.py`). r8NZ 6, r8NZ 12, yeiP 24 and zeb 12 have no freeze.
- Re-encoded clips do not reproduce these (r8NZ 12 clip took a different repair path); use the original video with a manifest of only the warned matches (`scratch/a360/runm.sh`, `warn-*.json` from segments.matches.json). v68 reproduces r8NZ 6/12 exactly.
- r8NZ 12 P1 cause: the flexible opening step picked T instead of "no lock" (the GO queue slide was counted as a lock), adding a phantom T. The first-pass queue board repair removed it (severe 11 -> 0), but order refinement rebuilt the raw timeline from NEXT and restored it; the later repair window (anomaly 32, lookback 18) could not reach phase 10 and rolled back.
- Fix: when refinement's phase layout matches, keep the first-pass piece identities and take only the denser board evidence (TR_REFINE_KEEP_IDENTITY=0 restores). r8NZ 6/12: warnings 2 -> 1 (12 fixed), 302 -> 162 s. Phases 3-10 still show the 4-cell opening phantom (below warning level).
- main56: traces identical to v68b in all 56 matches, 0 warnings. Back to back same exe: keep=0 447 s (sum 735, loaded) then keep=1 304 s (sum 463); v68b was 297 s. 4-match ABAB: main56 m5 74-81 s -> 95-108 s because refinement now succeeds instead of falling back; others unchanged.
- Still open (engine): r8NZ 6 (board breaks just before match end), yeiP 24 (L read as I right after a perfect clear), zeb 12 (queue phases collapse under a 3-line clear flash).

## 2026-11-14 v72-partial-compare-only (39c50e11, not deployed)
- yeiP 24 P1 cause: the first solve misplaced a piece (severe 116 from phase 15). The board-driven queue repair ran first and rewrote correct identities Z,L -> I,Z (116 -> 91), adopted as a "safe partial gain"; the ladder then only reached 73. With TR_DISABLE_QUEUE_BOARD_REPAIR=1 the ladder on the original identities reaches 116 -> 1 and the warning disappears.
- Fix (6cf4aedb): when a queue repair is adopted only partially (anomaly remains), run the trajectory ladder on both the repaired and the original identities; switch to the original identities only when strictly healthier, else restore the partial repair exactly. TR_QUEUE_PARTIAL_COMPARE=0 disables. yeiP 24: warning 1 -> 0, pieces 22-25 O Z L J (matches video), 37 -> 81 s.
- PERFECT CLEAR letters: the 24-cell L-majority detector missed yeiP 24 (23 cells in phases 22-23). v70/v70b (erase L at row >= 12 for 2.5 s after a solved perfect clear; raw cleanup + per-path beam scoring) were tried, did not change yeiP 24 and flipped main56 m47 P2 phases 27-28 (L,T -> T,L; video shows L then T-spin double) through the re-solve. Reverted (0cb66bde, 39c50e11).
- main56 back to back: v69 429 s (sum 727) vs v72 473 s (sum 810), 0 warnings both; traces identical except m5 P1 (8 moves; opening phantom Z removed, phases 2-13 now match the video). The whole time difference is m5 (120 -> 204 s); the other 55 matches sum 607.0 vs 605.4 s.
- Remaining-warning causes (evidence in review/warn3/): r8NZ 6 and zeb 12 = NEXT window read in a mixed slide state accepted by the queue decoder (r8NZ 6: a compression-starved 10-frame blend right after a 49 KB keyframe, decoder resync advance=5; zeb 12: one blended frame, 1-slot tolerance kept the wrong T, resync at 27.01 inserted 5 phases).

## 2026-11-15 v73b-queue-jump-cap (3524165f, not deployed)
- Queue advance statistics over 967 matches (~172k decoded NEXT transitions; `scratch/diag/advance_stats.py`): advance 1 = 171,742, advance 2 = 36 (real, e.g. HOLD plus lock right after), advance 3-5 = 33, all from video defects (yeiP 37/38), the r8NZ 6 error, stream cuts (69 s jumps) or in-place tail corrections. No real play shows 3+.
- Fix 1: a stable resync may jump more than 2 pieces only when the stable gap covers 0.4 s per piece (TR_QUEUE_RESYNC_MAX_ADVANCE=2, TR_QUEUE_RESYNC_SECONDS_PER_PIECE=0.4). r8NZ 6: the 10-frame blended read (advance 5 with no overlap check) no longer inserts 5 phantom placements.
- Fix 2: a NEXT run of <= 4 samples (one 30 fps frame) is not evidence: keeping the parent costs nothing (TR_QUEUE_SHORT_RUN_SAMPLES=4; 2 was not enough because zeb 12's blended run lasted 3 samples).
- Results: r8NZ 6 and zeb 12 warnings 1 -> 0 (36 -> 7 s, 30 -> 7 s). Full 23 suites: warnings only yeiP 37/38 (video defects). Versus v72: main56 identical; 11 changed matches, 9 only lose 4-5 phantom placements after the match end; uRZn 4/63 identical placements (phase numbering only); roDw 3 P1 changed only after an already broken board (below).

## 2026-11-15 v74c-hole-lookahead (46572867, not deployed)
- roDw 3 P1 (no warning, found by the v73b comparison): a 7-row rise at 16.92 s was first seen mid-animation (`GG_______G`), so the hole of that row was a 10-way tie; the alternate-count path took the first empty column (2) instead of 7. The 2-cell error stayed buried 22 s; at 39.62 s the player's TETRIS in column 7 cleared only 2 lines and the last 8 placements were wrong in both v72 and v73b.
- Scan for persistent buried hole mismatches (`scratch/diag/hole_scan.py`, >= 6 phases) over all 23 suites: roDw 3, r8NZ 4 and yeiP 37/38 (video defect) only.
- Fix: for an obscured garbage-like row, follow it into the next 8 raw frames, anchored on every exact garbage row around it (unique upward shift allowed for a further rise), and use its hole when >= 2 frames show it exactly and none disagrees; applied to both the primary and the alternate-count rise variants (TR_GARBAGE_HOLE_LOOKAHEAD=0 disables). roDw 3: summed mismatch 799 -> 439 cells, phases >= 10 cells 18 -> 7, the TETRIS now clears 4 and the ending matches the video. Evidence: review/v73check/ro3-hole-report.jpg.
- r8NZ 4 P1 unchanged (different cause): the 1-row rise at 29.89 s was taken from a frame before the rise was drawn, so the old bottom row was read as an exact (certain) hole; 2 cells stay wrong to the end, no breakdown.

## 2026-11-15 v75b-shift-stack-veto (a4d5ae73 + ffe9ffaf, not deployed)
- r8NZ 4 P1 real cause (review/r84/REPORT.md): HOLD showed T (magenta) from ~29.4 s but was read as Z, so the engine placed T first; T filled the top garbage row's hole and cleared it ~1 s early; the board offset was compensated by a fake 1-line rise (ratio 0.57) copying the old bottom row's hole (col 2, holesCertain). The real 2-line rise at 30.90 s (both col 6) was then counted as 1.
- Fix: a garbage rise found only by the coloured-cell correlation (ratio < 0.8) is vetoed when both boards carry an exact bottom hole stack (>= 2 rows) and the offset makes >= 2 retained rows disagree. Applied in sourceBoardShift (v75) and in the backtracking alternate-count path (v75b; the r8NZ fake rise came from there). TR_SHIFT_STACK_VETO=0 disables.
- r8NZ 4: order now Z (29.89) -> T + 2-line rise col 6/6 (31.29), matching the video; persistent hole mismatch gone; bad phases 21 -> 14.
- Tested only matches that contain an accepted rise with ratio < 0.8 (36 rises, 29 matches incl. main56 1/28; scratch/v75/run_subset.py, manifest mode, v74c same subset as baseline): 0 new warnings; other changes only in the last 1-3 phases of top-out (roDw 35 P2, roDw 38 P1, IWsk 14 P2) and in the video-defect matches yeiP 37/38. Full 23-suite run not done.
