/* Run with Node.js: node tools/test-fumen-ai-scoring-worker.js */

'use strict';

const assert = (condition, message) => {
    if (!condition) throw new Error(message);
};
const BOARD_WIDTH = 10;
const BOARD_HEIGHT = 40;
const PIECES = ['I', 'O', 'T', 'L', 'J', 'S', 'Z'];
const empty = () => Array.from({ length: BOARD_HEIGHT }, () => Array(BOARD_WIDTH).fill(null));
const clone = board => board.map(row => [...row]);
const occupied = value => value !== null && value !== undefined && value !== '';
const layoutFromBoard = board => Array.from(board.rows, row => Array.from(
    { length: BOARD_WIDTH },
    (_, x) => (row & (1 << x)) ? 'G' : null
));
const SHAPES = {
    I: [
        [[0, 0], [-1, 0], [1, 0], [2, 0]],
        [[1, 0], [1, -1], [1, 1], [1, 2]],
        [[0, 1], [-1, 1], [1, 1], [2, 1]],
        [[0, 0], [0, -1], [0, 1], [0, 2]]
    ],
    O: [[[0, 0], [1, 0], [0, -1], [1, -1]]],
    T: [
        [[0, 0], [-1, 0], [1, 0], [0, -1]],
        [[0, 0], [0, -1], [1, 0], [0, 1]],
        [[0, 0], [1, 0], [-1, 0], [0, 1]],
        [[0, 0], [0, 1], [-1, 0], [0, -1]]
    ],
    S: [
        [[0, 0], [-1, 0], [0, -1], [1, -1]],
        [[0, 0], [0, -1], [1, 0], [1, 1]],
        [[0, 0], [1, 0], [0, 1], [-1, 1]],
        [[0, 0], [0, 1], [-1, 0], [-1, -1]]
    ],
    Z: [
        [[0, 0], [1, 0], [0, -1], [-1, -1]],
        [[0, 0], [0, 1], [1, 0], [1, -1]],
        [[0, 0], [-1, 0], [0, 1], [1, 1]],
        [[0, 0], [0, -1], [-1, 0], [-1, 1]]
    ],
    J: [
        [[0, 0], [-1, 0], [1, 0], [-1, -1]],
        [[0, 0], [0, -1], [0, 1], [1, -1]],
        [[0, 0], [1, 0], [-1, 0], [1, 1]],
        [[0, 0], [0, 1], [0, -1], [-1, 1]]
    ],
    L: [
        [[0, 0], [1, 0], [-1, 0], [1, -1]],
        [[0, 0], [0, 1], [0, -1], [1, 1]],
        [[0, 0], [-1, 0], [1, 0], [-1, 1]],
        [[0, 0], [0, -1], [0, 1], [-1, -1]]
    ]
};

const originalSelf = globalThis.self;
const messages = [];
const workerScope = {
    postMessage(message) { messages.push(message); }
};
globalThis.self = workerScope;
globalThis.importScripts = file => {
    if (file === '../../simulator/workers/cold-clear-core.js') {
        require('../simulator/workers/cold-clear-core.js');
    }
};
require('../F/app/84-ai-scoring-worker.js');

const { Search } = workerScope.ColdClearSimulatorCore;

function edgeCells(edge) {
    const placement = edge.placement;
    const rotations = SHAPES[placement.type];
    const shape = rotations[placement.rotation] || rotations[0];
    return shape
        .map(([dx, dy]) => [placement.x + dx, placement.y + dy])
        .filter(([x, y]) => x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT);
}

function cellKey(cells) {
    return cells.map(([x, y]) => `${x},${y}`).sort().join('|');
}

function sameCells(left, right) {
    return left.length === right.length && cellKey(left) === cellKey(right);
}

function addEdge(board, edge) {
    const next = clone(board);
    for (const [x, y] of edgeCells(edge)) next[y][x] = edge.placement.type;
    return next;
}

