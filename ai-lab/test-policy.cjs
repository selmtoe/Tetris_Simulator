'use strict';

// ABI/policy tests; no simulator, trainer, saved model, or Kasane dependencies.
const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { createPolicy, createPolicyFromBridge } = require('./policy.js');
const wasmPath = path.join(__dirname, 'build/cold-clear.wasm');
const compiled = WebAssembly.compile(fs.readFileSync(wasmPath));
const clean = value => JSON.parse(JSON.stringify(value));
const empty = () => Array.from({ length: 40 }, () => Array(10).fill(null));
const snapshot = () => ({ board: empty(), currentPiece: 'T', nextQueue: [...'IOLJSZTI'], holdPiece: null, canHold: true, isB2B: false, ren: -1, incoming: 0 });

// Read the actual upstream Default solely for parity testing. This is never
// used for population initialization or production custom-genome creation.
function standardGenome() {
    const source = fs.readFileSync(path.join(__dirname, '../third_party/cold-clear-reference/bot/src/evaluation/standard.rs'), 'utf8');
    const block = source.split('impl Default for Standard {')[1].split('impl Standard {')[0];
    const weights = {};
    for (const [, name, value] of block.matchAll(/^\s+(\w+):\s*(-?\d+|true|false|\[[^\]]+\]),/gm)) weights[name] = JSON.parse(value);
    return { schema: 1, origin: 'test-only-actual-upstream-default-clone', weights,
        opponent: { pressureAttack: 0, pressureHeight: 0, threatSafety: 0 } };
}

async function bridge() {
    const context = { self: {}, TextEncoder, Uint8Array, DataView };
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../simulator/workers/cold-clear-wasm.js'), 'utf8'), context);
    return new context.self.ColdClearWasmBridge(await WebAssembly.instantiate(await compiled, {}));
}

function directSuggest(b, state, weights, nodes) {
    const handle = b.create({ ...state, nodeLimit: nodes, weights });
    try {
        const target = b.beginDecision(handle, nodes, state.incoming);
        while (b.nodeCount(handle) < target) b.exports.cc_think(handle, 1);
        return clean(b.suggest(handle, state.incoming));
    } finally { b.destroy(handle); }
}

test('full custom genome is mandatory; metadata accepted; no hidden defaults', async () => {
    const genome = standardGenome();
    for (const name of ['height', 'tslot', 'use_bag']) {
        const bad = clean(genome); delete bad.weights[name];
        await assert.rejects(createPolicy({ genome: bad, wasmPath }), /requires exactly/);
    }
    const bad = clean(genome); bad.weights.height = NaN;
    await assert.rejects(createPolicy({ genome: bad, wasmPath }), /integer/);
    delete genome.opponent.threatSafety;
    await assert.rejects(createPolicy({ genome, wasmPath }), /requires exactly/);
});

test('zero opponent coefficients match normal CC suggestion exactly at fixed node targets', async () => {
    for (const incoming of [0, 7, 20]) for (const nodes of [1, 300, 1500]) {
        const state = snapshot(); state.incoming = incoming;
        const genome = standardGenome();
        const b = await bridge();
        // Bypass must never call select, regardless of opponent observation.
        b.select = () => { throw new Error('zero coefficients called select'); };
        const policy = createPolicyFromBridge(b, { genome, nodes });
        try {
            const move = policy.choose({ ...state, opponent: { board: empty(), incoming: 999 } });
            const { stats, ...actual } = move;
            const expected = directSuggest(await bridge(), state, genome.weights, nodes);
            assert.deepEqual(clean(actual), expected);
            assert.equal(stats.targetNodes, stats.beforeNodes + nodes);
            assert.equal(stats.budgetReached, true);
        } finally { policy.close(); }
    }
});

test('empty HOLD accepts addNextPiece before commit and retains the correct DAG', async () => {
    const policy = await createPolicy({ nodes: 300, wasmPath });
    const state = snapshot();
    try {
        const move = policy.choose(state);
        assert.equal(move.hold, true); assert.equal(move.piece, 'I'); assert.equal(move.rotation, 0);
        // Independent expected horizontal I board + two queue consumptions.
        for (let x = move.x; x < move.x + 4; x++) state.board[move.y][x] = 'I';
        policy.addNextPiece('Z'); // Empty HOLD -> immediate spawn -> new preview.
        assert.equal(policy.commit(), true);
        policy.addNextPiece('L'); // Post-lock spawn -> new preview.
        state.currentPiece = 'O'; state.nextQueue = [...'LJSZTIZL']; state.holdPiece = 'T';
        const next = policy.choose(state);
        assert.equal(next.stats.recreated, false);
        assert.equal(next.stats.targetNodes, next.stats.beforeNodes + 300);
        assert.equal(policy.stats.commits, 1);
        assert.throws(() => policy.choose(state), /commit/);
        policy.reset();
        assert.equal(policy.choose(state).stats.recreated, true);
    } finally { policy.close(); policy.close(); }
});

