# Cold Clear Standard-mode port

`cold-clear-master.zip` is kept intact. The simulator does not load code from
that archive. Instead, `workers/cold-clear-core.js` is a new JavaScript port of
the algorithms needed for Cold Clear's normal/Standard mode.

The reference archive is Cold Clear by MinusKelvin and is licensed under MPL-2.0.
The port file carries the same license notice. This project does not contain a
reference opening-book data file, so no opening book is forced.

## Reference-to-port mapping

| Reference source | Simulator port |
| --- | --- |
| `libtetris/src/board.rs`, `lock_data.rs` | `CCBoard`: bit rows, column heights, line clears, combo, B2B, perfect clear, and attack accounting |
| `libtetris/src/piece.rs`, `moves.rs` | SRS movement search, kick-aware mini/full T-spin state, input-time tracking, and legal placements |
| `bot/src/evaluation/standard.rs` | Standard default coefficients, `Value`/`Reward`, timed jeopardy, bag-aware T-slot cutouts, well/bumpiness/cavity/covered-cell evaluation |
| `bot/src/dag.rs`, `modes/normal.rs` | Persistent transposition DAG, 7-bag unknown-piece branches, weighted random leaf selection, backpropagation, state reuse after a committed move |

The Worker protocol mirrors the important lifecycle of the reference bot:

```text
analyze(snapshot) -> move -> think ahead in short Worker slices
                   -> commit(played move) -> addNextPiece(revealed preview)
                   -> analyze(next snapshot)
```

`addNextPiece` resolves the matching 7-bag chance branch just as
`DagState::add_next_piece` does in the reference. A rolling NEXT preview is
therefore not treated as a different board state. Only garbage, manual input,
or a failed placement causes a safe reset. This is deliberately different from
the old `start(snapshot) -> stop` pair, which stopped before meaningful
thinking could occur.

## Deliberate scope

This port implements the normal `Standard` game AI. The archive's optional
opening book has no data in this repository, and its PC-loop mode is disabled
in the reference wasm build too; neither is replaced with a hand-written
template. The old fixed templates were removed from the active AI path.

The core currently runs in a dedicated JavaScript Worker. A pure WebAssembly
build is not included because the supplied archive has no prebuilt wasm package
and the workspace lacks the wasm Rust target/bindgen tooling. The Worker and
server are structured for a later Rust/WASM core without changing the Player
protocol. Do not substitute the original archive as a binary dependency: keep
the port independent and preserve the MPL-2.0 notice.

## Tests

Run the core smoke test with Node.js:

```text
node tools/test-cold-clear-core.js
node tools/test-cold-clear-worker-background.js
```

It verifies legal moves for every tetromino, correct empty-hold transitions,
rolling NEXT reuse after normal/hold moves, and cooperative background search.