function nextFor(search, state) {
    const current = state.current || search.knownPieces[state.index] || null;
    return (current ? [current] : []).concat(search.knownPieces.slice(state.index + 1)).join('');
}

function makeSearch(board, current, queue, hold = null) {
    const search = new Search();
    search.synchronize({
        board,
        currentPiece: current,
        nextQueue: queue,
        holdPiece: hold,
        canHold: true,
        isB2B: false,
        ren: -1
    });
    search.expand(search.root);
    return search;
}

function sourcePage(board, next, hold = '') {
    return { p1: { board, hold, next } };
}

function targetFromEdge(boardBefore, search, edge, { preClear = false } = {}) {
    const state = edge.child.state;
    return {
        p1: {
            board: preClear ? addEdge(boardBefore, edge) : layoutFromBoard(state.board),
            hold: state.hold || '',
            next: nextFor(search, state)
        }
    };
}

function run(pages, runId, options = {}) {
    const start = messages.length;
    workerScope.onmessage({ data: {
        type: 'score',
        runId,
        pages,
        startPage: options.startPage || 0,
        endPage: options.endPage === undefined ? pages.length - 2 : options.endPage,
        replay: Boolean(options.replay),
        operationPages: options.operationPages,
        replayInitial: options.replayInitial,
        nodeBudget: options.nodeBudget === undefined ? 500 : options.nodeBudget,
        thresholdScore: options.thresholdScore === undefined ? 999999 : options.thresholdScore,
        planLength: options.planLength
    }});
    const output = messages.slice(start);
    const error = output.find(message => message.type === 'error');
    assert(!error, `Scoring worker error: ${error && error.message}`);
    const done = output.find(message => message.type === 'done');
    assert(done, 'Scoring worker did not finish.');
    return done.results;
}

function expectSingle(pages, runId, status = 'scored', options = {}) {
    const results = run(pages, runId, options);
    assert(results.length === 1, 'Expected exactly one transition result.');
    assert(results[0].status === status, `Unexpected status: ${results[0].status}`);
    return results[0];
}

function firstEdge(search, predicate) {
    const edge = search.root.children.find(predicate);
    assert(edge, 'Test position has no requested legal edge.');
    return edge;
}

// A normal F transition is the literal four occupied cells added by NEXT[0].
const normalBoard = empty();
const normalSearch = makeSearch(normalBoard, 'T', ['I', 'O', 'L', 'J', 'S', 'Z']);
const normalEdge = firstEdge(normalSearch, edge => !edge.hold && edge.placement.type === 'T');
const normalSource = sourcePage(normalBoard, 'TIOLJSZ');
const normalTarget = targetFromEdge(normalBoard, normalSearch, normalEdge);
const normal = expectSingle([normalSource, normalTarget], 1);
assert(normal.sourceConvention === 'next-first' && normal.targetConvention === 'next-first', 'NEXT[0] convention was not preserved.');
assert(normal.actualMove.piece === 'T' && !normal.actualMove.hold, 'The recorded move did not use source NEXT[0].');
assert(sameCells(normal.actualMove.cells, edgeCells(normalEdge)), 'Actual highlight was not the literal four-cell page delta.');
assert(normal.actualMove.stateAfter && normal.actualMove.stateAfter.hold === (normalEdge.child.state.hold || '') &&
    normal.actualMove.stateAfter.next === nextFor(normalSearch, normalEdge.child.state),
'Actual move does not return its Cold Clear child HOLD/NEXT state.');
assert(normal.reconstruction === 'delta-4' && normal.scoreReliable, 'Normal delta was not scored as literal evidence.');
assert(!Object.prototype.hasOwnProperty.call(normal, 'lossPercent'), 'Percentage loss must not be returned.');