test('authoritative B2B/board differences invalidate retained state', async () => {
    const p = await createPolicy({ nodes: 1, wasmPath });
    try {
        const s = snapshot();
        p.choose(s); p.commit();
        s.isB2B = true; s.board[39][0] = 'G';
        assert.equal(p.choose(s).stats.recreated, true);
        assert.equal(p.stats.recreations, 1);
    } finally { p.close(); }
});

test('arbitrary root select returns an SRS path, enforces safety, and commits', async () => {
    const b = await bridge();
    const h = b.create({ ...snapshot(), nodeLimit: 300 });
    try {
        b.beginDecision(h, 300, 20); b.exports.cc_think(h, 300);
        const normal = b.suggest(h, 20);
        const roots = b.candidates(h);
        let rejected = 0, selected;
        for (let i = 0; i < roots.length; i++) {
            const result = b.select(h, i, 20);
            if (!result) rejected++;
            else {
                assert.equal(result.x, roots[i].x); assert.equal(result.piece, roots[i].piece);
                assert.equal(result.hold, roots[i].hold); assert.ok(Array.isArray(result.inputs));
                selected = result;
            }
        }
        assert.ok(rejected > 0 && selected && normal);
        assert.equal(b.select(h, 0xffffffff, 20), null);
        assert.equal(b.commit(h), true);
        assert.equal(b.commit(h), false);
        assert.equal(b.exports.cc_has_legal_move(h), 1);
    } finally { b.destroy(h); }
});

test('opponent pressure can change root choice while retaining safety', async () => {
    const genome = standardGenome(); genome.opponent.pressureHeight = 100000;
    const low = await createPolicy({ genome, nodes: 300, wasmPath });
    const high = await createPolicy({ genome, nodes: 300, wasmPath });
    try {
        const s = snapshot();
        const a = low.choose({ ...s, opponent: { maxHeight: 0, holes: 0, incoming: 0 } });
        const b = high.choose({ ...s, opponent: { maxHeight: 20, holes: 10, incoming: 10 } });
        assert.equal(a.stats.reranked, false);
        // Use a stack that differentiates candidate heights if the empty-board
        // normal suggestion already has minimum height.
        if (!b.stats.reranked) {
            low.reset(); high.reset();
            for (let y = 34; y < 40; y++) for (let x = 0; x < 6; x++) s.board[y][x] = 'G';
            const c = low.choose({ ...s, opponent: { maxHeight: 0 } });
            const d = high.choose({ ...s, opponent: { maxHeight: 20, holes: 10, incoming: 10 } });
            assert.equal(d.stats.reranked, true);
            assert.notDeepEqual([c.piece, c.rotation, c.x, c.y], [d.piece, d.rotation, d.x, d.y]);
        }
    } finally { low.close(); high.close(); }
});

test('no hidden previews, no wall-clock search; browser import needs no require', async () => {
    const context = { TextEncoder, Uint8Array, DataView };
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, 'policy.js'), 'utf8'), context);
    assert.equal(typeof context.AiLabPolicy.createPolicyFromBridge, 'function');
    const a = context.AiLabPolicy.createPolicyFromBridge(await bridge(), { nodes: 300 });
    const b = await createPolicy({ nodes: 300, wasmPath });
    try {
        const s = snapshot();
        Object.defineProperty(s, 'hiddenNextQueue', { get() { throw new Error('hidden preview read'); } });
        Object.defineProperty(s, 'rng', { get() { throw new Error('private RNG read'); } });
        assert.deepEqual(clean(a.choose(s)), clean(b.choose(s)));
    } finally { a.close(); b.close(); }
});

test('no-suggestion is proved by legal search; failed search is never a fake loss', async () => {
    const b = await bridge();
    const p = createPolicyFromBridge(b, { nodes: 1 });
    try {
        b.suggest = () => null; // Simulate a failed suggestion on a legal board.
        assert.throws(() => p.choose(snapshot()), e => e.code === 'CC_SEARCH_EXHAUSTED');
        p.reset();
        const s = snapshot(); s.board = Array.from({ length: 40 }, () => Array(10).fill('G'));
        assert.deepEqual(p.choose(s).noLegalMove, true);
        assert.equal(p.choose(s).verified, true);
    } finally { p.close(); }
});

