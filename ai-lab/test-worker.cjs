'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { performance } = require('node:perf_hooks');
const { createPolicy, createPolicyFromBridge, createRuntime } = require('./policy.js');
const { runMatch } = require('./simulator.cjs');

test('browser Worker loads the exported model/WASM and honors analyze, commit and reset', async () => {
  const folder = path.resolve(__dirname, '../simulator/workers');
  const workerUrl = 'http://worker.test/simulator/workers/ga-worker.js';
  const output = [];
  const timers = new Map(); let nextTimer = 0;
  const context = vm.createContext({ performance, WebAssembly, TextEncoder, Uint8Array, DataView,
    Response, URL, location: { href: workerUrl }, console, postMessage: value => output.push(structuredClone(value)),
    setTimeout: callback => { timers.set(++nextTimer, callback); return nextTimer; },
    clearTimeout: id => timers.delete(id),
    fetch: async url => {
      const pathname = new URL(String(url), workerUrl).pathname;
      const filename = path.resolve(__dirname, '..', pathname.slice(1));
      return new Response(fs.readFileSync(filename), { headers: {
        'Content-Type': filename.endsWith('.wasm') ? 'application/wasm' : 'application/json'
      } });
    }
  });
  context.importScripts = (...urls) => {
    for (const url of urls) vm.runInContext(fs.readFileSync(path.resolve(folder, url.split('?')[0]), 'utf8'), context);
  };
  vm.runInContext('self = globalThis;', context);
  vm.runInContext(fs.readFileSync(path.join(folder, 'ga-worker.js'), 'utf8'), context);
  const send = async data => {
    context.onmessage({ data });
    await vm.runInContext('messages', context);
  };
  const snapshot = { type: 'analyze', requestId: 1, nodeLimit: 1000000, thinkTimeMs: 1,
    board: Array.from({ length: 40 }, () => Array(10).fill(null)), currentPiece: 'T',
    nextQueue: [...'IJOLSZTJ'], holdPiece: null, canHold: true, isB2B: false, ren: -1, incoming: 0 };
  snapshot.opponent = { board: Array.from({ length: 40 }, () => Array(10).fill(null)),
    currentPiece: 'I', nextQueue: [...'OTLJSZTI'], holdPiece: null, canHold: true, isB2B: false, incoming: 0 };
  await send(snapshot);
  assert.equal(output.some(m => m.type === 'error'), false, JSON.stringify(output));
  const move = output.find(m => m.type === 'move');
  assert.ok(move && move.piece && move.requestId === 1);
  assert.equal(move.stats.timeLimited, true);
  assert.ok(output.some(m => m.type === 'nodeCount'));
  assert.equal(timers.size, 1, 'operating time starts background search');
  const initialCount = output.filter(m => m.type === 'nodeCount').at(-1).count;
  const [timerId, tick] = [...timers][0]; timers.delete(timerId); tick();
  assert.ok(output.filter(m => m.type === 'nodeCount').at(-1).count > initialCount);
  assert.equal(output.filter(m => m.type === 'move').length, 1, 'background never replaces the selected move');
  await send({ type: 'pause' });
  assert.equal(timers.size, 0, 'pause cancels pending work');
  await send({ type: 'commit' });
  assert.equal(timers.size, 0, 'commit does not open a speculative next decision');
  await send({ type: 'addNextPiece', piece: 'O' });
  await send({ type: 'reset' });
  await send({ ...snapshot, requestId: 2 });
  assert.ok(output.some(m => m.type === 'move' && m.requestId === 2));
  assert.equal(output.some(m => m.type === 'error'), false, JSON.stringify(output));
  await send({ type: 'stop' });
  assert.equal(timers.size, 0);
  await send({ type: 'reset' });
  await send({ ...snapshot, requestId: 4, background: false });
  assert.equal(timers.size, 0, 'background=false is honored');
  await send({ type: 'reset' });
  await send({ ...snapshot, requestId: 3, board: [] });
  assert.ok(output.some(m => m.type === 'error'));
  assert.equal(timers.size, 0, 'errors leave no runaway timer');
  assert.equal(output.some(m => m.type === 'noLegalMove'), false, 'Invalid input is not a fake defeat');
  await send({ type: 'reset' });
  const beforeMissingOpponent = output.length;
  await send({ ...snapshot, requestId: 5, opponent: null });
  assert.ok(output.slice(beforeMissingOpponent).some(m => m.type === 'error' && /requires an opponent/.test(m.message)));
  assert.equal(output.slice(beforeMissingOpponent).some(m => m.type === 'move' || m.type === 'noLegalMove'), false);
  assert.equal(timers.size, 0);
});

test('background-expanded selected roots still execute and commit through real Player', async () => {
  const model = JSON.parse(fs.readFileSync(path.join(__dirname, 'models/ga-v1.json'), 'utf8'));
  // Deliberately emphasize reranking to exercise selected non-default roots.
  model.genome.opponent.pressureHeight = 100000;
  const runtime = await createRuntime();
  const interactive = createPolicyFromBridge(runtime.bridge, { genome: model.genome, nodes: 1500, maxThinkMs: 0.001 });
  const choose = interactive.choose.bind(interactive);
  interactive.choose = snapshot => {
    const move = choose(snapshot);
    if (!move.noLegalMove) {
      let work;
      for (let i = 0; i < 100; i++) { work = interactive.thinkAhead(8); if (work.done) break; }
      assert.equal(work.done, true);
    }
    return move;
  };
  const opponent = await createPolicy({ nodes: 1500 });
  try {
    const result = await runMatch({ seed: 260929, policies: [interactive, opponent], maxTimeMs: 45000, maxPieces: 200 });
    assert.ok(result.players[0].pieces > 10);
    assert.ok(interactive.stats.backgroundNodesAdded > 0);
    assert.ok(interactive.stats.reranked > 0);
    assert.ok(interactive.stats.commits > 10);
  } finally { interactive.close(); opponent.close(); }
});