// Shape mismatch and any non-four-cell delta are excluded, never replaced by
// a nearest legal Cold Clear candidate.
const fiveCellsTarget = JSON.parse(JSON.stringify(normalTarget));
fiveCellsTarget.p1.board[0][0] = 'X';
assert(expectSingle([normalSource, fiveCellsTarget], 2, 'ignored').reconstruction === 'invalid-page-delta', 'Five-cell delta was not excluded.');

const wrongShapeTarget = JSON.parse(JSON.stringify(normalTarget));
const removedActual = edgeCells(normalEdge)[0];
wrongShapeTarget.p1.board[removedActual[1]][removedActual[0]] = null;
wrongShapeTarget.p1.board[0][0] = 'X';
assert(expectSingle([normalSource, wrongShapeTarget], 3, 'ignored').reconstruction === 'invalid-page-delta', 'Wrong four-cell shape was not excluded.');

const removedOldBoard = empty();
removedOldBoard[39][0] = 'G';
const removedSearch = makeSearch(removedOldBoard, 'T', ['I', 'O', 'L', 'J', 'S', 'Z']);
const removedEdge = firstEdge(removedSearch, edge => !edge.hold && edge.placement.type === 'T');
const removedTarget = targetFromEdge(removedOldBoard, removedSearch, removedEdge);
removedTarget.p1.board[39][0] = null;
assert(expectSingle([sourcePage(removedOldBoard, 'TIOLJSZ'), removedTarget], 4, 'ignored').reconstruction === 'invalid-page-delta', 'Removing an old block was not excluded.');

const shiftedNextTarget = JSON.parse(JSON.stringify(normalTarget));
shiftedNextTarget.p1.next = shiftedNextTarget.p1.next.slice(1);
assert(expectSingle([normalSource, shiftedNextTarget], 5, 'ignored').reconstruction === 'invalid-page-delta', 'Preview-only NEXT was incorrectly accepted.');

const wrongHoldTarget = JSON.parse(JSON.stringify(normalTarget));
wrongHoldTarget.p1.hold = 'Z';
assert(expectSingle([normalSource, wrongHoldTarget], 6, 'ignored').reconstruction === 'invalid-page-delta', 'Target HOLD mismatch was incorrectly accepted.');

const missingNextSource = sourcePage(normalBoard, '');
assert(expectSingle([missingNextSource, normalTarget], 7, 'ignored').reconstruction === 'invalid-page-delta', 'Missing NEXT invented a seven-piece fallback.');

// A nonempty HOLD is the only alternate source piece.  Empty HOLD must not
// use NEXT[1] as an invented action.
const heldSearch = makeSearch(normalBoard, 'T', ['I', 'O', 'L', 'J', 'S', 'Z'], 'J');
const heldEdge = firstEdge(heldSearch, edge => edge.hold && edge.placement.type === 'J');
const heldSource = sourcePage(normalBoard, 'TIOLJSZ', 'J');
const heldTarget = targetFromEdge(normalBoard, heldSearch, heldEdge);
const held = expectSingle([heldSource, heldTarget], 8);
assert(held.actualMove.hold && held.actualMove.piece === 'J', 'Nonempty HOLD action was not reconstructed.');

// Regression: J rotation 3 must keep its foot below-left of the pivot.  This
// catches a duplicated, mirrored cell map in the scoring worker even when the
// other J rotations still reconstruct correctly.
const jRotation3Edge = firstEdge(heldSearch, edge => edge.hold && edge.placement.type === 'J' && edge.placement.rotation === 3);
const jRotation3Target = targetFromEdge(normalBoard, heldSearch, jRotation3Edge);
const jRotation3 = expectSingle([heldSource, jRotation3Target], 18);
assert(jRotation3.actualMove.rotation === 3, 'J rotation 3 was not preserved.');
assert(sameCells(jRotation3.actualMove.cells, edgeCells(jRotation3Edge)), 'J rotation 3 cells do not match the simulator geometry.');

