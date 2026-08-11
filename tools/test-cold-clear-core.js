/* Run with Node.js: node tools/test-cold-clear-core.js */

'use strict';

globalThis.self = globalThis;
require('../simulator/workers/cold-clear-core.js');

const { Board, Search, findMoves } = globalThis.ColdClearSimulatorCore;
const empty = () => Array.from({ length: 40 }, () => Array(10).fill(null));

function assert(condition, message) {
    if (!condition) throw new Error(message);
}

function layoutFromBoard(board) {
    return Array.from(board.rows, row => Array.from(
        { length: 10 },
        (_, x) => (row & (1 << x)) ? 'G' : null
    ));
}

const blank = new Board();
const counts = Object.fromEntries(['I', 'O', 'T', 'L', 'J', 'S', 'Z'].map(piece => [piece, findMoves(blank, piece).length]));
for (const [piece, count] of Object.entries(counts)) {
    assert(count > 0, `No legal ${piece} placement on an empty field.`);
}

// Board/lock accounting mirrors libtetris: B2B is only awarded to a hard
// clear and is broken by an ordinary line clear.
const gapColumn = 4;
const fourRows = new Uint16Array(40);
fourRows[35] = 1;
for (let y = 36; y < 40; y++) fourRows[y] = 0x3ff & ~(1 << gapColumn);
const verticalI = { type: 'I', x: gapColumn - 1, y: 37, rotation: 1, tspin: 0 };
const tetris = new Board(fourRows).lock(verticalI, true, 0);
assert(tetris.lock.kind === 'clear4' && tetris.lock.b2b && tetris.b2b && tetris.lock.garbage === 5, 'Tetris/B2B accounting is incorrect.');
const oneRow = new Uint16Array(40);
oneRow[39] = 0x3ff & ~(1 << gapColumn);
const single = new Board(oneRow).lock(verticalI, true, 0);
assert(single.lock.kind === 'clear1' && !single.lock.b2b && !single.b2b, 'Ordinary clear did not break B2B.');

const search = new Search();
const initial = {
    board: empty(),
    currentPiece: 'T',
    nextQueue: ['I', 'O', 'L', 'J', 'S', 'Z', 'T', 'I'],
    holdPiece: null,
    canHold: true,
    isB2B: false,
    ren: -1,
    thinkTimeMs: 15,
    nodeLimit: 5000
};
const first = search.analyze(initial);
assert(first.move, 'Search did not return an opening move.');

const normal = search.root.children.filter(edge => !edge.hold);
const held = search.root.children.filter(edge => edge.hold);
assert(normal.length > 0 && held.length > 0, 'Expected both normal and hold branches.');
assert(normal.every(edge => edge.child.state.current === 'I' && edge.child.state.hold === null), 'Normal placement corrupted the empty-hold state.');
assert(held.every(edge => edge.child.state.current === 'O' && edge.child.state.hold === 'T'), 'Empty-hold branch did not advance the queue correctly.');

search.commit();
const afterMove = search.root.state;
const rootBeforeReveal = search.root;
const revealed = search.addNextPiece('Z');
assert(revealed.added, 'New preview was not accepted by the DAG.');
assert(search.root === rootBeforeReveal, 'A revealed preview unexpectedly replaced the DAG root.');
const observedPieces = search.knownPieces.slice(afterMove.index);
const second = search.analyze({
    board: layoutFromBoard(afterMove.board),
    currentPiece: observedPieces[0],
    nextQueue: observedPieces.slice(1),
    holdPiece: afterMove.hold,
    canHold: afterMove.canHold,
    isB2B: afterMove.b2b,
    ren: afterMove.combo - 1,
    thinkTimeMs: 8,
    nodeLimit: 5000
});
assert(!second.reset, 'Committed state plus a rolling NEXT reveal failed to reuse the Cold Clear DAG.');

// A normal placement exposes one new preview; an empty-hold placement exposes
// one while swapping and another after locking.  Both must advance source
// generations without a board reset.
function assertRollingPreviewReuse(useHold) {
    const branchSearch = new Search();
    branchSearch.analyze({ ...initial, thinkTimeMs: 8, nodeLimit: 5000 });
    const edge = branchSearch.root.children.find(candidate => candidate.hold === useHold);
    assert(edge, `Missing ${useHold ? 'hold' : 'normal'} branch for preview test.`);
    branchSearch.pendingEdge = edge;
    if (useHold) branchSearch.addNextPiece('L');
    branchSearch.commit();
    branchSearch.addNextPiece('J');
    const state = branchSearch.root.state;
    const pieces = branchSearch.knownPieces.slice(state.index);
    const result = branchSearch.analyze({
        board: layoutFromBoard(state.board),
        currentPiece: pieces[0],
        nextQueue: pieces.slice(1),
        holdPiece: state.hold,
        canHold: state.canHold,
        isB2B: state.b2b,
        ren: state.combo - 1,
        thinkTimeMs: 8,
        nodeLimit: 5000
    });
    assert(!result.reset, `${useHold ? 'Hold' : 'Normal'} rolling preview reset the DAG.`);
}
assertRollingPreviewReuse(false);
assertRollingPreviewReuse(true);

// Exercise the actual Worker bridge too, using a minimal Worker-like scope.
const originalScope = globalThis.self;
const workerMessages = [];
const workerScope = {
    ColdClearSimulatorCore: globalThis.ColdClearSimulatorCore,
    postMessage(message) { workerMessages.push(message); }
};
globalThis.self = workerScope;
globalThis.importScripts = () => {};
require('../simulator/workers/cold-clear-worker.js');
workerScope.onmessage({ data: { ...initial, type: 'analyze', background: false } });
assert(workerMessages.some(message => message && message.piece), 'Worker bridge did not return a move.');
assert(!workerMessages.some(message => message && message.type === 'error'), 'Worker bridge reported an error.');
globalThis.self = originalScope;

console.log(JSON.stringify({
    passed: true,
    legalMoveCounts: counts,
    nodes: search.nodeCount,
    firstMove: first.move,
    secondMove: second.move,
    workerMessages: workerMessages.map(message => message && message.type || 'move')
}, null, 2));
