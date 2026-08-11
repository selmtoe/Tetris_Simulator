/*
 * Worker bridge for the simulator-owned Cold Clear standard-mode port.
 * The core is intentionally separate from the reference archive supplied in
 * cold-clear-master.zip; see cold-clear-core.js for provenance and licensing.
 */

'use strict';

importScripts('./cold-clear-core.js');

let search = null;
let weightsSignature = null;
let backgroundTimer = null;
let backgroundToken = 0;
let backgroundNodeLimit = 120000;

function stopBackground() {
    backgroundToken++;
    if (backgroundTimer !== null) {
        clearTimeout(backgroundTimer);
        backgroundTimer = null;
    }
}

function postNodeCount() {
    if (!search) return;
    self.postMessage({ type: 'nodeCount', count: search.nodeCount, template: null, shift: 0 });
}

// A Worker slice is deliberately short.  It keeps accepting commit/reset/new
// preview messages while the simulator is animating the previous placement,
// without bringing back the old permanent busy loop.
function startBackground(snapshot) {
    if (!search || snapshot.background === false) return;
    stopBackground();
    backgroundNodeLimit = snapshot.nodeLimit;
    const token = ++backgroundToken;
    const run = () => {
        backgroundTimer = setTimeout(() => {
            backgroundTimer = null;
            if (token !== backgroundToken || !search) return;
            const result = search.thinkAhead(8, backgroundNodeLimit);
            if (result.nodesAdded > 0) postNodeCount();
            if (token === backgroundToken && search.nodeCount < backgroundNodeLimit && result.nodesAdded > 0) run();
        }, 0);
    };
    run();
}

function signatureFor(weights) {
    try {
        return JSON.stringify(weights || {});
    } catch (_) {
        return '';
    }
}

function makeSearch(weights) {
    const signature = signatureFor(weights);
    if (!search || signature !== weightsSignature) {
        search = new self.ColdClearSimulatorCore.Search(weights);
        weightsSignature = signature;
        return true;
    }
    return false;
}

function normalizedSnapshot(data) {
    return {
        board: Array.isArray(data.board) ? data.board : [],
        currentPiece: data.currentPiece,
        nextQueue: Array.isArray(data.nextQueue) ? data.nextQueue : [],
        holdPiece: data.holdPiece || null,
        canHold: data.canHold !== false,
        isB2B: Boolean(data.isB2B),
        ren: Number.isFinite(data.ren) ? data.ren : -1,
        incoming: Math.max(0, Number.isFinite(data.incoming) ? data.incoming : 0),
        // A bounded synchronous slice lets the Worker process commit/reset
        // messages between pieces and avoids the old permanent busy loop.
        thinkTimeMs: Math.max(8, Math.min(500, Number.isFinite(data.thinkTimeMs) ? data.thinkTimeMs : 180)),
        nodeLimit: Math.max(5000, Math.min(200000, Number.isFinite(data.nodeLimit) ? data.nodeLimit : 120000)),
        background: data.background !== false
    };
}

function legacySnapshot(data) {
    let board = [];
    try { board = JSON.parse(data.boardString || '[]'); } catch (_) { board = []; }
    const queue = typeof data.nextString === 'string' ? data.nextString.split('') : [];
    return {
        board,
        currentPiece: queue.shift() || null,
        nextQueue: queue,
        holdPiece: data.holdPiece || null,
        canHold: data.canHold !== false,
        isB2B: Boolean(data.isB2B),
        ren: Number.isFinite(data.ren) ? data.ren - 1 : -1,
        incoming: 0,
        thinkTimeMs: 180,
        nodeLimit: 120000,
        background: false
    };
}

function postAnalysis(data) {
    stopBackground();
    const weightsChanged = makeSearch(data.weights);
    const snapshot = normalizedSnapshot(data);
    const result = search.analyze(snapshot);
    const reset = weightsChanged || result.reset;
    const status = reset
        ? 'Tree RESET (Cold Clear DAG)'
        : result.revealed > 0
            ? `Tree REUSED (+${result.revealed} preview, Cold Clear DAG)`
            : 'Tree REUSED / thinking ahead (Cold Clear DAG)';
    self.postMessage({ type: 'debug', message: status });
    postNodeCount();
    self.postMessage(result.move
        ? { type: 'move', requestId: data.requestId ?? null, ...result.move }
        : { type: 'move', requestId: data.requestId ?? null, move: null });
    if (result.move) startBackground(snapshot);
}

function runSelfTest() {
    const board = new self.ColdClearSimulatorCore.Board();
    const counts = {};
    for (const piece of ['I', 'O', 'T', 'L', 'J', 'S', 'Z']) {
        counts[piece] = self.ColdClearSimulatorCore.findMoves(board, piece).length;
    }
    const passed = Object.values(counts).every(count => count > 0);
    self.postMessage({ type: 'selfTest', passed, counts });
}

self.onmessage = event => {
    const data = event.data || {};
    try {
        if (data.type === 'analyze') {
            postAnalysis(data);
        } else if (data.type === 'commit') {
            if (search && search.commit()) startBackground({ nodeLimit: backgroundNodeLimit, background: true });
        } else if (data.type === 'addNextPiece') {
            if (search) {
                const resolved = search.addNextPiece(data.piece);
                if (resolved.added) postNodeCount();
            }
        } else if (data.type === 'reset') {
            stopBackground();
            search = null;
            weightsSignature = null;
        } else if (data.type === 'pause' || data.type === 'stop') {
            stopBackground();
        } else if (data.type === 'selfTest') {
            runSelfTest();
        } else if (data.type === 'start') {
            // Temporary compatibility for saved pages using the former protocol.
            const snapshot = legacySnapshot(data);
            postAnalysis({ ...data, ...snapshot, type: 'analyze' });
        }
    } catch (error) {
        self.postMessage({ type: 'error', message: String(error && error.stack || error) });
    }
};
