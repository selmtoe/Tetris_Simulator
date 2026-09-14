/* Cold Clear worker backed by the original Rust/WASM Standard implementation. */

'use strict';

importScripts('./cold-clear-wasm.js?v=search-draws-v4');

let bridge = null;
let search = 0;
let weightsSignature = '';
let backgroundTimer = null;
let backgroundToken = 0;
let backgroundNodeLimit = 120000;
let decisionNodeLimit = 0;
let decisionNodesAdded = 0;

const ready = ColdClearWasmBridge.load().then(value => {
    bridge = value;
    return value;
});

function stopBackground() {
    backgroundToken++;
    if (backgroundTimer !== null) {
        clearTimeout(backgroundTimer);
        backgroundTimer = null;
    }
}

function postNodeCount() {
    if (!bridge || !search) return;
    self.postMessage({ type: 'nodeCount', count: bridge.nodeCount(search), template: null, shift: 0 });
}

function signatureFor(weights) {
    try { return JSON.stringify(weights || {}); } catch (_) { return ''; }
}

function makeSearch(snapshot, force = false) {
    const signature = signatureFor(snapshot.weights);
    if (!search || force || signature !== weightsSignature) {
        // Construct the replacement before releasing the retained DAG.  If
        // allocation fails, the existing search remains valid and the Worker
        // reports the actual error instead of losing both states.
        const previous = search;
        search = bridge.create(snapshot);
        weightsSignature = signature;
        decisionNodeLimit = 0;
        decisionNodesAdded = 0;
        if (previous) bridge.destroy(previous);
        return true;
    }
    return false;
}

function beginDecision(snapshot) {
    const budget = Math.max(1, Math.floor(Number(snapshot.nodeLimit) || 1));
    decisionNodeLimit = bridge.beginDecision(search, budget, snapshot.incoming || 0);
    decisionNodesAdded = 0;
    backgroundNodeLimit = decisionNodeLimit;
}

function think(snapshot, milliseconds) {
    if (!decisionNodeLimit) beginDecision(snapshot);
    const result = bridge.think(search, milliseconds, decisionNodeLimit);
    decisionNodesAdded += result.nodesAdded;
    return result;
}

function startBackground(snapshot) {
    if (!search || snapshot.background === false) return;
    stopBackground();
    backgroundNodeLimit = decisionNodeLimit;
    const token = ++backgroundToken;
    const run = () => {
        backgroundTimer = setTimeout(() => {
            backgroundTimer = null;
            if (token !== backgroundToken || !search) return;
            const result = think(snapshot, 8);
            if (result.nodesAdded > 0) postNodeCount();
            if (token === backgroundToken && bridge.nodeCount(search) < backgroundNodeLimit && result.nodesAdded > 0) run();
        }, 0);
    };
    run();
}

async function postAnalysis(data) {
    stopBackground();
    let reset = makeSearch(data);
    if (!decisionNodeLimit) beginDecision(data);
    think(data, data.thinkTimeMs || 180);
    let move = bridge.suggest(search, data.incoming || 0);
    let budgetRecovery = false;

    // Cold Clear's max_nodes applies to every node retained in its DAG, not
    // just to the current root.  At a deliberately small league budget, a
    // commit can retain >= max_nodes while the newly exposed root generation
    // has not been expanded yet.  suggest_move() then returns None even though
    // legal placements exist.  Re-seed once from Player's authoritative
    // snapshot before treating None as a real top-out.  Successful searches
    // (including the normal 120k-node production path) are left untouched.
    const nodeLimit = Math.max(1, Math.floor(Number(data.nodeLimit) || 1));
    const retainedNodes = bridge.nodeCount(search);
    const legacyAbsoluteCapReached = bridge.supportsDecisionTopUp === false
        && retainedNodes >= nodeLimit;
    if (!move && !reset && (retainedNodes >= decisionNodeLimit || legacyAbsoluteCapReached)) {
        makeSearch(data, true);
        reset = true;
        budgetRecovery = true;
        beginDecision(data);
        think(data, data.thinkTimeMs || 180);
        move = bridge.suggest(search, data.incoming || 0);
    }

    const status = budgetRecovery
        ? 'Tree RESET (Cold Clear retained-DAG budget exhausted)'
        : reset
            ? 'Tree RESET (Cold Clear reference WASM)'
            : 'Tree REUSED / reference WASM thinking ahead';
    self.postMessage({
        type: 'debug',
        message: `${status}; decision new nodes=${decisionNodesAdded}`
    });
    self.postMessage({
        type: 'decisionWork',
        nodesAdded: decisionNodesAdded,
        absoluteLimit: decisionNodeLimit,
        budget: Math.max(1, Math.floor(Number(data.nodeLimit) || 1))
    });
    postNodeCount();
    if (!move) {
        self.postMessage({
            type: 'noLegalMove',
            requestId: data.requestId ?? null,
            reason: 'no-legal-placement'
        });
        return;
    }
    self.postMessage({
        type: 'move',
        requestId: data.requestId ?? null,
        ...move
    });
    startBackground(data);
}

async function handle(data) {
    if (data.type === 'analyze') {
        await postAnalysis(data);
    } else if (data.type === 'commit') {
        stopBackground();
        if (search && bridge.commit(search)) {
            // The next decision is opened by its authoritative analyze
            // snapshot, which supplies the post-cancellation incoming value
            // used by the shared native/wasm deterministic seed contract.
            decisionNodeLimit = 0;
            decisionNodesAdded = 0;
        }
    } else if (data.type === 'addNextPiece') {
        if (search && bridge.addNextPiece(search, data.piece)) postNodeCount();
    } else if (data.type === 'reset') {
        stopBackground();
        if (search) bridge.destroy(search);
        search = 0;
        weightsSignature = '';
        decisionNodeLimit = 0;
        decisionNodesAdded = 0;
    } else if (data.type === 'pause' || data.type === 'stop') {
        stopBackground();
    }
}

self.onmessage = event => {
    ready.then(() => handle(event.data || {})).catch(error => {
        self.postMessage({ type: 'error', message: String(error && error.stack || error) });
    });
};
