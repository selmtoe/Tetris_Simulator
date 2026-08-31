# KASANE final-v2 overnight report — 2026-09-01

## 結論

凍結した KASANE Strategy-v2 final-v2 は、事前登録した限定 native 課題で bundled Cold Clear 1 Standard を上回った。

正確な主張範囲は、PC特別火力0、完全穴バラ下12段、20秒KO、固定300 nodes、空盤面攻撃側、同一2,048 paired seed の試験である。この条件では KASANE のKO率は 61.47%、Cold Clearは33.45%、差は **+28.03 percentage points**、固定標本Hoeffding 95%区間は **[+22.03, +34.03] points** だった。事前gateである下限0超と、実用目標である平均差15 points以上・下限10 points以上をともに通過した。

これは Cold Clear 2、公式配布binary、通常PC10全般、Web worker、equal CPU、equal wall time、または普遍的なテトリス性能への主張ではない。

## 実装した強化

- 各プレイヤーの埋め込みCold Clear安全床を世代対応の永続DAGにし、盤面、HOLD、B2B、REN、Nextが一致する展開済みbranchだけを次手へ継続する。不一致、未展開override、外部garbage riseでは破棄して安全に再構築する。
- 40行盤面、HOLD、Next、bag、B2B、REN、node予算、incomingを明示的に混合し、各bounded think直前に決定論的にreseedする。Rayon worker数やstateless opponent forecastの実行順による揺れを除いた。
- incoming garbageを到着順packet列として扱い、FIFO相殺、同時着火、750 ms line-clear中、1,000 ms grace超過後、次forecast lockまでのriseを投影する。
- opponent forecastの最初の手には現在のincoming queueを反映した`outgoing_attack`、途中garbage riseを完全再現できない2手目以降には過小評価を避ける`raw_attack`上限を使う。
- Strategy-v2 `AgentKind::Kasane` は埋め込みCold Clear床が10,000 nodes以上のとき、床とplacement keyが異なる候補を全て棄却する。同一placementの状態観測・待機判断だけを残す。300-nodeではこのgateは不活性で、独自配置専門家が動く。別のStack-REN agentはこの保証の対象外であり、今回の3試験では無効。
- native/WASM双方のnode budgetを上限として扱い、WASMのthink-time tierとnode指定は厳しい側を採用する。
- benchmark/direct schemaをv5へ上げ、固定indexed seed collection、固定標本Hoeffding、exact McNemar感度指標、override counters、source/model/executable provenanceを収録した。

