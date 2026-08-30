/* KASANE worker: complete snapshot in, tactical placement + wait timing out. */

'use strict';

importScripts('./kasane-wasm.js?v=kasane-v8');

let bridge = null;
let generation = 0;

const ready = KasaneWasmBridge.load().then(value => {
    bridge = value;
    return value;
});

async function analyze(data) {
    const token = generation;
    const started = performance.now();
    const move = bridge.choose(data.snapshot);
    const searchElapsedMs = performance.now() - started;
    if (token !== generation) return;

    if (!move) {
        self.postMessage({
            type: 'noLegalMove',
            requestId: data.requestId ?? null,
            reason: 'no-legal-placement'
        });
        return;
    }

    // The snapshot timestamp precedes synchronous WASM inference. Consume that
    // real elapsed time from a tactical wait so the requested lock timestamp
    // remains stable, but never turn the AI setting into an artificial minimum
    // delay (Cold Clear treats it as a search budget too).
    move.waitMs = Math.max(0, Math.round(move.waitMs - searchElapsedMs));
    move.searchElapsedMs = Math.round(searchElapsedMs);
    const modelLabel = data.model === 'kasane-base'
        ? 'KASANE Base v3'
        : data.model === 'kasane-guard'
            ? 'KASANE Guard v1'
            : data.model === 'kasane-strategy'
                ? 'KASANE Strategy v2'
                : data.model === 'kasane-stack-ren'
                    ? 'KASANE Stack-REN v3'
                    : 'KASANE Basic v1';
    const strategyLabel = move.strategyEvent && move.strategyEvent !== 'None'
        ? ` | ${move.strategyEvent}`
        : '';
    const overrideLabel = move.policyOverride && move.policyOverride !== 'None'
        ? ` | override ${move.policyOverride}`
        : '';
    const wellWidth = (move.strategyDetail >>> 4) & 0x0f;
    const wellStart = move.strategyDetail & 0x0f;
    const wellLabel = wellWidth >= 2 && wellWidth <= 4 && wellStart + wellWidth <= 10
        ? ` | well C${wellStart + 1}-C${wellStart + wellWidth}`
        : '';
    self.postMessage({
        type: 'debug',
        message: `${modelLabel} | ${move.intent}${strategyLabel}${overrideLabel}${wellLabel} | score ${move.score.toFixed(2)} | wait ${move.waitMs} ms | search ${move.searchElapsedMs} ms`
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
            bridge.resetController();
            generation++;
            break;
        case 'invalidate':
            bridge.invalidateSearch();
            generation++;
            break;
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
