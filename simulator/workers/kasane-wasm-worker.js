/* KASANE worker: complete snapshot in, tactical placement + wait timing out. */

'use strict';

importScripts('./kasane-wasm.js?v=kasane-v2');

let bridge = null;
let generation = 0;

const ready = KasaneWasmBridge.load().then(value => {
    bridge = value;
    return value;
});

function delay(milliseconds) {
    return new Promise(resolve => setTimeout(resolve, milliseconds));
}

async function analyze(data) {
    const token = generation;
    const started = performance.now();
    const move = bridge.choose(data.snapshot);
    const searchElapsedMs = performance.now() - started;
    const decisionLatencyMs = Math.max(0, Number(data.decisionLatencyMs) || 0);
    if (searchElapsedMs < decisionLatencyMs) {
        await delay(decisionLatencyMs - searchElapsedMs);
    }
    if (token !== generation) return;

    if (!move) {
        self.postMessage({ type: 'move', requestId: data.requestId ?? null });
        return;
    }

    // Consume search overrun from tactical waiting so the simulator's chosen
    // AI think-time remains the timing source of truth on every device.
    const overrunMs = Math.max(0, searchElapsedMs - decisionLatencyMs);
    move.waitMs = Math.max(0, Math.round(move.waitMs - overrunMs));
    move.searchElapsedMs = Math.round(searchElapsedMs);
    const modelLabel = data.model === 'kasane-base'
        ? 'KASANE Base v3'
        : data.model === 'kasane-guard'
            ? 'KASANE Guard v1'
            : 'KASANE Basic v1';
    self.postMessage({
        type: 'debug',
        message: `${modelLabel} | ${move.intent} | score ${move.score.toFixed(2)} | wait ${move.waitMs} ms | search ${move.searchElapsedMs} ms`
    });
    self.postMessage({
        type: 'move',
        requestId: data.requestId ?? null,
        model: data.model,
        ...move
    });
}

async function handle(data) {
    switch (data.type) {
        case 'analyze':
            await analyze(data);
            break;
        case 'reset':
        case 'pause':
        case 'stop':
            generation++;
            break;
        // KASANE is snapshot-driven, so commit and rolling preview events are
        // deliberately no-ops. The next analyze message contains exact state.
        case 'commit':
        case 'addNextPiece':
            break;
    }
}

self.onmessage = event => {
    ready.then(() => handle(event.data || {})).catch(error => {
        self.postMessage({ type: 'error', message: String(error && error.stack || error) });
    });
};