const emptyHoldSearch = makeSearch(normalBoard, 'T', ['I', 'O', 'L', 'J', 'S', 'Z']);
const emptyHoldEdge = firstEdge(emptyHoldSearch, edge => edge.hold && edge.placement.type === 'I');
const emptyHoldTarget = targetFromEdge(normalBoard, emptyHoldSearch, emptyHoldEdge);
assert(expectSingle([normalSource, emptyHoldTarget], 9, 'ignored').reconstruction === 'invalid-page-delta', 'Empty HOLD incorrectly reconstructed NEXT[1].');

// The simulator records a pre-clear page just after lock.  It is still a
// valid +4 transition even though Cold Clear's child board has cleared lines.
const clearBoard = empty();
for (let x = 0; x < 6; x++) clearBoard[39][x] = 'G';
const clearSearch = makeSearch(clearBoard, 'I', ['T', 'O', 'L', 'J', 'S', 'Z']);
const clearEdge = firstEdge(clearSearch, edge => !edge.hold && edge.placement.type === 'I' && edgeCells(edge).every(([, y]) => y === 39));
const clearSource = sourcePage(clearBoard, 'ITOLJSZ');
const preClearTarget = targetFromEdge(clearBoard, clearSearch, clearEdge, { preClear: true });
const postClearTarget = targetFromEdge(clearBoard, clearSearch, clearEdge);
const clearResults = run([clearSource, preClearTarget, postClearTarget], 10, { endPage: 1 });
assert(clearResults[0].status === 'scored', 'The lock-before-clear +4 page was not scored.');
assert(clearResults[1].status === 'ignored', 'The following clear-only page was not excluded.');
assert(sameCells(clearResults[0].actualMove.cells, edgeCells(clearEdge)), 'Pre-clear highlight did not use the observed four cells.');

// One unmistakable garbage rise: source shifts upward, a Gx9 bottom row is
// inserted, and an I fills the garbage hole.  The worker scores on that
// virtual pre-move board and exposes it to the renderer.
const garbageSourceBoard = empty();
const garbageBaseline = empty();
for (let x = 1; x < BOARD_WIDTH; x++) garbageBaseline[39][x] = 'G';
const garbageSearch = makeSearch(garbageBaseline, 'I', ['T', 'O', 'L', 'J', 'S', 'Z']);
const garbageEdge = firstEdge(garbageSearch, edge => !edge.hold && edge.placement.type === 'I' && edgeCells(edge).some(([x, y]) => x === 0 && y === 39));
const garbageTarget = targetFromEdge(garbageBaseline, garbageSearch, garbageEdge, { preClear: true });
const garbage = expectSingle([
    sourcePage(garbageSourceBoard, 'ITOLJSZ'),
    garbageTarget
], 11);
assert(garbage.reconstruction === 'garbage-rise' && garbage.garbageRows === 1, 'Explicit garbage rise was not normalized.');
assert(garbage.displayBoard[39].filter(cell => cell === 'G').length === 9, 'Garbage pre-move board was not returned for display.');
assert(sameCells(garbage.actualMove.cells, edgeCells(garbageEdge)), 'Garbage-rise move did not use its observed four cells.');

const doubleGarbageBaseline = empty();
for (const y of [38, 39]) for (let x = 1; x < BOARD_WIDTH; x++) doubleGarbageBaseline[y][x] = 'G';
const doubleGarbageSearch = makeSearch(doubleGarbageBaseline, 'I', ['T', 'O', 'L', 'J', 'S', 'Z']);
const doubleGarbageEdge = firstEdge(doubleGarbageSearch, edge => !edge.hold && edge.placement.type === 'I' &&
    edgeCells(edge).some(([x, y]) => x === 0 && y === 38) && edgeCells(edge).some(([x, y]) => x === 0 && y === 39));
const doubleGarbageTarget = targetFromEdge(doubleGarbageBaseline, doubleGarbageSearch, doubleGarbageEdge, { preClear: true });
const doubleGarbage = expectSingle([
    sourcePage(empty(), 'ITOLJSZ'),
    doubleGarbageTarget
], 12);
assert(doubleGarbage.reconstruction === 'garbage-rise' && doubleGarbage.garbageRows === 2, 'Two-row garbage rise was not normalized.');

