/* Run with Node.js: node tools/test-cold-clear-worker-background.js */

'use strict';

const assert = (condition, message) => {
    if (!condition) throw new Error(message);
};
const empty = () => Array.from({ length: 40 }, () => Array(10).fill(null));

(async () => {
    const originalScope = globalThis.self;
    const messages = [];
    const workerScope = {
        postMessage(message) { messages.push(message); }
    };
    globalThis.self = workerScope;
    globalThis.importScripts = file => {
        if (file === './cold-clear-core.js') require('../simulator/workers/cold-clear-core.js');
    };
    require('../simulator/workers/cold-clear-worker.js');

    workerScope.onmessage({ data: {
        type: 'analyze',
        requestId: 1,
        board: empty(),
        currentPiece: 'T',
        nextQueue: ['I', 'O', 'L', 'J', 'S', 'Z', 'T', 'I'],
        holdPiece: null,
        canHold: true,
        isB2B: false,
        ren: -1,
        thinkTimeMs: 8,
        nodeLimit: 5000,
        background: true
    }});
    await new Promise(resolve => setTimeout(resolve, 90));
    workerScope.onmessage({ data: { type: 'pause' } });
    await new Promise(resolve => setTimeout(resolve, 10));

    const counts = messages.filter(message => message && message.type === 'nodeCount').map(message => message.count);
    const move = messages.find(message => message && message.type === 'move');
    const errors = messages.filter(message => message && message.type === 'error');
    assert(move && move.piece, 'Worker did not produce an initial move.');
    assert(counts.length >= 2, 'Worker did not report background search progress.');
    assert(Math.max(...counts) > counts[0], 'Background search did not add any DAG nodes.');
    assert(errors.length === 0, `Worker reported: ${errors.map(error => error.message).join('; ')}`);

    globalThis.self = originalScope;
    console.log(JSON.stringify({
        passed: true,
        initialNodes: counts[0],
        backgroundNodes: Math.max(...counts),
        updates: counts.length
    }, null, 2));
})().catch(error => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
