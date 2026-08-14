# Cold Clear Standard WASM integration

The simulator now runs the original Cold Clear Standard search core from
`cold-clear-master.zip` through `simulator/workers/cold-clear.wasm`. The
algorithmic source is kept under `third_party/cold-clear-reference/` and is
licensed under MPL-2.0. The original archive remains intact.

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

The WASM build is reproducible with:

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
comparison. It is no longer loaded by the simulator's active AI Worker.
