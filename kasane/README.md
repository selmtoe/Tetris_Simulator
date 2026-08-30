# KASANE

KASANE は、相手盤面と時間を読む対戦用テトリスAIです。最初のマイルストーンとして、独自の4手ビーム探索による火力基盤と、Cold Clearで相手の着火時刻を予測する相殺外しを実装しています。自分の着手決定にCold Clearの評価値や候補手は使いません。

現在の採用モデルは、相殺外しを狙う `KASANE-Basic-v1` と、攻守を反転して相殺当て・生存を優先する `KASANE-Guard-v1` です。中開けRENは意図的に未実装で、実験的な2手チャージも既定では無効です。

## 学習・ベンチマーク固定ルール

- 操作入力間隔: 50 ms
- ライン消去時間: 750 ms
- お邪魔猶予: 1000 ms（到着から1000 msを超えた時点で上昇可能）
- パーフェクトクリア特別火力・評価報酬: 両AIとも0
- 7-bag、HOLD、SRS到達可能手、同時着火、相殺、穴バラをイベント駆動で計算

`Rules::validate` が最初の3値とPC0を再現可能なオフライン実験契約として検査します。この固定値は学習と比較ベンチマーク専用で、Webシミュレータの対戦設定には上書きしません。

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

## ビルドと実行

PowerShellで `kasane` ディレクトリから実行します。

```powershell
cargo build --release
cargo test --release
.\target\release\kasane.exe benchmark --games 500 --threads 24 --cold-clear-nodes 1000 --seed 5422712408173666305 --output results\reproduce-500.json
```

`benchmark` のKASANE側既定値はBase v3、Tempo v3、depth 4、beam 64、予測300ノード、相殺閾値13、チャージ無効、通常手の自由な再順位付け無効です。Cold Clearを1500ノードへ強める場合は `--cold-clear-nodes 1500` を指定します。実験的チャージは `--enable-charge`、通常手の自由なTempo再順位付けは `--allow-free-rerank` で明示的に有効化できます。

機械可読な採用設定は [config/kasane-basic-v1.json](config/kasane-basic-v1.json) にあります。

## Webシミュレータでの対戦

シミュレータのデバッグモードを有効にすると、編集画面のP1/P2それぞれの「AI」欄に現在のモデル名が表示されます。AI欄を650 ms長押し（PCではモデル名のクリックまたは右クリックも可）すると、`Cold Clear`、`KASANE Basic v1`、`KASANE Guard v1`、戦術なしの`KASANE Base v3`を個別に選択できます。2Pで両方のAIをチェックすれば、その組み合わせで直接対戦します。

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

完全レポート:

- [1000-node 未見500 A](results/kasane-basic-v1-final-unseen-500.json)
- [1000-node 未見500 B](results/kasane-basic-v1-replication-unseen-500.json)
- [1000試合合算](results/kasane-basic-v1-aggregate-unseen-1000.json)
- [1500-node 未見500](results/kasane-basic-v1-cc1500-unseen-500.json)
- [Base v3進化履歴](results/base-evolution-v3.json)

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

1. 中開けではない通常REN/掘りRENの発火判断を、貯め時間、相手即時火力、穴バラ蓋、確実に繋がる高さ、接続可能高さから学習する。
2. 予測分布を単一CCロールアウトから複数候補・速度不確実性へ広げ、期待値ではなく下側リスクで相殺外しを選ぶ。
3. KASANE同士の自己対戦・リーグと、Cold Clearを含む対戦相手混合でValue/Tempoを更新する。
4. その後に中開けRENを別オプションとして実装し、発火/継続/撤退をNNまたは分布価値モデルに判断させる。

ユーザー提示の中開け評価軸「ほぼ確実に繋がる高さ」と「下穴を塞がなければ接続可能な高さ」は、将来のREN状態特徴として扱う予定です。Basic-v1には中開け固有ロジックを混ぜていません。
