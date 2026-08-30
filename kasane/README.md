# KASANE

KASANE は、相手盤面と時間を読む対戦用テトリスAIです。最初のマイルストーンとして、独自の4手ビーム探索による火力基盤と、Cold Clearで相手の着火時刻を予測する相殺外しを実装しています。KASANE-Base自身の合法手探索・評価値はCold Clearから独立しています。Guard / Strategy v2 / Stack-REN v3は事故時の性能下限を守るため、同じ探索予算のCold Clear候補を安全床として取得し、KASANE独自の予測・NN・状態戦略が十分に有利な時だけ上書きします。

現在は、相殺外しを狙う `KASANE Basic v1`、相殺当て・生存を優先する `KASANE Guard v1`、攻撃/生存の専門家と状態付き相殺判断を統合した `KASANE Strategy v2`、Strategy v2を安全な土台として独立した積み込み状態機械を重ねる `KASANE Stack-REN v3` を収録しています。Stack-REN v3は2〜4列のウェルを対局中に固定し、蓄積、着火形の仕込み、発火、REN継続、撤退を別々に判断します。

## 学習・ベンチマーク固定ルール

- 操作入力間隔: 50 ms
- ライン消去時間: 750 ms
- お邪魔猶予: 1000 ms（到着から1000 msを超えた時点で上昇可能）
- 下12段の倒し切り比較: パーフェクトクリア特別火力・評価報酬を両AIとも0
- 通常対戦とStack-REN学習: シミュレータ同様のPC火力10
- 7-bag、HOLD、SRS到達可能手、同時着火、相殺、穴バラをイベント駆動で計算

`Rules::pinned()` はPC0の下12段比較、`Rules::live()` はPC10の通常対戦を表し、どちらも最初の3値を検査します。Web対戦では固定値をモデル側から上書きせず、AI設定画面の値を観測へ渡します。

## 構成

```text
自盤面 ──> KASANE-Base（独自合法手探索・4手beam64）──> 安全な通常手
   │                                                    │
   └─ 相手盤面/Next/速度 ─> CC 4手ロールアウト ─> packet timeline
                                                    │
                         同時着火で通る火力を比較 <──┘
                                                    │
                       安全制約を全て満たす時だけ通常手を上書き
```

### KASANE-Base

- Cold Clearとは独立した合法手列挙、HOLD、4手ビーム探索です。
- 盤面高さ、穴、被り、凹凸、行遷移、井戸、B2B、Tetris/T-spin、REN火力、掘り、入力数を評価します。
- 状態価値は探索葉で一度だけ、消去・火力・入力などの遷移報酬は経路上で一度ずつ加算します。
- 12軸の分離CEMで、20秒の実対戦KO数を最優先に6,144試合（12候補×64共通シード×8世代）最適化しました。
- 採用重みは [models/base-model-evolved-v3.json](models/base-model-evolved-v3.json) です。

