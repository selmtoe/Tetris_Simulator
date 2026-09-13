/* Opt-in model/worker mapping. Candidate Worker/WASM loads only for the exact id. */

'use strict';

(function publish(root, factory) {
    const api = factory(root);
    if (typeof module === 'object' && module.exports) module.exports = api;
    if (root) root.KasaneStackRenCandidateHarness = api;
})(typeof window !== 'undefined' ? window : globalThis, root => {
    const MODEL_ID = 'kasane-stack-ren-candidate';
    const STRATEGY_MODEL_ID = 'kasane-strategy-candidate';
    const MODEL_IDS = Object.freeze([MODEL_ID, STRATEGY_MODEL_ID]);
    const DEFAULT_WORKER_SCRIPT = './simulator/candidate/kasane-stack-ren-candidate-worker.js?v=candidate-v3';
    const candidateWasm = (() => {
        const search = typeof root?.location?.search === 'string' ? root.location.search : '';
        const requested = new URLSearchParams(search).get('candidateWasm');
        if (!requested) return null;
        if (!/^\.\/builds\/[A-Za-z0-9][A-Za-z0-9._-]*\/kasane-stack-ren-candidate\.wasm$/.test(requested)) {
            throw new Error(`Refusing candidate WASM path outside immutable candidate builds: ${requested}`);
        }
        return requested;
    })();
    const WORKER_SCRIPT = candidateWasm
        ? `${DEFAULT_WORKER_SCRIPT}&wasm=${encodeURIComponent(candidateWasm)}`
        : DEFAULT_WORKER_SCRIPT;

    function workerScriptFor(modelId, fallbackScript) {
        return MODEL_IDS.includes(modelId) ? WORKER_SCRIPT : fallbackScript;
    }

    return Object.freeze({
        MODEL_ID,
        STRATEGY_MODEL_ID,
        MODEL_IDS,
        WORKER_SCRIPT,
        candidateWasm,
        workerScriptFor
    });
});
