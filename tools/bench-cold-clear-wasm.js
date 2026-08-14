/*
 * Compare the legacy JS port and reference Rust/WASM on identical snapshots.
 * Run with Node.js after building the artifact:
 *   node tools/bench-cold-clear-wasm.js
 */

'use strict';

const fs = require('fs');
const path = require('path');
const { performance } = require('perf_hooks');

globalThis.self = globalThis;
require('../simulator/workers/cold-clear-core.js');
const { Search } = globalThis.ColdClearSimulatorCore;

const wasmPath = path.join(__dirname, '..', 'simulator', 'workers', 'cold-clear.wasm');
if (!fs.existsSync(wasmPath)) throw new Error(`Missing ${wasmPath}; run tools/build-cold-clear-wasm.ps1 first.`);

const empty = Array.from({ length: 40 }, () => Array(10).fill(null));
const snapshot = {
    board: empty,
    currentPiece: 'T',
    nextQueue: ['I', 'O', 'L', 'J', 'S', 'Z', 'T', 'I'],
    holdPiece: null,
    canHold: true,
    isB2B: false,
    ren: -1,
    incoming: 0,
    weights: {}
};

function runJs(milliseconds) {
    const search = new Search();
    search.analyze({ ...snapshot, thinkTimeMs: 1, nodeLimit: 1 });
    const before = search.nodeCount;
    const start = performance.now();
    search.think(milliseconds, 1_000_000);
    return { elapsedMs: performance.now() - start, nodes: search.nodeCount - before, totalNodes: search.nodeCount };
}

function alloc(exports, memory, bytes) {
    const ptr = exports.cc_alloc(bytes.length);
    new Uint8Array(memory.buffer, ptr, bytes.length).set(bytes);
    return ptr;
}

function runWasm(instance, milliseconds) {
    const e = instance.exports;
    const memory = e.memory;
    const board = new Uint8Array(400);
    const next = new TextEncoder().encode(snapshot.nextQueue.join(''));
    const weights = new TextEncoder().encode('{}');
    const boardPtr = alloc(e, memory, board);
    const nextPtr = alloc(e, memory, next);
    const weightsPtr = alloc(e, memory, weights);
    const handle = e.cc_create(boardPtr, 'T'.charCodeAt(0), nextPtr, next.length, 0, 1, 0, -1, 1_000_000, weightsPtr, weights.length);
    e.cc_dealloc(boardPtr, board.length);
    e.cc_dealloc(nextPtr, next.length);
    e.cc_dealloc(weightsPtr, weights.length);
    const before = e.cc_node_count(handle);
    const start = performance.now();
    let count = before;
    while (performance.now() - start < milliseconds) {
        const previous = count;
        e.cc_think(handle, 256);
        count = e.cc_node_count(handle);
        if (count === previous) break;
    }
    const elapsedMs = performance.now() - start;
    e.cc_destroy(handle);
    return { elapsedMs, nodes: count - before, totalNodes: count };
}

(async () => {
    const wasm = await WebAssembly.instantiate(fs.readFileSync(wasmPath), {});
    const samples = [50, 100, 250];
    const rows = samples.map(milliseconds => {
        const js = runJs(milliseconds);
        const wasmResult = runWasm(wasm.instance, milliseconds);
        return {
            milliseconds,
            jsNodes: js.nodes,
            wasmNodes: wasmResult.nodes,
            increase: wasmResult.nodes / Math.max(1, js.nodes),
            jsNodesPerMs: js.nodes / Math.max(0.001, js.elapsedMs),
            wasmNodesPerMs: wasmResult.nodes / Math.max(0.001, wasmResult.elapsedMs)
        };
    });
    console.log(JSON.stringify({ passed: true, samples: rows }, null, 2));
})().catch(error => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
