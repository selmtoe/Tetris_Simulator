# KASANE final-v2 fixed-sample confirmation protocol

Written after the high-budget placement-floor A/B test and before any final-v2
seed was executed. Source, binaries, policy, models, sample sizes, endpoints,
and seed roots are frozen. No run may be stopped or resized after observing a
partial result.

## Frozen candidate

- `kasane.exe`: `427769317e6c6e69f71b9b6d54fab1ee501d51465f120523d5d69091c4cbb277`
- `evaluate_duel.exe`: `72476f97a5158092cb7a63a56ab0ad67fb525c766abcb0f246e326c11cf8ed4b`
- `src/agent/strategy.rs`: `705d04ede0ac8a7611868cf235eb9e8b03d9bed54d496224aca4134885345a0d`
- `config/kasane-strategy-v2.json`: `853fb8775737eda9cb19b167532da19890cd513bde2de5d0f0e8d796b55f136f`
- Base model: `098913bd7b4e8fcaaacedc51231c0c38560f429517fe1367e93b152533ea7ded`
- Strategy model: `d61b0b1048894f7b7c1ab37a7a002556ebb6a4d20809f81f05d1ca1ffc5b4482`
- Cold Clear DAG source: `fe14d84dd5e9c023a0d791e979ec18456ff013803daf514596fd70211d5983a4`
- Cold Clear Standard evaluator: `25cbb5549a39b0c3843bd10348d6099f9fb74764052dd2b638077a19375646d7`

For the Strategy-v2 `AgentKind::Kasane` used by all three commands, at
`cold_clear_nodes >= 10,000` the learned 300-node experts may retain state and
inspect timing, but any action whose placement key differs from the exact Cold
Clear floor is rejected. The new production gate is inactive below 10,000
nodes. This statement does not cover the separate Stack-REN agent.

## Public seed derivation

Each root is the little-endian unsigned 64-bit value of the first eight bytes
of SHA-256 over the UTF-8 label. These labels were chosen and recorded before
the roots were used.

- Primary label `kasane-2026-09-01-final-v2-primary`, SHA-256
  `a46d0b15e9cce2f407cd9aeafdc71f9483c749117fbe06c9d79186561689e2c5`,
  seed `17645891591443541412`
- Stress label `kasane-2026-09-01-final-v2-stress`, SHA-256
  `a2e02715c794e20e9c125307e160c0aeaf551730567bc65b674bc2dc0ed2537a`,
  seed `1072583244041937058`
- Direct label `kasane-2026-09-01-final-v2-direct`, SHA-256
  `271670c32a618549abade1ec9cd2a2776939dcded8a755c02e3d29ff20b9de21`,
  seed `5297747372966680103`

All three decimal roots were absent from existing result JSON files when this
protocol was written.

## 1. Primary limited superiority claim

- 2,048 paired bottom-12 strict random-hole scenarios
- PC special attack/reward 0, 50 ms input, 750 ms line clear, 1,000 ms grace
- Empty attacker versus bundled Cold Clear 1 Standard defender
- 300-node standalone Cold Clear and 300-node embedded Cold Clear floor
- Primary endpoint: KO by 20,000 simulated milliseconds
- Gate: fixed-sample two-sided 95% Hoeffding lower bound for paired differences
  must exceed zero
- Practical target: mean advantage >=15 pp and Hoeffding lower bound >=10 pp

```powershell
Set-Location 'C:\Users\hirom\Documents\tetris ai\kasane'
.\target\release\kasane.exe benchmark --games 2048 --threads 24 --time-limit-ms 20000 --cold-clear-nodes 300 --kasane-nodes 300 --strategy-policy config\kasane-strategy-v2.json --seed 17645891591443541412 --output results\night-final-v2-primary-300n-2048.json
```

The only permitted superiority wording is limited to this exact native,
fixed-node, PC0 bottom-12, 20-second task.

## 2. Production-node safety stress

- Same bottom-12 contract and 2,048 fixed pairs
- 120,000 nodes for standalone Cold Clear and the KASANE floor
- Expected safety property: no placement override at production budget
- Validity gate: `kasane.mean_placement_overrides == 0`
- A superiority claim requires this run's own Hoeffding lower bound above zero.
  Equality of observed outcomes is a safety observation, not an equivalence
  proof.

```powershell
Set-Location 'C:\Users\hirom\Documents\tetris ai\kasane'
.\target\release\kasane.exe benchmark --games 2048 --threads 24 --time-limit-ms 20000 --cold-clear-nodes 120000 --kasane-nodes 120000 --strategy-policy config\kasane-strategy-v2.json --seed 1072583244041937058 --output results\night-final-v2-stress-120k-2048.json
```

## 3. Normal-PC direct-duel secondary endpoint

- 512 mirrored seed clusters, 1,024 seat-swapped matches
- Empty versus empty, PC special attack 10, 120,000 nodes on both CC searches
- 120,000 simulated ms; timeout/draw score 0.5
- Fixed-sample Hoeffding interval over mirrored cluster scores
- Production safety consistency requires `kasane.placement_overrides == 0`
- Always secondary. Decisive-only Wilson rate cannot establish superiority.

```powershell
Set-Location 'C:\Users\hirom\Documents\tetris ai\kasane'
.\target\release\evaluate_duel.exe --pairs 512 --threads 24 --time-limit-ms 120000 --cold-clear-nodes 120000 --kasane-nodes 120000 --strategy-policy config\kasane-strategy-v2.json --seed 5297747372966680103 --output results\night-final-v2-direct-pc10-120k-512pairs.json
```

## Validity checks

Every scheduled sample must finish. Schema must be v5. All hashes actually
embedded in each report must match with no read error. Because the cheese
report does not embed the outer policy or Strategy-v2 source hash, the frozen
list above is also the required external manifest: the two executables,
`strategy.rs`, policy, both models, CC DAG, and CC evaluator are re-hashed after
all runs and must match it. The direct report additionally must contain matching
roles for policy, Strategy/Timing/CC bridge sources, both CC sources, both
models, and its executable. The fixed-sample Hoeffding field is the only
superiority gate; legacy Wald/Wilson and McNemar sensitivity cannot pass a
claim alone.

These are native fixed-node comparisons, not equal-CPU, equal-wall-time,
browser-worker, CC2, or universal Tetris claims.