const garbageOnlyTarget = sourcePage(garbageBaseline, 'ITOLJSZ');
assert(expectSingle([sourcePage(garbageSourceBoard, 'ITOLJSZ'), garbageOnlyTarget], 13, 'ignored').reconstruction === 'invalid-page-delta', 'Garbage-only page was scored as a P1 move.');

const invalidGarbageTarget = JSON.parse(JSON.stringify(garbageTarget));
for (let x = 1; x < BOARD_WIDTH; x++) invalidGarbageTarget.p1.board[39][x] = 'T';
assert(expectSingle([sourcePage(garbageSourceBoard, 'ITOLJSZ'), invalidGarbageTarget], 14, 'ignored').reconstruction === 'invalid-page-delta', 'Non-G garbage row was accepted.');

// Candidate-only detailed pass remains on the same DAG, and every animated
// planned move has authoritative HOLD/NEXT state after it.
const poorEdge = [...normalSearch.root.children]
    .filter(edge => !edge.hold && edge.placement.type === 'T')
    .sort((left, right) => {
        const a = normalSearch.edgeValue(left).value;
        const b = normalSearch.edgeValue(right).value;
        return a - b;
    })[0];
const poorTarget = targetFromEdge(normalBoard, normalSearch, poorEdge);
const detailed = expectSingle([normalSource, poorTarget], 15, 'scored', { nodeBudget: 500, thresholdScore: 1 });
assert(detailed.detailed && detailed.detailNodes >= 15000, 'Flagged move did not receive a 15,000-node detailed pass.');
assert(detailed.nodes === detailed.detailNodes && detailed.nodes >= detailed.roughNodes, 'Detailed search did not extend the rough DAG.');
assert(Array.isArray(detailed.aiPlan) && detailed.aiPlan.length >= 1, 'AI plan is missing.');
assert(detailed.aiPlan.every(move => move.stateAfter && typeof move.stateAfter.hold === 'string' && typeof move.stateAfter.next === 'string'), 'AI plan does not synchronize HOLD/NEXT state.');
assert(detailed.aiPlan.length === 6, 'Default AI plan length must be six moves when six known moves are available.');

// The requested plan length is clamped to 1…12.  A long known queue lets the
// worker expand the deterministic PV rather than stopping at the rough
// frontier, and the returned node count includes that extra expansion.
const longNext = 'TIOLJSZTIOLJSZ';
const longSearch = makeSearch(normalBoard, 'T', longNext.slice(1).split(''));
const longEdge = firstEdge(longSearch, edge => !edge.hold && edge.placement.type === 'T');
const longSource = sourcePage(normalBoard, longNext);
const longTarget = targetFromEdge(normalBoard, longSearch, longEdge);
const shortPlan = expectSingle([longSource, longTarget], 16, 'scored', { planLength: 0 });
assert(shortPlan.aiPlan.length === 1, 'Plan length lower bound was not clamped to one.');
const maxPlan = expectSingle([longSource, longTarget], 17, 'scored', { planLength: 99 });
assert(maxPlan.aiPlan.length === 12, 'Plan length upper bound was not clamped to twelve known moves.');
assert(maxPlan.nodes >= maxPlan.roughNodes, 'Final node count did not include PV branch expansion.');

