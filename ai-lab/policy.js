(function (root, factory) {
    const api = factory();
    if (typeof module === 'object' && module.exports) module.exports = api;
    else root.AiLabPolicy = api;
})(globalThis, function () {
'use strict';

// Cold Clear Standard is the explicit upstream search/evaluation base. No
// Kasane policy, coefficients, model or trainer is loaded here.
const scalarWeights = `back_to_back bumpiness bumpiness_sq row_transitions height
top_half top_quarter jeopardy cavity_cells cavity_cells_sq overhang_cells
overhang_cells_sq covered_cells covered_cells_sq well_depth max_well_depth
b2b_clear clear1 clear2 clear3 clear4 tspin1 tspin2 tspin3 mini_tspin1 mini_tspin2
perfect_clear combo_garbage move_time wasted_t`.split(/\s+/);
const flags = ['use_bag', 'timed_jeopardy', 'stack_pc_damage'];
const opponentKeys = ['pressureAttack', 'pressureHeight', 'threatSafety'];
const weightKeys = [...scalarWeights, 'tslot', 'well_column', ...flags];
const modules = new Map();

function integer(value, name, min, max) {
    if (!Number.isInteger(value) || value < min || value > max) throw new TypeError(`${name} must be an integer in [${min}, ${max}]`);
    return value;
}

function exactKeys(value, keys, name) {
    if (!value || typeof value !== 'object' || Array.isArray(value) ||
        Object.keys(value).length !== keys.length || keys.some(key => !Object.hasOwn(value, key))) {
        throw new TypeError(`${name} requires exactly: ${keys.join(', ')}`);
    }
}

function validateGenome(genome) {
    if (genome === null) return null; // The only route to Standard defaults.
    if (!genome || typeof genome !== 'object' || Array.isArray(genome) ||
        !Object.hasOwn(genome, 'weights') || !Object.hasOwn(genome, 'opponent') ||
        Object.keys(genome).some(key => !['schema', 'origin', 'weights', 'opponent'].includes(key))) {
        throw new TypeError('genome requires weights and opponent; optional metadata: schema, origin');
    }
    exactKeys(genome.weights, weightKeys, 'genome.weights (no default coefficients)');
    const weights = {};
    for (const key of scalarWeights) weights[key] = integer(genome.weights[key], key, -2147483648, 2147483647);
    integer(weights.max_well_depth, 'max_well_depth', 0, 40);
    for (const [key, size] of [['tslot', 4], ['well_column', 10]]) {
        if (!Array.isArray(genome.weights[key]) || genome.weights[key].length !== size) throw new TypeError(`${key} needs ${size} integers`);
        weights[key] = genome.weights[key].map(n => integer(n, key, -2147483648, 2147483647));
    }
    for (const key of flags) {
        if (typeof genome.weights[key] !== 'boolean') throw new TypeError(`${key} must be boolean`);
        weights[key] = genome.weights[key];
    }
    exactKeys(genome.opponent, opponentKeys, 'genome.opponent');
    const opponent = {};
    for (const key of opponentKeys) {
        if (typeof genome.opponent[key] !== 'number' || !Number.isFinite(genome.opponent[key])) throw new TypeError(`${key} must be finite`);
        opponent[key] = genome.opponent[key];
    }
    return { weights, opponent };
}

function piece(value, name, nullable = false) {
    if (nullable && (value === null || value === undefined || value === '')) return null;
    if (typeof value !== 'string' || !/^[IOTLJSZ]$/.test(value)) throw new TypeError(`Invalid ${name}`);
    return value;
}

function normalizeBoard(board) {
    if (!Array.isArray(board) || board.length !== 40 || board.some(row => !Array.isArray(row) || row.length !== 10)) throw new TypeError('board must be 40x10, top-down');
    return board.map(row => row.map(cell => cell == null || cell === 0 || cell === false ? null : '#'));
}

function normalizeSnapshot(snapshot) {
    if (!snapshot || typeof snapshot !== 'object') throw new TypeError('snapshot is required');
    // The upstream ABI ignores can_hold. Do not silently allow illegal re-holds.
    if (snapshot.canHold === false) throw new Error('Policy requires a fresh piece decision with canHold=true');
    if (!Array.isArray(snapshot.nextQueue) || !snapshot.nextQueue.length) throw new TypeError('nextQueue must contain visible previews');
    return {
        board: normalizeBoard(snapshot.board),
        currentPiece: piece(snapshot.currentPiece, 'currentPiece'),
        nextQueue: snapshot.nextQueue.map(p => piece(p, 'nextQueue')),
        holdPiece: piece(snapshot.holdPiece, 'holdPiece', true),
        canHold: true,
        isB2B: !!snapshot.isB2B,
        ren: integer(snapshot.ren ?? -1, 'ren', -1, 1000000),
        incoming: integer(snapshot.incoming ?? 0, 'incoming', 0, 1000000),
    };
}

// Simulator anchor/cell convention, including the I offsets in lib.rs.
const shapes = {
    O: [[0, 0], [1, 0], [0, -1], [1, -1]],
    T: [[-1, 0], [0, 0], [1, 0], [0, -1]],
    L: [[-1, 0], [0, 0], [1, 0], [1, -1]],
    J: [[-1, 0], [0, 0], [1, 0], [-1, -1]],
    S: [[-1, 0], [0, 0], [0, -1], [1, -1]],
    Z: [[-1, -1], [0, -1], [0, 0], [1, 0]],
};
const iShapes = [
    [[0, 0], [1, 0], [2, 0], [3, 0]],
    [[2, -1], [2, 0], [2, 1], [2, 2]],
    [[0, 1], [1, 1], [2, 1], [3, 1]],
    [[1, -1], [1, 0], [1, 1], [1, 2]],
];
function cells(move) {
    return (move.piece === 'I' ? iShapes[move.rotation] : shapes[move.piece]).map(([x, y]) => {
        if (move.piece !== 'I') for (let r = 0; r < move.rotation; r++) [x, y] = [-y, x];
        return [x + move.x, y + move.y];
    });
}

function terrain(board) {
    const heights = Array(10).fill(0);
    let holes = 0;
    for (let x = 0; x < 10; x++) {
        for (let y = 0; y < 40; y++) {
            if (board[y][x] !== null) { if (!heights[x]) heights[x] = 40 - y; }
            else if (heights[x]) holes++;
        }
    }
    return { heights, maxHeight: Math.max(...heights), holes };
}

function project(snapshot, move) {
    const board = snapshot.board.map(row => row.slice());
    const occupied = cells(move);
    for (const [x, y] of occupied) {
        if (x < 0 || x >= 10 || y < 0 || y >= 40 || board[y][x] !== null) throw new Error('CC candidate does not fit the snapshot');
        board[y][x] = '#';
    }
    const remaining = board.filter(row => row.some(cell => cell === null));
    const lines = 40 - remaining.length;
    if (lines > 4) throw new Error('Snapshot contains uncleared full rows');
    while (remaining.length < 40) remaining.unshift(Array(10).fill(null));
    const shape = terrain(remaining);
    const hard = lines === 4 || move.tspin === 'full' || move.tspin === 'mini';
    let attack = move.tspin === 'full' ? [0, 2, 4, 6][lines]
        : move.tspin === 'mini' ? [0, 0, 1][lines] : [0, 0, 1, 2, 4][lines];
    if (attack === undefined) throw new Error('Invalid CC spin/clear combination');
    if (lines) attack += (hard && snapshot.isB2B ? 1 : 0) + [0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 4, 5][Math.min(13, snapshot.ren + 1)];
    const perfectClear = lines > 0 && shape.maxHeight === 0;
    if (perfectClear) attack = 10; // Player PC replaces damage and combo bonus.
    const queue = [snapshot.currentPiece, ...snapshot.nextQueue];
    const current = queue.shift();
    let held = snapshot.holdPiece;
    const played = move.hold ? (held || queue.shift()) : current;
    if (played !== move.piece) throw new Error('CC candidate hold/queue mismatch');
    if (move.hold) held = current;
    return {
        board: remaining, currentPiece: queue.shift(), nextQueue: queue, holdPiece: held,
        canHold: true,
        isB2B: perfectClear || (lines ? hard : move.tspin ? false : snapshot.isB2B),
        // Fingerprint MUST describe the native cache, not just Player's next
        // state; these rule differences then trigger authoritative resync.
        nativeB2B: lines ? hard : snapshot.isB2B,
        ren: lines ? snapshot.ren + 1 : -1,
        attack, lines, lockedOut: occupied.every(([, y]) => y < 20), ...shape,
    };
}

function obviousThreat(opponent, board) {
    const availability = p => opponent.currentPiece === p || (opponent.canHold !== false && opponent.holdPiece === p)
        ? 1 : opponent.nextQueue?.[0] === p ? 0.5 : 0;
    const filled = (x, y) => x < 0 || x >= 10 || y >= 40 || (y >= 0 && board[y][x] !== null);
    let threat = 0;
    for (const kind of ['I', 'T']) {
        const ready = availability(kind);
        if (!ready) continue;
        for (let rotation = 0; rotation < 4; rotation++) for (let x = -2; x < 10; x++) for (let y = 18; y < 40; y++) {
            const placement = cells({ piece: kind, rotation, x, y });
            if (placement.some(([cx, cy]) => cy < 0 || cy >= 40 || filled(cx, cy))) continue;
            if (!placement.some(([cx, cy]) => filled(cx, cy + 1))) continue;
            // I: unobstructed vertical drop. T: supported, three-corner slot;
            // this is a visible geometric warning, not a legal SRS forecast.
            if (kind === 'I' && placement.some(([cx, cy]) => board.slice(0, cy).some(row => row[cx] !== null))) continue;
            if (kind === 'T' && [[-1, -1], [-1, 1], [1, -1], [1, 1]].filter(([dx, dy]) => filled(x + dx, y + dy)).length < 3) continue;
            const lines = [...new Set(placement.map(([, cy]) => cy))].filter(cy => board[cy].every((cell, cx) => cell !== null || placement.some(([px, py]) => px === cx && py === cy))).length;
            if ((kind === 'I' && lines !== 4) || (kind === 'T' && (lines < 1 || lines > 3))) continue;
            threat = Math.max(threat, ready * ((kind === 'I' ? 4 : lines * 2) + (opponent.isB2B ? 1 : 0)));
        }
    }
    return threat;
}

function observation(opponent) {
    if (!opponent) return { pressure: 0, obviousAttack: 0, queued: 0 };
    const board = opponent.board ? normalizeBoard(opponent.board) : null;
    const t = board ? terrain(board) : { maxHeight: opponent.maxHeight ?? 0, holes: opponent.holes ?? 0 };
    for (const [key, value] of Object.entries(t)) if (key !== 'heights' && (!Number.isFinite(value) || value < 0)) throw new TypeError(`Invalid opponent ${key}`);
    const queued = integer(opponent.incoming ?? 0, 'opponent.incoming', 0, 1000000);
    return { pressure: Math.min(2, t.maxHeight / 20 + t.holes / 20 + queued / 10), queued,
        obviousAttack: board ? obviousThreat(opponent, board) : 0 };
}

// All three features are dimensionless; coefficients are CC score units.
// P = clamp(opponent height/20 + holes/20 + queued/10, 0, 2)
// T = own queued + max(0, visible I/T attack - opponent queued)
// score = CC.value + pressureAttack * P * max(0, attack-ownQueued)/10
//                  - pressureHeight * P * ownNextHeight/20
//                  - threatSafety * min(4,T/10)
//                       * max(0,ownNextHeight + max(0,T-attack) - 10)/10.
// Positive coefficients reward pressure attacks and penalize height/exposure.
// Values around 100 mean about 100 CC points per unit; there is NO implicit
// scaling by or initialization from Standard's weights. Zero is exact bypass.
function features(next, incoming, opponent) {
    const threat = incoming + Math.max(0, opponent.obviousAttack - opponent.queued);
    return {
        pressureAttack: opponent.pressure * Math.max(0, next.attack - incoming) / 10,
        pressureHeight: -opponent.pressure * next.maxHeight / 20,
        threatSafety: -Math.min(4, threat / 10) * Math.max(0, next.maxHeight + Math.max(0, threat - next.attack) - 10) / 10,
    };
}

function signature(state) {
    return JSON.stringify([state.board, state.currentPiece, state.nextQueue, state.holdPiece, state.isB2B, state.ren]);
}

/**
 * choose(snapshot) -> move|{noLegalMove:true,verified:true,stats}; commit() -> boolean;
 * addNextPiece(letter) -> boolean; reset() discards state; close() is idempotent.
 * Snapshots are production 40x10 top-down boards. Snapshot weights/budget fields
 * are ignored: immutable genome and nodes passed here are authoritative.
 * Call addNextPiece for each newly visible preview, including empty-HOLD
 * spawns before commit. Both the current and pending queue are updated. choose also
 * recreates the DAG on snapshot mismatch (e.g. garbage arrival), with telemetry.
 * stats is a detached cumulative snapshot; move.stats describes that decision.
 */
async function createPolicy({ genome = null, nodes = 1500, wasmPath, runtime } = {}) {
    validateGenome(genome);
    const loaded = runtime ?? await createRuntime({ wasmPath });
    return createPolicyFromBridge(loaded.bridge, { genome, nodes, wasmPath: loaded.wasmPath });
}

// A sequential worker may reuse this after closing its previous policy. Every
// match still creates fresh bot/search state and supplies all genome weights.
// Reusing the two linear-memory arenas avoids accumulating discarded instances
// before V8 happens to collect their much smaller JavaScript wrappers.
async function createRuntime({ wasmPath } = {}) {
    // Node-only loader. importScripts('policy.js') never executes require.
    const fs = require('node:fs');
    const path = require('node:path');
    const vm = require('node:vm');
    const resolved = path.resolve(wasmPath ?? path.join(__dirname, 'build/cold-clear.wasm'));
    const stat = await fs.promises.stat(resolved);
    const cacheKey = `${resolved}:${stat.size}:${stat.mtimeMs}`;
    if (!modules.has(cacheKey)) modules.set(cacheKey, fs.promises.readFile(resolved).then(bytes => WebAssembly.compile(bytes)));
    const instance = await WebAssembly.instantiate(await modules.get(cacheKey), {});
    const context = { self: {}, TextEncoder, Uint8Array, DataView };
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../simulator/workers/cold-clear-wasm.js'), 'utf8'), context);
    const bridge = new context.self.ColdClearWasmBridge(instance);
    return { bridge, wasmPath: resolved };
}

// Each policy must exclusively own its Bridge/WASM instance (RNG is per instance).
function createPolicyFromBridge(bridge, { genome = null, nodes = 1500, wasmPath = null, maxThinkMs = null } = {}) {
    const model = validateGenome(genome);
    integer(nodes, 'nodes', 1, 10000000);
    if (maxThinkMs !== null && (!Number.isFinite(maxThinkMs) || maxThinkMs <= 0)) throw new TypeError('maxThinkMs must be positive or null');
    const rerank = model !== null && opponentKeys.some(key => model.opponent[key] !== 0);
    const e = bridge.exports;
    for (const name of ['cc_create', 'cc_alloc', 'cc_dealloc', 'cc_move_size', 'cc_begin_decision', 'cc_think', 'cc_node_count', 'cc_suggest', 'cc_search_status', 'cc_suggest_legal', 'cc_has_legal_move', 'cc_commit', 'cc_add_next_piece', 'cc_destroy', ...(rerank ? ['cc_select', 'cc_candidate_count', 'cc_candidate_size', 'cc_write_candidates'] : [])]) {
        if (typeof e[name] !== 'function') throw new Error(`Candidate WASM missing ${name}; rebuild ai-lab/build/cold-clear.wasm`);
    }
    let handle = 0, expected = null, pending = null, closed = false, decisionTarget = 0;
    const totals = { decisions: 0, commits: 0, resets: 0, recreations: 0, nodesAdded: 0, overshoot: 0, reranked: 0, rejectedSelections: 0, noMove: 0, legalFallbacks: 0, backgroundNodesAdded: 0 };
    const alive = () => { if (closed) throw new Error('Policy is closed'); };
    return {
        get stats() { return { ...totals, nodes, maxThinkMs, base: 'Cold Clear Standard', customWeights: model !== null, wasmPath }; },
        choose(snapshot) {
            alive();
            if (pending) throw new Error('commit() or reset() before the next choose()');
            const state = normalizeSnapshot(snapshot);
            let recreated = false;
            if (!handle || signature(state) !== signature(expected)) {
                const replacement = bridge.create({ ...state, nodeLimit: nodes, weights: model?.weights ?? {} });
                if (handle) { bridge.destroy(handle); totals.recreations++; }
                handle = replacement;
                expected = state;
                recreated = true;
            }
            const before = bridge.nodeCount(handle);
            const target = bridge.beginDecision(handle, nodes, state.incoming);
            decisionTarget = target;
            let after = before, thinkCalls = 0, stalled = 0;
            // Offline always leaves this null: fully deterministic fixed work.
            // The interactive Worker explicitly opts into the UI time budget.
            const deadline = maxThinkMs === null ? Infinity : performance.now() + maxThinkMs;
            let timeLimited = false;
            const maxThinkCalls = nodes * 2 + 256;
            // One whole CC expansion is indivisible. The target is exactly
            // retained+nodes; actual allocation may overshoot by one expansion.
            // Offline has no deadline or wall-clock stopping criterion.
            while (after < target && thinkCalls < maxThinkCalls) {
                if (maxThinkMs !== null && thinkCalls > 0 && performance.now() >= deadline) {
                    timeLimited = true;
                    break;
                }
                const previous = after;
                after = e.cc_think(handle, 1) >>> 0;
                thinkCalls++;
                stalled = after === previous ? stalled + 1 : 0;
                // Dedup/backpropagation can do useful work without adding nodes.
                if (stalled >= 256 || (e.cc_search_status(handle) & 1)) break;
            }
            let move = bridge.suggest(handle, state.incoming);
            const stats = { requestedNodes: nodes, beforeNodes: before, targetNodes: target, afterNodes: after,
                nodesAdded: after - before, overshoot: Math.max(0, after - target), budgetReached: after >= target,
                thinkCalls, maxThinkCalls, iterationLimitHit: thinkCalls >= maxThinkCalls && after < target,
                stalled: stalled >= 256, searchDoomed: !!(e.cc_search_status(handle) & 1),
                recreated, timeLimited, reranked: false, candidates: 0, rejectedSelections: 0, legalFallback: false };
            if (stats.iterationLimitHit) {
                const error = new Error('CC deterministic search iteration bound exceeded');
                error.code = 'CC_SEARCH_LIMIT'; error.stats = stats; throw error;
            }
            if (!move && stats.searchDoomed) {
                move = bridge.suggestLegal(handle, state.incoming);
                stats.legalFallback = !!move;
                totals.legalFallbacks += Number(stats.legalFallback);
            }
            if (move && rerank) {
                const opp = observation(snapshot.opponent);
                const candidates = bridge.candidates(handle);
                stats.candidates = candidates.length;
                const roots = candidates.map((candidate, index) => {
                    const next = project(state, candidate);
                    const f = features(next, state.incoming, opp);
                    const adjustment = opponentKeys.reduce((sum, key) => sum + model.opponent[key] * f[key], 0);
                    const score = candidate.value + adjustment;
                    if (!Number.isFinite(score)) throw new Error('Opponent score overflow');
                    return { candidate, index, next, features: f, score, adjustment };
                });
                const same = c => c.piece === move.piece && c.hold === move.hold && c.rotation === move.rotation && c.x === move.x && c.y === move.y && c.tspin === move.tspin;
                const baseline = roots.find(root => same(root.candidate));
                if (!baseline) throw new Error('Normal suggestion is absent from candidate roots');
                const safe = root => !root.next.lockedOut && (state.incoming === 0 || root.next.heights.slice(3, 6).every(h => state.incoming - root.next.attack + h <= 20));
                const allowed = roots.filter(safe);
                // If CC cannot avoid incoming death, retain its max-spike escape.
                if (allowed.length) {
                    allowed.sort((a, b) => b.score - a.score || (a === baseline ? -1 : b === baseline ? 1 : a.index - b.index));
                    for (const root of allowed) {
                        if (root === baseline) break;
                        const selected = bridge.select(handle, root.index, state.incoming);
                        if (selected) {
                            move = selected;
                            stats.reranked = true;
                            stats.root = { ccValue: root.candidate.value, score: root.score, adjustment: root.adjustment, features: root.features, attack: root.next.attack, maxHeight: root.next.maxHeight };
                            break;
                        }
                        stats.rejectedSelections++;
                    }
                }
                stats.root ??= { ccValue: baseline.candidate.value, score: baseline.score, adjustment: baseline.adjustment, features: baseline.features, attack: baseline.next.attack, maxHeight: baseline.next.maxHeight };
                stats.opponent = opp;
            }
            totals.decisions++;
            totals.nodesAdded += stats.nodesAdded;
            totals.overshoot += stats.overshoot;
            totals.reranked += Number(stats.reranked);
            totals.rejectedSelections += stats.rejectedSelections;
            if (!move) {
                totals.noMove++;
                const legal = e.cc_has_legal_move(handle) !== 0;
                if (!legal) return { noLegalMove: true, verified: true, stats };
                const error = new Error('CC search exhausted without a suggestion, but legal placements exist');
                error.code = 'CC_SEARCH_EXHAUSTED';
                error.stats = stats;
                throw error;
            }
            pending = project(state, move);
            return { ...move, inputs: Array.from(move.inputs), stats };
        },
        // Interactive only: spend the remainder of THIS decision's node budget
        // while Player executes the already-selected move. Never suggest/select
        // again here, so the pending placement and its commit stay unchanged.
        // The deterministic offline path is explicitly a no-op.
        thinkAhead(milliseconds = 8) {
            alive();
            if (!Number.isFinite(milliseconds) || milliseconds <= 0) throw new TypeError('thinkAhead milliseconds must be positive');
            const before = handle ? bridge.nodeCount(handle) : 0;
            if (maxThinkMs === null || !handle || !pending || !decisionTarget) return { nodesAdded: 0, count: before, done: true };
            const deadline = performance.now() + Math.min(milliseconds, 8);
            let after = before, calls = 0, stalled = 0;
            while (after < decisionTarget && (calls === 0 || performance.now() < deadline)) {
                const previous = after;
                after = e.cc_think(handle, 1) >>> 0;
                calls++;
                stalled = after === previous ? stalled + 1 : 0;
                if (stalled >= 256 || (e.cc_search_status(handle) & 1)) break;
            }
            const added = Math.max(0, after - before);
            totals.backgroundNodesAdded += added;
            return { nodesAdded: added, count: after,
                done: after >= decisionTarget || stalled >= 256 || !!(e.cc_search_status(handle) & 1) };
        },
        commit() {
            alive();
            if (!pending) return false;
            if (!bridge.commit(handle)) throw new Error('CC commit failed');
            expected = { ...pending, isB2B: pending.nativeB2B };
            pending = null;
            decisionTarget = 0;
            totals.commits++;
            return true;
        },
        addNextPiece(next) {
            alive();
            piece(next, 'next piece');
            if (!handle) throw new Error('addNextPiece requires an active decision');
            if (!bridge.addNextPiece(handle, next)) throw new Error('CC addNextPiece failed');
            const append = state => {
                if (!state.currentPiece) state.currentPiece = next;
                else state.nextQueue.push(next);
            };
            append(expected);
            if (pending) append(pending);
            return true;
        },
        reset() {
            alive();
            if (handle) bridge.destroy(handle);
            handle = 0; expected = null; pending = null; decisionTarget = 0;
            totals.resets++;
        },
        close() {
            if (!closed && handle) bridge.destroy(handle);
            handle = 0; expected = null; pending = null; decisionTarget = 0; closed = true;
        },
    };
}

return { createPolicy, createPolicyFromBridge, createRuntime };
});