test('interactive time limit is opt-in; a large node target does not stall the browser', async () => {
    const p = createPolicyFromBridge(await bridge(), { nodes: 1000000, maxThinkMs: 1 });
    try {
        const move = p.choose(snapshot());
        assert.ok(move.piece);
        assert.equal(move.stats.timeLimited, true);
        assert.equal(move.stats.budgetReached, false);
        assert.equal(p.commit(), true);
    } finally { p.close(); }
});

test('background consumes only the current budget and preserves pending commit', async () => {
    const p = createPolicyFromBridge(await bridge(), { nodes: 3000, maxThinkMs: 0.001 });
    try {
        assert.equal(p.thinkAhead().done, true, 'no speculative search before analyze');
        const move = p.choose(snapshot());
        assert.equal(move.stats.timeLimited, true);
        assert.ok(move.stats.nodesAdded < 3000);
        let work;
        for (let i = 0; i < 100; i++) {
            work = p.thinkAhead(8);
            if (work.done) break;
        }
        assert.equal(work.done, true);
        assert.ok(p.stats.backgroundNodesAdded > 0);
        assert.ok(work.count >= move.stats.targetNodes);
        assert.equal(p.thinkAhead().nodesAdded, 0, 'reaching the cap never tops it up');
        assert.equal(p.stats.decisions, 1);
        assert.equal(p.commit(), true, 'the originally selected move still commits');
        assert.equal(p.thinkAhead().nodesAdded, 0, 'wait for authoritative next snapshot');
        p.reset();
        assert.equal(p.thinkAhead().nodesAdded, 0);
    } finally { p.close(); }
    assert.throws(() => p.thinkAhead(), /closed/);
});

test('offline policies cannot silently add wall-clock background work', async () => {
    const p = await createPolicy({ nodes: 300, wasmPath });
    try {
        const move = p.choose(snapshot());
        const work = p.thinkAhead(8);
        assert.equal(work.nodesAdded, 0);
        assert.equal(work.count, move.stats.afterNodes);
        assert.equal(p.stats.backgroundNodesAdded, 0);
        assert.equal(p.commit(), true);
    } finally { p.close(); }
});

test('captured 81500ms doomed-future position uses same-weight legal roots and commits', async () => {
    // Extracted from failing seed158643333 seat0. Occupancy only; no private
    // future queue or simulator dependency. Immediate legal moves remain after
    // the DAG prunes all continuations, so this must not become a fake loss.
    const bottomUp = [
        '###.######', '####.#####', '####.#####',
        '######.###', '######.###', '######.###', '######.###',
        '##.#######', '##.#######', '##.#######',
        '#########.', '#########.', '#########.',
        '.#########', '.#########', '.#########', '.#########', '.#########',
        '#########.', '..########', '........#.',
    ];
    const state = snapshot();
    bottomUp.forEach((row, y) => { state.board[39 - y] = [...row].map(c => c === '.' ? null : 'G'); });
    state.holdPiece = 'J'; state.nextQueue = [...'SOZLIOTI']; state.ren = 1;
    const genome = standardGenome();
    // Deliberately different coefficients verify fallback uses the supplied
    // evaluator. Keep opponent terms zero for exact direct-ABI comparison.
    genome.weights.height = -137;
    const p = await createPolicy({ genome, nodes: 3000, wasmPath });
    const b = await bridge();
    const h = b.create({ ...state, weights: genome.weights, nodeLimit: 3000 });
    try {
        const move = p.choose(state);
        assert.equal(move.stats.searchDoomed, true);
        assert.equal(move.stats.legalFallback, true);
        assert.equal(move.noLegalMove, undefined);
        b.beginDecision(h, 3000, 0);
        for (let i = 0; i < 6256 && !(b.exports.cc_search_status(h) & 1); i++) b.exports.cc_think(h, 1);
        assert.equal(b.suggest(h, 0), null);
        assert.equal(b.exports.cc_has_legal_move(h), 1);
        const fallback = b.suggestLegal(h, 0);
        const { stats, ...actual } = move;
        assert.deepEqual(clean(actual), clean(fallback));
        assert.ok(b.candidates(h).length > 0);
        assert.equal(b.commit(h), true);
        assert.equal(b.nodeCount(h), 1);
        assert.equal(p.commit(), true);
        p.addNextPiece('Z');
        assert.equal(p.stats.legalFallbacks, 1);
    } finally { p.close(); b.destroy(h); }
});
