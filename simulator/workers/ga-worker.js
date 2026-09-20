'use strict';
// Same single evaluator/search policy used by the offline GA and simulator.
// Cold Clear covered source and license: ../../licenses/index.html
importScripts('./cold-clear-wasm.js?v=ga-v1', '../../ai-lab/policy.js?v=ga-v1');

let policy = null;
let currentNodes = 0;
let currentThinkMs = 0;
let backgroundTimer = null;
let backgroundToken = 0;
function stopBackground() {
    backgroundToken++;
    if (backgroundTimer !== null) clearTimeout(backgroundTimer);
    backgroundTimer = null;
}
function startBackground(enabled) {
    stopBackground();
    if (!enabled || !policy) return;
    const token = backgroundToken;
    const tick = () => {
        backgroundTimer = null;
        if (token !== backgroundToken || !policy) return;
        try {
            const progress = policy.thinkAhead(8);
            if (progress.nodesAdded > 0) self.postMessage({ type: 'nodeCount', count: progress.count });
            if (!progress.done && token === backgroundToken) backgroundTimer = setTimeout(tick, 0);
        } catch (error) {
            stopBackground();
            self.postMessage({ type: 'error', message: error.stack || String(error) });
        }
    };
    backgroundTimer = setTimeout(tick, 0);
}
const ready = Promise.all([
    ColdClearWasmBridge.load('../../ai-lab/build/cold-clear.wasm?v=ga-v1'),
    fetch('../../ai-lab/models/ga-v1.json?v=ga-v1').then(response => {
        if (!response.ok) throw new Error(`GA model HTTP ${response.status}`);
        return response.json();
    })
]);
let messages = Promise.resolve();
self.onmessage = event => {
    const data = event.data || {};
    messages = messages.then(async () => {
        const [bridge, model] = await ready;
        if (data.type === 'analyze') {
            stopBackground();
            if (!data.opponent?.board) throw new Error('GA v1 requires an opponent: select 2P mode.');
            const nodes = Math.max(1, Math.floor(Number(data.nodeLimit) || 120000));
            const maxThinkMs = Math.max(1, Number(data.thinkTimeMs) || 50);
            if (!policy || currentNodes !== nodes || currentThinkMs !== maxThinkMs) {
                policy?.close();
                policy = AiLabPolicy.createPolicyFromBridge(bridge, { genome: model.genome || model, nodes, maxThinkMs });
                currentNodes = nodes;
                currentThinkMs = maxThinkMs;
            }
            const move = policy.choose(data);
            if (move?.noLegalMove && move.verified) {
                self.postMessage({ type: 'noLegalMove', requestId: data.requestId, reason: 'no-legal-placement' });
            } else if (move) {
                self.postMessage({ type: 'move', requestId: data.requestId, ...move });
                self.postMessage({ type: 'nodeCount', count: move.stats.afterNodes });
                startBackground(data.background !== false);
            } else throw new Error('GA search returned no verified result');
        } else if (data.type === 'reset') {
            stopBackground();
            policy?.reset();
        } else if (data.type === 'commit') {
            stopBackground();
            policy?.commit();
        } else if (data.type === 'addNextPiece') {
            policy?.addNextPiece(data.piece);
        } else if (data.type === 'pause' || data.type === 'stop') {
            stopBackground();
        }
    }).catch(error => {
        stopBackground();
        self.postMessage({ type: 'error', message: error.stack || String(error) });
    });
};