Cold Clearの公開実装にある高さ層、穴・被り、凹凸、行遷移、井戸、T-slot、B2B、消去種別、REN、移動時間、T温存という評価族は設計上の参考にしました。一方、探索器、特徴計算、最終重み、時間戦術はKASANE側の実装です。参照元は [Cold Clear公式リポジトリ](https://github.com/MinusKelvin/cold-clear) とローカルに固定した `third_party/cold-clear-reference` です。

### 相手予測と相殺外し

- 観測した相手盤面、Next/HOLD、保留お邪魔をCold Clearへ渡し、最大4手をロールアウトします。
- 操作列×50 ms、判断遅延、消去750 ms、観測平均速度から各着火時刻を推定します。
- 自分の各攻撃候補を50 ms格子で前後へずらし、お互いの保留パケットを時系列で厳密に相殺します。
- 同一時刻の着火では、両者が既存キューを相殺してから新規パケットを交換します。したがって早撃ちで相殺される4火力が、完全同期なら4火力のまま通る場合があります。この性質は単体テストで固定しています。
- Basic-v1では、待ち750 ms以下、相手高さ＋通過圧13以上、自分の余白5以上、早撃ちより通過圧が増える、完全同期、という条件を全て要求します。
- 待機なしの通常手はKASANE-Baseの選択を厳守し、証明された相殺外しだけが上書きできます。

### KASANE Guard v1（相殺当て・生存特化）

- Cold Clearの選択を安全なフォールバックとして保持し、学習済みKASANE候補が盤面安全性を落とさず相殺を増やす時だけ差し替えます。
- Cold Clear候補よりheadroom、穴、被り、到達可能な下穴を悪化させず、凹凸増加は2以内に制限します。
- 盤面安全性が同等なら、保留おじゃまを少なくとも1行多く相殺できる候補だけを採用します。
- 学習・評価では防御側の送信火力を攻撃源から分離し、Cold ClearとGuardへ完全に同一の攻撃時系列を与えています。全paired試合で攻撃digestが一致し、PC特別火力・報酬は0です。
- 採用重みは [models/guard-model-evolved-v1.json](models/guard-model-evolved-v1.json)、採用ゲートは [config/kasane-guard-v1.json](config/kasane-guard-v1.json) です。

### KASANE Strategy v2（攻守の状態戦略）

- Cold Clearと同じノード予算の永続フォールバックを常に保持し、攻撃・生存・RENの専門家を盤面高、穴負担、1000 ms以内の保留火力、相手余白から切り替えます。
- 火力をすぐ撃つ、相手着火まで止める、通常手を積みながら貯める、観測したおじゃま到着または上昇を合図に放つ、の状態を明示的に持ちます。
- 予測だけを根拠に放出せず、実際のincoming edgeまたはgarbage-rise edgeを観測してから発火する安全側ゲートがあります。
- 相殺は到着順FIFOで処理し、ライン消去中の750 msと1000 ms猶予をまたいで上昇するパケットを先読みします。
- 方策は [config/kasane-strategy-v2.json](config/kasane-strategy-v2.json)、学習済みゲートは [models/strategy-model-v2.json](models/strategy-model-v2.json) です。

### KASANE Stack-REN v3（独立した積み込み・発火モデル）

- Strategy v2とは別モデルです。Neutral / Building / Firingを永続化し、選んだウェルの開始列と幅を途中で変えません。
- 空盤面では完遂率の高い3-wideを事前分布の中心にしつつ、2〜4列を全て候補に残します。既存のおじゃまがある時は、最深部の1穴がウェル内かつウェル内最下列に一致する候補を優先し、不一致へ大きな減点を入れます。
- 外側へ非消去で積み、2穴までの回収可能な過渡形を許容します。十分な深さになった後だけウェル内へ着火形を仕込み、その次から3手以上のRENが実際のNEXTで成立すると短いビーム探索が証明した場合に限って採用します。発火後におじゃま穴が露出しても、穴数だけでREN継続を止めません。
- 新規の積み込みは相手8〜10段、1000 ms内の確定受信0、予測火力1以下、発火後余白12段以上、可視NEXTで2連鎖以上が先行証明された局面に限定します。操作入力50 ms、消去750 ms、おじゃま猶予1000 ms、FIFO相殺を各手で進め、開始後は余白が許す限り受けてから即発火できます。
- 56特徴を正規化する線形ヘッドと固定56→64→32 MLP特徴写像を併用し、最終32重みをCold Clearとの座席反転（mirrored）CEM対戦で探索します。Phase 4の未見検証では学習候補が初期ヘッドを超えなかったため、悪化重みを棄却して透明な初期ヘッドを保持しました。推論本体は固定長配列を使うallocation-free経路です。
- 方策は [config/kasane-stack-ren-v3.json](config/kasane-stack-ren-v3.json)、モデルは [models/stack-ren-model-v3.json](models/stack-ren-model-v3.json) です。

## ビルドと実行

PowerShellで `kasane` ディレクトリから実行します。

```powershell
cargo build --release
cargo test --release
.\target\release\kasane.exe benchmark --games 500 --threads 24 --cold-clear-nodes 1000 --seed 5422712408173666305 --output results\reproduce-500.json
```

旧 `benchmark` のBasic側既定値はBase v3、Tempo v3、depth 4、beam 64、予測300ノードです。Strategy v2 / Stack-REN v3の通常PC10対戦は `evaluate_duel` を使い、ブラウザ版と同じ通常手の自由な再順位付けが既定です。旧挙動の比較だけは `--strict-base-policy` を指定します。

```powershell
.\target\release\evaluate_duel.exe --stack-ren --pairs 64 --threads 20 --cold-clear-nodes 300 --kasane-nodes 300 --strategy-policy config\kasane-stack-ren-v3.json --stack-ren-model models\stack-ren-model-v3.json --output results\stack-ren-holdout.json
.\target\release\train_stack_ren.exe --generations 12 --population 24 --elite 6 --pairs 16 --validation-pairs 32 --validation-candidates 10 --nodes 300 --strategy-policy config\kasane-stack-ren-v3.json --source-model models\stack-ren-model-v3.json --output models\stack-ren-model-v3-candidate.json --provenance results\stack-ren-training.json
```

機械可読な採用設定は [config/kasane-basic-v1.json](config/kasane-basic-v1.json) にあります。

## Webシミュレータでの対戦

シミュレータのデバッグモードを有効にすると、編集画面のP1/P2それぞれの「AI」欄に現在のモデル名が表示されます。AI欄を650 ms長押し（PCではモデル名のクリックまたは右クリックも可）すると、`Cold Clear`、`KASANE Basic v1`、`KASANE Guard v1`、`KASANE Strategy v2`、`KASANE Stack-REN v3`、戦術なしの`KASANE Base v3`を個別に選択できます。2Pで両方のAIをチェックすれば、その組み合わせで直接対戦します。Stack-REN選択時のデバッグ表示には、固定中のウェル列も `well C1-C4` の形式で出ます。

Web対戦では通常のPC火力10を使い、AI思考時間・操作入力間隔・AI SDF・ライン消去時間・お邪魔猶予などは設定画面の値をそのまま使います。KASANEへは毎手、その実設定と、双方の盤面・Current/NEXT/HOLD・保留お邪魔の到着時刻・現在フェーズ・実測平均速度を渡します。同一描画フレームで発火した攻撃は、双方が既存パケットを相殺してから一括配送するため、相殺外しがプレイヤー更新順に潰されません。

モデルを更新した後は、リポジトリ直下から次のコマンドでブラウザ用WASMを再生成します。

```powershell
cargo build --release --target wasm32-unknown-unknown --manifest-path simulator/kasane-wasm/Cargo.toml
Copy-Item simulator/kasane-wasm/target/wasm32-unknown-unknown/release/simulator_kasane_wasm.wasm simulator/workers/kasane.wasm -Force
```

## 評価結果

主シナリオは「攻撃側は空盤面、守備側Cold Clearは下12段が各行で必ず穴位置の変わる完全穴バラ」です。両者は同じシードのミノ列・穴列を使うpaired比較です。

| CC探索 | 未見試合 | 時間 | Cold Clear | KASANE | 差 | paired 95% CI |
|---:|---:|---:|---:|---:|---:|---:|
| 1000 nodes | 1000 | 20秒 | 21.3% | 27.2% | **+5.9 pt** | **+2.53〜+9.27 pt** |
| 1000 nodes | 1000 | 30秒 | 58.2% | 64.3% | **+6.1 pt** | — |
| 1500 nodes | 500 | 20秒 | 23.6% | 22.4% | -1.2 pt | -5.87〜+3.47 pt |
| 1500 nodes | 500 | 30秒 | 56.4% | 62.0% | **+5.6 pt** | — |

1000ノード合算では、20秒でKASANEだけが倒した試合179、Cold Clearだけが倒した試合120でした。KASANEの相殺外しは平均0.145回/試合、待機53.8 ms/試合です。平均送信はKASANE 20.616、Cold Clear 20.556でほぼ同じなので、改善の中心は総火力の水増しではなく送信タイミングです。

Strategy v2を安全床にした最終Stack-REN v3を、PC報酬0・完全穴バラ下12段・20秒・同一未見256シード・各300 nodesで評価すると、Cold Clearは90/256（35.16%）、KASANEは125/256（48.83%）で、差は **+13.67 pt**、paired 95% CIは **+6.07〜+21.27 pt** でした。通常PC10の別未見64 mirrored pairsでは、Strategy v2が15勝13敗100時間切れ、積極的なStack-REN設定は悪化したため採用していません。公開既定値は入口96の安全側設定で、このサンプルではStack-REN開始0となりStrategy v2と同じ15勝13敗100時間切れです。したがって、下12段性能のCold Clear超えは確認済みですが、通常対戦でStack-RENがStrategy v2やCold Clearを上回る主張はまだしません。

完全レポート:

- [1000-node 未見500 A](results/kasane-basic-v1-final-unseen-500.json)
- [1000-node 未見500 B](results/kasane-basic-v1-replication-unseen-500.json)
- [1000試合合算](results/kasane-basic-v1-aggregate-unseen-1000.json)
- [1500-node 未見500](results/kasane-basic-v1-cc1500-unseen-500.json)
- [Base v3進化履歴](results/base-evolution-v3.json)
- [Stack-REN Phase 4学習・検証履歴](results/stack-ren-training-v3-phase4.json)
- [Stack-REN v3 下12段・未見256](results/stack-ren-v3-final-bottom12-shared-seed-256.json)
- [Stack-REN v3 通常PC10・最終安全床](results/stack-ren-v3-final-default-unseen-64pairs.json)

Guard v1は、下10/14/16段の穴バラと固定Cold Clear攻撃時系列を混ぜた防御試験で学習しました。未使用seed 256試合の標準探索では、トップアウトがCold Clear 174/256に対してGuard 151/256（**8.98 pt減**、paired 95% CI **3.51〜14.46 pt**）、平均生存時間は596.99 ms増えました。探索をattacker/defender各600 nodes、forecast 200、depth 3、beam 32へ強めた別seed 256試合でも、Cold Clear 167/256に対してGuard 162/256、平均生存時間474 ms増、相殺率16.13%→16.77%でした。後者の差は1.95 ptで95% CIが-3.07〜6.98 ptのため、強探索での優位はまだ統計的確定ではありません。

Guard完全レポート:

- [学習・標準未使用holdout](results/guard-training-v1.json)
- [強探索・完全別seed holdout](results/guard-holdout-strong-v1.json)

当初置いた野心目標「20秒差+15 pt、paired CI下限+10 pt」はまだ未達です。Basic-v1は1000ノードCCには統計的に勝ち越しましたが、1500ノードCCの20秒指標では同等以上とまだ言えません。30秒の倒し切りでは両設定で上回っています。

## 学習系

Cold Clear教師による盤面セル＋Next/HOLDの蒸留、DAgger用の挙動混合、PyTorch MLP学習経路も残しています。

```powershell
.\target\release\kasane.exe generate-base-dataset --states 20000 --teacher-nodes 1500 --threads 24 --output data\base-distillation.jsonl
python tools\train_base_model.py --dataset data\base-distillation.jsonl --bootstrap config\base-model-heuristic-v1.json --output models\base-mlp.json --epochs 30 --device cuda
```

ただし今回の366入力MLPは検証top-1 55.09%でも閉ループ80試合の20秒KOが0%だったため不採用です。オフライン模倣精度だけでは探索中の分布ずれを防げない、という結果です。現在の基盤は透明なValue/Reward探索＋CEMであり、GAだけでもNN一本でもありません。

基盤を再進化する例:

```powershell
.\target\release\kasane.exe evolve-base --population 12 --elites 4 --generations 8 --games-per-candidate 64 --defender-nodes 1000 --base-depth 4 --base-beam-width 64 --initial-sigma 0.32 --source-model models\base-model-evolved-v2.json --output-model models\base-model-evolved-v3.json --output-report results\base-evolution-v3.json --threads 24
```

Guardの再学習と凍結モデル評価:

```powershell
cargo run --release --bin train_guard -- --output-policy config\kasane-guard-v1.json --output-model models\guard-model-evolved-v1.json --output-report results\guard-training-v1.json
cargo run --release --bin evaluate_guard -- --games 256 --attacker-nodes 600 --defender-nodes 600 --forecast-nodes 200 --base-depth 3 --base-beam-width 32 --output results\guard-holdout-strong-v1.json
```

## 次の段階

1. Stack-REN v3の着火形探索を3手から長期Valueへ広げ、実測勝率で発火/継続/撤退を同時学習する。
2. 予測分布を単一CCロールアウトから複数候補・速度不確実性へ広げ、期待値ではなく下側リスクで相殺外しを選ぶ。
3. KASANE同士、Cold Clear、Guardを混ぜたリーグで、特定相手・特定seedへの過適合を抑える。
4. Stack-REN v3とは別の中開け専用モデルを追加し、「ほぼ確実に繋がる高さ」と「下穴を塞がなければ接続可能な高さ」を直接特徴化する。

Basic / Guard / Strategy v2へ中開け固有ロジックは混ぜていません。中開け専用モデルは、現在のStack-REN v3の固定ウェル状態とリスク予測を土台にしつつ、別の選択肢として追加する予定です。