// Replay cases score the recorded operation on every operation page.  They do
// not need a synthetic target page or a four-cell board delta between pages.
function operationFromEdge(edge) {
    return {
        type: edge.placement.type,
        x: edge.placement.type === 'I' ? edge.placement.x - 1 : edge.placement.x,
        y: edge.placement.y,
        rotation: ['spawn', 'right', 'reverse', 'left'][edge.placement.rotation],
        holdUsed: Boolean(edge.hold)
    };
}
const replayBoard0 = empty();
const replaySearch0 = makeSearch(replayBoard0, 'T', ['I', 'O', 'L', 'J', 'S', 'Z']);
const replayEdge0 = firstEdge(replaySearch0, edge => !edge.hold && edge.placement.type === 'T');
const replayBoard1 = addEdge(replayBoard0, replayEdge0);
const replaySearch1 = makeSearch(replayBoard1, 'I', ['O', 'L', 'J', 'S', 'Z']);
const replayEdge1 = firstEdge(replaySearch1, edge => !edge.hold && edge.placement.type === 'I');
const replayBoard2 = addEdge(replayBoard1, replayEdge1);
const replaySearch2 = makeSearch(replayBoard2, 'O', ['L', 'J', 'S', 'Z']);
const replayEdge2 = firstEdge(replaySearch2, edge => !edge.hold && edge.placement.type === 'O');
const replayPages = [
    { p1: { board: replayBoard0, hold: '', next: 'IOLJSZ', operation: operationFromEdge(replayEdge0) } },
    { p1: { board: replayBoard1, hold: '', next: 'OLJSZ', operation: operationFromEdge(replayEdge1) } },
    { p1: { board: replayBoard2, hold: '', next: 'LJSZ', operation: operationFromEdge(replayEdge2) } }
];
const replayResults = run(replayPages, 18, {
    replay: true,
    operationPages: [0, 1, 2],
    endPage: 2,
    replayInitial: { p1: { sequence: 'TIOLJSZ', hold: '' } }
});
assert(replayResults.length === 3, 'Replay scoring did not return every operation page.');
assert(replayResults.every(result => result.status === 'scored'), 'Replay scoring skipped a recorded operation.');
assert(replayResults.every(result => result.reconstruction === 'recorded-operation'), 'Replay scoring used the legacy page-delta path.');

const holdReplaySearch0 = makeSearch(empty(), 'T', ['I', 'O', 'L', 'J', 'S', 'Z'], 'J');
const holdReplayEdge0 = firstEdge(holdReplaySearch0, edge => edge.hold && edge.placement.type === 'J');
const holdReplayBoard1 = addEdge(empty(), holdReplayEdge0);
const holdReplaySearch1 = makeSearch(holdReplayBoard1, 'I', ['O', 'L', 'J', 'S', 'Z'], 'T');
const holdReplayEdge1 = firstEdge(holdReplaySearch1, edge => !edge.hold && edge.placement.type === 'I');
const holdReplayResults = run([
    { p1: { board: empty(), hold: 'J', next: 'IOLJSZ', operation: operationFromEdge(holdReplayEdge0) } },
    { p1: { board: holdReplayBoard1, hold: 'T', next: 'OLJSZ', operation: operationFromEdge(holdReplayEdge1) } }
], 19, {
    replay: true,
    operationPages: [0, 1],
    endPage: 1,
    replayInitial: { p1: { sequence: 'TIOLJSZ', hold: 'J' } }
});
assert(holdReplayResults.length === 2 && holdReplayResults.every(result => result.status === 'scored'), 'Replay HOLD operation was not scored.');

globalThis.self = originalSelf;
console.log(JSON.stringify({
    passed: true,
    normal: { piece: normal.actualMove.piece, cells: normal.actualMove.cells.length },
    hold: held.actualMove.piece,
    clear: clearResults.map(result => result.status),
    garbage: { rows: [garbage.garbageRows, doubleGarbage.garbageRows], cells: garbage.actualMove.cells.length },
    detailed: { roughNodes: detailed.roughNodes, nodes: detailed.nodes, planLength: detailed.aiPlan.length },
    requestedPlans: { short: shortPlan.aiPlan.length, max: maxPlan.aiPlan.length },
    replay: { operations: replayResults.length, holdOperations: holdReplayResults.length, statuses: replayResults.map(result => result.status) }
}, null, 2));
