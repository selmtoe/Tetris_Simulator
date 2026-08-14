/* Cold Clear worker backed by the original Rust/WASM Standard implementation. */

'use strict';

importScripts('./cold-clear-wasm.js');

let bridge = null;
let search = 0;
let weightsSignature = '';
let backgroundTimer = null;
let backgroundToken = 0;
let backgroundNodeLimit = 120000;

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

function makeSearch(snapshot) {
    const signature = signatureFor(snapshot.weights);
    if (!search || signature !== weightsSignature) {
        if (search) bridge.destroy(search);
        search = bridge.create(snapshot);
        weightsSignature = signature;
        return true;
    }
    return false;
}

function think(snapshot, milliseconds) {
    return bridge.think(search, milliseconds, snapshot.nodeLimit);
}

function startBackground(snapshot) {
    if (!search || snapshot.background === false) return;
    stopBackground();
    backgroundNodeLimit = snapshot.nodeLimit;
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
    const reset = makeSearch(data);
    think(data, data.thinkTimeMs || 180);
    const move = bridge.suggest(search, data.incoming || 0);
    const status = reset
        ? 'Tree RESET (Cold Clear reference WASM)'
        : 'Tree REUSED / reference WASM thinking ahead';
    self.postMessage({ type: 'debug', message: status });
    postNodeCount();
    self.postMessage({
        type: 'move',
        requestId: data.requestId ?? null,
        ...(move || {})
    });
    if (move) startBackground(data);
}

async function handle(data) {
    if (data.type === 'analyze') {
        await postAnalysis(data);
    } else if (data.type === 'commit') {
        if (search && bridge.commit(search)) startBackground({ nodeLimit: backgroundNodeLimit, background: true });
    } else if (data.type === 'addNextPiece') {
        if (search && bridge.addNextPiece(search, data.piece)) postNodeCount();
    } else if (data.type === 'reset') {
        stopBackground();
        if (search) bridge.destroy(search);
        search = 0;
        weightsSignature = '';
    } else if (data.type === 'pause' || data.type === 'stop') {
        stopBackground();
    }
}

self.onmessage = event => {
    ready.then(() => handle(event.data || {})).catch(error => {
        self.postMessage({ type: 'error', message: String(error && error.stack || error) });
    });
};
