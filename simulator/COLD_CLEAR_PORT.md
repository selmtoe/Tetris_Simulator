# Cold Clear Standard WASM integration

The simulator runs a modified Cold Clear Standard Rust search core through
`simulator/workers/cold-clear.wasm`. The reference source and project changes
are in `third_party/cold-clear-reference/` under MPL-2.0. The C ABI integration
in `simulator/cold-clear-wasm/` is also provided under MPL-2.0. Cold Clear is by
MinusKelvin and contributors: https://github.com/MinusKelvin/cold-clear.

Project changes include browser ABI/state management, incremental search and
node budgets, candidate/plan exports, and platform adaptations in the search
and supporting crates. See the supplied source for the exact implementation;
this build is not an unchanged upstream release. All original license notices
are retained. Full text and source delivery: [../licenses/index.html](../licenses/index.html).

The distributed WASM's corresponding source (including modifications,
Cargo.lock and build scripts) is [../licenses/cold-clear-source.zip](../licenses/cold-clear-source.zip).
Its binary and source hashes are in `licenses/cold-clear-build.json`. A ZIP
snapshot is kept when the binary is built; subsequent local edits are not
silently substituted for that snapshot.

The reference modules used by the WASM build are:

- `libtetris`: `u16` bitboard rows, column heights, lock/clear accounting,
  SRS kicks, T-spin detection, and placement search.
- `bot/src/dag.rs`: the persistent generation-aware DAG and Monte Carlo leaf
  selection.
- `bot/src/evaluation/standard.rs`: the published Standard evaluator and
  coefficients.
- `bot/src/modes/normal.rs`: hold, 7-bag speculation, node expansion,
  backpropagation, and move selection.

`simulator/workers/cold-clear-wasm-worker.js` preserves the existing Worker
protocol (`analyze`, `commit`, `addNextPiece`, `reset`, `pause`). The Rust ABI
keeps the DAG alive across pieces; the JS side only marshals board snapshots,
previews, and move results. The simulator's top-to-bottom board coordinates
are converted at the ABI boundary to libtetris' bottom-to-top coordinates.

Install Rust with the `wasm32-unknown-unknown` target and Python 3. Rebuild with:

```text
powershell -ExecutionPolicy Bypass -File tools/build-cold-clear-wasm.ps1
cargo test --manifest-path simulator/cold-clear-wasm/Cargo.toml --test smoke
```

The browser benchmark is available at `tools/cold-clear-benchmark.html` when
the local server is running. It compares the legacy JS port and the reference
WASM on the same empty-field snapshot and reports retained DAG nodes, nodes/ms,
and the WASM/JS node ratio. The Node.js equivalent is
`tools/bench-cold-clear-wasm.js`.

The old `workers/cold-clear-core.js` remains in the repository for regression
comparison and REN search. It is provided under MPL-2.0 and is no longer loaded
by the simulator's active AI Worker.