評価関数の設計族は、固定revisionの[Cold Clear Standard evaluator](https://github.com/MinusKelvin/cold-clear/blob/279edd7c3177ff8077f6a930193397814b281f27/bot/src/evaluation/standard.rs)を参照した。DAGの持続・世代遷移は同revisionの[Cold Clear DAG](https://github.com/MinusKelvin/cold-clear/blob/279edd7c3177ff8077f6a930193397814b281f27/bot/src/dag.rs)を基準に監査した。KASANEのFIFO timing、相手forecast、状態戦略、高予算placement floorは本候補側の実装である。

## 事前登録した最終結果

Protocol: [night-final-v2-protocol-2026-09-01.md](night-final-v2-protocol-2026-09-01.md)

| Endpoint | Cold Clear | KASANE | Paired result | 判定 |
|---|---:|---:|---:|---|
| Primary: PC0 bottom-12, 300 nodes, 20 s, 2,048 pairs | 685/2,048 = 33.45% | 1,259/2,048 = 61.47% | +28.0273 pt; Hoeffding 95% [+22.0253, +34.0294] pt; K-only 756, CC-only 182 | 限定優位gate PASS |
| Stress: PC0 bottom-12, 120k nodes, 20 s, 2,048 pairs | 280/2,048 = 13.67% | 280/2,048 = 13.67% | 0 pt; Hoeffding 95% [-6.0020, +6.0020] pt; 全pair一致 | placement safety PASS、優位gate FAIL |
| Secondary direct: PC10, 120k nodes, 120 s, 512 mirrored pairs | — | 98勝98敗828 timeout | 全512 cluster score 0.5; mean 0.5; Hoeffding 95% [0.439980, 0.560020] | placement safety PASS、優位なし |

Primaryの300-node KASANEは平均13.8628 placement overrides/試合であり、新しい10k production gateは不活性だった。したがってprimary改善を「待機だけ」またはCold Clearとの同一配置によるものとは解釈しない。

120k stressではKASANEのplacement、policy、wait overrideがすべて0で、KO CDF、勝敗、piece、attack、intent集計までCold Clearと観測上完全一致した。Directでも同じ3 overrideが0で、全512 cluster scoreが0.5だった。これらは高予算回帰を止めた安全観測であり、統計的同等性の証明ではない。

Machine-readable results:

- [Primary JSON](night-final-v2-primary-300n-2048.json)
- [120k stress JSON](night-final-v2-stress-120k-2048.json)
- [PC10 direct JSON](night-final-v2-direct-pc10-120k-512pairs.json)

## 統計監査

Primary/stressの独立単位は2,048 paired seedsで、各pairの差は[-1,1]。two-sided 95% Hoeffding半径は `sqrt(2 ln(40) / 2048) = 0.060020174457495`。Primaryの差 `(756 - 182) / 2048 = 0.2802734375` から区間を再計算し、JSONと一致した。

Directの独立単位は512 mirrored seed clustersで、scoreは[0,1]。半径は `sqrt(ln(40) / (2 * 512)) = 0.060020174457495`。全scoreが0.5でも区間を0幅にせず、固定標本boundを維持した。Decisive-only Wilsonは探索指標で、優位判定には使っていない。

3結果は別エージェントがconfig、公開labelからのseed導出、件数、joint counts、区間式、override gate、provenance hashを独立再計算し、差異なしだった。途中結果を見た停止、seed差替え、sample縮小は行っていない。

## Frozen hashes

| Artifact | SHA-256 |
|---|---|
| Protocol | `6e7196365d87d4ecca5abff3993b3657c8ffc0fb27e344d039ecbc1bb0c8f73a` |
| Primary result | `528b0d3868984ea866e115f7fd1f61faeabfc12e1a37e57afd512c773b7e511c` |
| Stress result | `7ac75e8fd186f78ec205d885e1b69d0e0fbaaace2f2c30a2393f46894211fa3a` |
| Direct result | `9d3afac6348b7a3505f1f23f2a34be4e98fe21ddff3694f3cabf2502eac53a06` |
| `kasane.exe` | `427769317e6c6e69f71b9b6d54fab1ee501d51465f120523d5d69091c4cbb277` |
| `evaluate_duel.exe` | `72476f97a5158092cb7a63a56ab0ad67fb525c766abcb0f246e326c11cf8ed4b` |
| `src/agent/strategy.rs` | `705d04ede0ac8a7611868cf235eb9e8b03d9bed54d496224aca4134885345a0d` |
| `src/agent/kasane.rs` | `ba8a03190c8db13e5340d7554081c1162e10b3da412cf2da5e320814f5b8be17` |
| `src/agent/cc.rs` | `a411c3b5840f070017fb7e6326231365b89eee96b53ba5949683f91e7dc495b3` |
| `src/benchmark.rs` | `c0e742836eb5d580aed31132fc9c7fd4851145f1aed6abb0eddd0dca723e524f` |
| `src/bin/evaluate_duel.rs` | `5b637fb5a22acb629cc287a0fe3bcd4d367aed38b1cea570fa924f042c512675` |
| Strategy policy | `853fb8775737eda9cb19b167532da19890cd513bde2de5d0f0e8d796b55f136f` |
| Base model | `098913bd7b4e8fcaaacedc51231c0c38560f429517fe1367e93b152533ea7ded` |
| Strategy model | `d61b0b1048894f7b7c1ab37a7a002556ebb6a4d20809f81f05d1ca1ffc5b4482` |
| Local CC DAG | `fe14d84dd5e9c023a0d791e979ec18456ff013803daf514596fd70211d5983a4` |
| Local CC Standard evaluator | `25cbb5549a39b0c3843bd10348d6099f9fb74764052dd2b638077a19375646d7` |
| Final installed `simulator/workers/kasane.wasm` | `423b896f4ccb1cd6ae2156e005c79b6caf9f215b25c4c7cdac83a553520dd9f5` |

最終WASM生成物と配布先は同hash、1,014,422 bytes。直前の中間build `21d4497c...b969d` から更新された。

## 検証

- `cargo fmt --all -- --check`: PASS
- `cargo test --release --locked --all-targets`: PASS。library 64 passed / 0 failed / 1 explicit 120k performance test ignored。全bin unit tests PASS。
- Relevant-path `git diff --check`: PASS。表示されたのはWindows line-ending warningのみ。
- Final WASM `cargo build --release --locked --target wasm32-unknown-unknown`: PASS。生成物と配布先hash一致、required exports確認済み。
- `node --check` for both KASANE worker files: PASS
- garbage replan 5 cases: PASS
- no-legal-move 6 cases: PASS
- Cold Clear background worker test: PASS
- tracked `kasane-v8` references: 0

Build/test warningはvendored Cold Clear/opening-bookの既存deprecated、unexpected cfg、dead-code警告のみだった。

## 再現環境と制約

- Base commit: `273456e344e04880384386faf6245a24c35ecae3`
- Rust: `rustc 1.97.1 (8bab26f4f 2026-07-14)`; LLVM 22.1.6
- Cargo: `1.97.1 (c980f4866 2026-06-30)`
- OS: Microsoft Windows 11 Home 10.0.26200, build 26200
- Upstream Cold Clear revision: `279edd7c3177ff8077f6a930193397814b281f27`

作業開始時からworktreeには多数のユーザー所有の変更・削除・未追跡成果物があった。無関係な変更は復元、削除、移動、commitしていない。

WebではKASANEのthink timeをnode tierへ写像する同期探索と、standalone Cold Clear workerのwall-time deadline/background探索が異なる。したがってWeb対戦はequal-compute証明に使わない。Opponent forecastは最大4手の単一路線で、後続手の途中garbage riseを盤面へ完全反映しないため、2手目以降のraw boundは安全側だが過大評価し得る。
