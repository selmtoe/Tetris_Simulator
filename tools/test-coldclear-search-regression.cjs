// Real WASM and viewer worker, isolated from browser storage and replay data.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { performance } = require('node:perf_hooks');
const root = path.resolve(__dirname, '..');
let checks = 0;
const check = (value, message) => { assert.ok(value, message); checks++; };
const same = (a, b, message) => { assert.equal(JSON.stringify(a), JSON.stringify(b), message); checks++; };
const context = vm.createContext({ console, performance, TextEncoder, TextDecoder, Uint8Array, DataView, WebAssembly, URL, self: { location: { href: 'http://localhost/F/app/84-ai-scoring-worker.js' } } });
context.importScripts = (...names) => names.forEach(name => {
    const file = path.resolve(root, 'F/app', name.split('?')[0]);
    vm.runInContext(fs.readFileSync(file, 'utf8'), context, { filename: file });
});
context.fetch = async () => ({ ok: true, arrayBuffer: async () => {
    const bytes = fs.readFileSync(path.join(root, 'simulator/workers/cold-clear.wasm'));
    return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
} });
vm.runInContext(fs.readFileSync(path.join(root, 'F/app/84-ai-scoring-worker.js'), 'utf8'), context);
const key = move => [move.piece, move.hold, move.rotation, move.x, move.y, move.tspin];
const empty = () => Array.from({ length: 40 }, () => Array(10).fill(null));
(async () => {
    const bridge = await context.loadWasmBridge();
    check(bridge.moveSize === 64, 'deployed timing ABI preserved');
    check(!bridge.supportsDecisionTopUp, 'search budget semantics unchanged');
    check(typeof bridge.exports.cc_write_plan === 'function', 'native plan export exists');
    const snapshot = { board: empty(), currentPiece: 'T', nextQueue: [...'IOLJSZTIOL'], holdPiece: null, canHold: true, nodeLimit: 120000 };
    const handle = bridge.create(snapshot);
    try {
        bridge.think(handle, 3000, 120000);
        const beforeNodes = bridge.nodeCount(handle), beforeCandidates = bridge.candidates(handle);
        const plan = bridge.plan(handle, 12);
        check(plan.length >= 5, 'reads an actually searched continuation');
        same(bridge.nodeCount(handle), beforeNodes, 'reading plan never expands or commits');
        same(bridge.candidates(handle), beforeCandidates, 'reading plan preserves candidate scores');
        same(bridge.plan(handle, 2), plan.slice(0, 2), 'plan capacity is honored');
        const source = { current: snapshot.currentPiece, next: snapshot.nextQueue, hold: null };
        const search = context.createSearch(snapshot.board, source, {});
        const bestMove = [...beforeCandidates].sort((a, b) => b.value-a.value || b.spike-a.spike)[0];
        const bestEdge = search.root.children.find(edge => context.sameScoredMove(context.publicMove(edge), bestMove));
        check(Boolean(bestEdge), 'WASM first move maps to a legal viewer move');
        const variation = context.analysisVariation(search, { bestMove, bestEdge }, 12, { bridge, handle });
        same(variation.map(key), plan.map(key), 'every viewer continuation move comes from the WASM tree');
        check(variation.every(move => move.cells.length === 4 && move.stateAfter), 'viewer retains cells and HOLD/NEXT state');
        const oldBinary = { bridge: { plan: () => [] }, handle: 1 };
        same(context.analysisVariation(search, { bestMove, bestEdge }, 12, oldBinary).map(key), [key(variation[0])], 'old binary never falls back to a fabricated multi-move route');
        const legacySearch = context.createSearch(snapshot.board, source, {});
        const legacyEdge = legacySearch.root.children.find(edge => context.sameScoredMove(context.publicMove(edge), bestMove));
        const legacy = context.principalVariation(legacySearch, legacyEdge, 12);
        check(JSON.stringify(legacy.map(key)) !== JSON.stringify(plan.map(key)), 'fixture exposes the former unsearched JS continuation');
        console.log(JSON.stringify({ searchedMoves: plan.length, legacyMoves: legacy.length, legacyRouteDiffers: true }));
    } finally { bridge.destroy(handle); }
    const single = bridge.create({ ...snapshot, currentPiece: 'O', nextQueue: [] });
    try {
        bridge.think(single, 3000, 120000);
        check(bridge.plan(single).length <= 1, 'no continuation invented beyond known pieces');
    } finally { bridge.destroy(single); }
    for (let rotation = 0; rotation < 4; rotation++) {
        const candidate = { piece: 'I', hold: false, rotation, x: 3, y: 35, tspin: null };
        const rendered = context.candidatePublicMove(candidate);
        const expected = context.recordedOperationCells({ type: 'I', ...candidate });
        same(rendered.cells.map(c => c.join(':')).sort(), expected.map(c => c.join(':')).sort(), `I orientation ${rotation} aligns with recorded geometry`);
    }
    // Exercise the complete scoring pipeline, not just the plan adapter.
    const source = { board: empty(), current: 'T', next: [...'IOLJSZTIOL'], hold: null };
    const scored = await context.scoreRecordedOperation(0, source, { type: 'T', rotation: 0, x: 4, y: 39 }, {}, 120000, 120000, Infinity, 12, true);
    check(scored.result.status === 'scored' && scored.result.searchEngine === 'wasm', 'full viewer scoring uses WASM');
    check(scored.result.aiPlan.length >= 5, 'full scoring retains a searched multi-move plan');
    same(key(scored.result.aiPlan[0]), key(scored.result.bestMove), 'displayed route begins at the scored best move');
    const positions = [
        { rows: ['......#...', '.....###..', '....####..', '..#.#####.', '.##.#####.', '###.######', '###.######', '###.######'], current: 'J', next: 'ILOTSZJTOZ', hold: 'T' },
        { rows: ['..###.....', '..####....', '..#####...', '#######...', '#######.##'], current: 'L', next: 'JIOSTZLZTL', hold: 'Z' },
        { rows: ['..##.#....', '..######..', '#########.'], current: 'S', next: 'JZSLOTITJZ', hold: 'O' }
    ];
    for (const position of positions) {
        const board = empty();
        position.rows.forEach((row, i) => { board[40-position.rows.length+i] = [...row].map(c => c === '#' ? 'G' : null); });
        const source = { current: position.current, next: [...position.next], hold: position.hold };
        const handle = bridge.create({ ...snapshot, board, currentPiece: source.current, nextQueue: source.next, holdPiece: source.hold, isB2B: true, ren: -1 });
        try {
            bridge.think(handle, 3000, 120000);
            const plan = bridge.plan(handle, 12);
            const search = context.createSearch(board, source, { b2b: true, ren: -1 });
            const bestMove = bridge.candidates(handle).sort((a,b) => b.value-a.value || b.spike-a.spike)[0];
            const bestEdge = search.root.children.find(edge => context.sameScoredMove(context.publicMove(edge), bestMove));
            const variation = context.analysisVariation(search, { bestMove, bestEdge }, 12, { bridge, handle });
            check(plan.length > 1, 'midgame tree has a searched route');
            same(variation.map(key), plan.map(key), 'midgame plan preserves WASM rotations, HOLD and continuation');
        } finally { bridge.destroy(handle); }
    }
    console.log(JSON.stringify({ passed: true, checks }));
})().catch(error => { console.error(error); process.exitCode = 1; });
