'use strict';

// Editor scoring is intentionally isolated from a running simulator.  The
// search implementation is the same Cold Clear port, but each page pair owns
// a scratch DAG in this worker.
importScripts('../../simulator/workers/cold-clear-core.js');
importScripts('../../simulator/workers/cold-clear-wasm.js');

const { Search } = self.ColdClearSimulatorCore;
const PIECES = Object.freeze(['I', 'O', 'T', 'L', 'J', 'S', 'Z']);
const BOARD_WIDTH = 10;
const BOARD_HEIGHT = 40;
let wasmBridgePromise = null;

function loadWasmBridge() {
    if (!wasmBridgePromise) {
        wasmBridgePromise = ColdClearWasmBridge.load('../../simulator/workers/cold-clear.wasm');
    }
    return wasmBridgePromise;
}

// These cells use exactly the simulator's top-origin public geometry.  They
// are used both to verify a recorded board delta and to draw the observed
// four cells in the editor.
const TETROMINO_CELLS = Object.freeze({
    I: [
        [[0, 0], [-1, 0], [1, 0], [2, 0]],
        [[1, 0], [1, -1], [1, 1], [1, 2]],
        [[0, 1], [-1, 1], [1, 1], [2, 1]],
        [[0, 0], [0, -1], [0, 1], [0, 2]]
    ],
    O: [
        [[0, 0], [1, 0], [0, -1], [1, -1]],
        [[0, 0], [1, 0], [0, -1], [1, -1]],
        [[0, 0], [1, 0], [0, -1], [1, -1]],
        [[0, 0], [1, 0], [0, -1], [1, -1]]
    ],
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
        // Rotation 3 is the clockwise-rotated spawn shape: the foot is
        // below-left of the pivot.  Keep this aligned with the simulator
        // and Cold Clear geometry.
        [[0, 0], [0, 1], [0, -1], [-1, 1]]
    ],
    L: [
        [[0, 0], [1, 0], [-1, 0], [1, -1]],
        [[0, 0], [0, 1], [0, -1], [1, 1]],
        [[0, 0], [-1, 0], [1, 0], [-1, 1]],
        [[0, 0], [0, -1], [0, 1], [-1, -1]]
    ]
});

function cleanPieces(value) {
    return Array.from(String(value || '').toUpperCase()).filter(piece => PIECES.includes(piece));
}

function pageState(page) {
    const p1 = page && page.p1 ? page.p1 : {};
    return {
        board: Array.isArray(p1.board) ? p1.board : [],
        hold: cleanPieces(p1.hold)[0] || null,
        current: cleanPieces(p1.current)[0] || null,
        // In an F page NEXT[0] is the mino which is about to be placed.
        next: cleanPieces(p1.next)
    };
}

function emptyLayout() {
    return Array.from({ length: BOARD_HEIGHT }, () => Array(BOARD_WIDTH).fill(null));
}

function normalizeLayout(layout) {
    const normalized = emptyLayout();
    if (!Array.isArray(layout)) return normalized;
    const offset = BOARD_HEIGHT - layout.length;
    for (let y = 0; y < layout.length; y++) {
        const destination = y + offset;
        if (destination < 0 || destination >= BOARD_HEIGHT) continue;
        for (let x = 0; x < BOARD_WIDTH; x++) {
            const value = layout[y] && layout[y][x];
            normalized[destination][x] = value === undefined ? null : value;
        }
    }
    return normalized;
}

function cloneLayout(layout) {
    return normalizeLayout(layout).map(row => [...row]);
}

function occupied(cell) {
    return cell !== null && cell !== undefined && cell !== '';
}

function currentFor(search, state) {
    return state.current || search.knownPieces[state.index] || null;
}

function expectedNext(search, state) {
    const current = currentFor(search, state);
    const future = search.knownPieces.slice(state.index + 1);
    return current ? [current, ...future] : future;
}

function queueEvidence(expected, observed) {
    const comparable = Math.min(expected.length, observed.length);
    let count = 0;
    while (count < comparable && expected[count] === observed[count]) count++;
    const firstMismatch = count < comparable;
    return {
        count,
        comparable,
        firstMismatch,
        compatible: !firstMismatch,
        exact: !firstMismatch && expected.length === observed.length
    };
}

function publicMove(edge) {
    const placement = edge.placement;
    const shape = TETROMINO_CELLS[placement.type] && TETROMINO_CELLS[placement.type][placement.rotation];
    return {
        piece: placement.type,
        // Preserve the simulator's public I-origin convention.
        x: placement.type === 'I' ? placement.x - 1 : placement.x,
        y: placement.y,
        rotation: placement.rotation,
        tspin: placement.tspin === 2 ? 'full' : (placement.tspin === 1 ? 'mini' : null),
        hold: Boolean(edge.hold),
        cells: shape ? shape
            .map(([dx, dy]) => [placement.x + dx, placement.y + dy])
            .filter(([x, y]) => x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT) : []
    };
}

function physicalMoveKey(edge) {
    const move = publicMove(edge);
    return `${move.piece}:${move.x}:${move.y}:${move.rotation}:${move.tspin || '-'}:${move.hold ? 1 : 0}`;
}

function staticEdgeValue(search, edge) {
    const value = search.edgeValue(edge);
    return {
        value: Number.isFinite(value.value) ? value.value : -Infinity,
        spike: Number.isFinite(value.spike) ? value.spike : -Infinity
    };
}

function cellSetKey(cells) {
    return cells
        .map(cell => `${Math.floor(cell[0])},${Math.floor(cell[1])}`)
        .sort()
        .join('|');
}

function sameCells(left, right) {
    return Array.isArray(left) && Array.isArray(right) && left.length === right.length &&
        cellSetKey(left) === cellSetKey(right);
}

// The player action is observed from the F pages, not guessed from the best
// search edge.  A normal transition must keep every old occupied cell and
// add exactly four occupied cells.  Colour is presentation data; occupancy
// and shape are the reliable facts here.
function strictDelta(before, after) {
    const added = [];
    const removed = [];
    for (let y = 0; y < BOARD_HEIGHT; y++) {
        for (let x = 0; x < BOARD_WIDTH; x++) {
            const wasOccupied = occupied(before[y][x]);
            const isOccupied = occupied(after[y][x]);
            if (!wasOccupied && isOccupied) added.push([x, y]);
            if (wasOccupied && !isOccupied) removed.push([x, y]);
        }
    }
    return {
        added,
        removed,
        valid: added.length === 4 && removed.length === 0
    };
}

function sameOccupancy(left, right) {
    for (let y = 0; y < BOARD_HEIGHT; y++) {
        for (let x = 0; x < BOARD_WIDTH; x++) {
            if (occupied(left[y][x]) !== occupied(right[y][x])) return false;
        }
    }
    return true;
}

function createSearch(preMoveBoard, source, context) {
    // Legacy pages expose the current piece as NEXT[0]. Replay pages expose
    // the pre-hold current piece separately and keep NEXT as the queue that
    // existed before the recorded operation.
    const hasExplicitCurrent = Boolean(source.current);
    const currentPiece = hasExplicitCurrent ? source.current : source.next[0];
    const nextQueue = hasExplicitCurrent ? source.next : source.next.slice(1);
    if (!currentPiece) return null;
    const search = new Search();
    search.synchronize({
        board: preMoveBoard,
        currentPiece,
        nextQueue,
        holdPiece: source.hold,
        canHold: true,
        isB2B: Boolean(context.b2b),
        ren: Number.isFinite(context.ren) ? context.ren : -1
    });
    search.expand(search.root);
    return search;
}

async function createWasmSearch(preMoveBoard, source, context, nodeLimit) {
    const currentPiece = source.current || source.next[0];
    const nextQueue = source.current ? source.next : source.next.slice(1);
    if (!currentPiece) return null;
    const bridge = await loadWasmBridge();
    const handle = bridge.create({
        board: preMoveBoard,
        currentPiece,
        nextQueue,
        holdPiece: source.hold,
        canHold: true,
        isB2B: Boolean(context.b2b),
        ren: Number.isFinite(context.ren) ? context.ren : -1,
        nodeLimit: Math.max(1, nodeLimit | 0)
    });
    return { bridge, handle };
}

function destroyWasmSearch(wasmSearch) {
    if (wasmSearch?.bridge && wasmSearch.handle) {
        wasmSearch.bridge.destroy(wasmSearch.handle);
        wasmSearch.handle = 0;
    }
}

const SIMULATOR_SHAPES = Object.freeze({
    I: { shape: [[0, 0], [1, 0], [2, 0], [3, 0]], center: [1.5, 0.5] },
    O: { shape: [[0, 0], [1, 0], [0, -1], [1, -1]], center: [0.5, -0.5] },
    T: { shape: [[0, 0], [-1, 0], [0, -1], [1, 0]], center: [0, 0] },
    L: { shape: [[-1, 0], [0, 0], [1, 0], [1, -1]], center: [0, 0] },
    J: { shape: [[0, 0], [-1, 0], [1, 0], [-1, -1]], center: [0, 0] },
    S: { shape: [[1, -1], [-1, 0], [0, 0], [0, -1]], center: [0, 0] },
    Z: { shape: [[0, 0], [1, 0], [0, -1], [-1, -1]], center: [0, 0] }
});

function operationRotationIndex(value) {
    if (typeof value === 'number') return ((Math.round(value) % 4) + 4) % 4;
    const index = ['spawn', 'right', 'reverse', 'left'].indexOf(String(value || 'spawn').toLowerCase());
    return index >= 0 ? index : 0;
}

function simulatorShape(type, rotation) {
    const definition = SIMULATOR_SHAPES[type];
    if (!definition) return [];
    if (type === 'O' || rotation === 0) return definition.shape.map(cell => [...cell]);
    return definition.shape.map(([baseX, baseY]) => {
        let x = baseX - definition.center[0];
        let y = baseY - definition.center[1];
        for (let index = 0; index < rotation; index++) [x, y] = [-y, x];
        return [Math.round(x + definition.center[0] + (type === 'O' ? 0.5 : 0)),
            Math.round(y + definition.center[1] + (type === 'O' ? 0.5 : 0))];
    });
}

function recordedOperationCells(operation) {
    if (!operation || !SIMULATOR_SHAPES[operation.type]) return [];
    const rotation = operationRotationIndex(operation.rotation);
    return simulatorShape(operation.type, rotation)
        .map(([x, y]) => [Math.round(operation.x + x), Math.round(operation.y + y)])
        .filter(([x, y]) => x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT);
}

function normalizeRecordedOperation(value) {
    if (!value || typeof value !== 'object') return null;
    const type = cleanPieces(value.type || value.piece)[0];
    const x = Number(value.x);
    const y = Number(value.y);
    if (!type || !Number.isFinite(x) || !Number.isFinite(y)) return null;
    return {
        type,
        x: Math.round(x),
        y: Math.round(y),
        rotation: operationRotationIndex(value.rotation),
        holdUsed: Boolean(value.holdUsed || value.hold || value.source === 'hold')
    };
}

function operationForWorker(page) {
    return normalizeRecordedOperation(page?.p1?.operation);
}

function replaySourceAtPage(pages, initial, pageIndex) {
    const explicitPage = pages[pageIndex]?.p1 || {};
    // Native replay exports already contain the authoritative state at every
    // page. Replaying carried 2P operations here would apply the same P1 lock
    // several times and shift HOLD/NEXT away from the page being scored.
    if (Object.prototype.hasOwnProperty.call(explicitPage, 'active')) {
        return {
            board: Array.isArray(explicitPage.board) ? explicitPage.board : [],
            current: cleanPieces(explicitPage.active)[0] || null,
            hold: cleanPieces(explicitPage.hold)[0] || null,
            next: cleanPieces(explicitPage.next),
            explicitCurrent: true
        };
    }
    const seed = initial || {};
    const stream = cleanPieces(seed.sequence);
    let current = stream.shift() || null;
    const queue = stream;
    let hold = cleanPieces(seed.hold)[0] || null;
    for (let index = 0; index < pageIndex; index++) {
        const operation = normalizeRecordedOperation(pages[index]?.p1?.operation);
        if (operation?.holdUsed) {
            if (hold) {
                const previousCurrent = current;
                current = hold;
                hold = previousCurrent;
            } else {
                hold = current;
                current = queue.shift() || null;
            }
        }
        if (operation) current = queue.shift() || null;
    }
    const page = pages[pageIndex]?.p1 || {};
    return {
        board: Array.isArray(page.board) ? page.board : [],
        current: current || cleanPieces(page.current)[0] || null,
        hold: hold || cleanPieces(page.hold)[0] || null,
        next: cleanPieces(page.next),
        explicitCurrent: false
    };
}

function recordedOperationMatches(search, operation, source) {
    if (!search?.root?.children || !operation) return [];
    const cells = recordedOperationCells(operation);
    if (cells.length !== 4) return [];
    // In the new replay format HOLD has already been resolved into
    // `source.current`; holdUsed records provenance for the viewer and must
    // not cause Cold Clear to press HOLD a second time.
    const usesHoldAction = !source.explicitCurrent && operation.holdUsed;
    return search.root.children
        .filter(edge => Boolean(edge.hold) === usesHoldAction)
        .filter(edge => edge.placement.type === operation.type)
        .filter(edge => edge.placement.rotation === operation.rotation)
        .filter(edge => sameCells(publicMove(edge).cells, cells))
        .map(edge => ({
            edge,
            state: {
                holdExact: true,
                queue: { count: 0, exact: true, compatible: true },
                valid: true
            }
        }));
}

function forcedRecordedOperationEdge(search, source, operation) {
    // A replay operation is authoritative even when the zero-gravity move
    // enumerator cannot reproduce its input path (most commonly a left-wall
    // I/S/Z placement after a garbage rise).  The board delta still proves
    // the four locked cells, so score that exact placement instead of
    // dropping the hand from the run.
    if (!search?.root?.state?.board || !source?.explicitCurrent ||
        source.current !== operation.type) return null;
    const rotation = operation.rotation;
    const placement = {
        type: operation.type,
        // Cold Clear uses the SRS anchor for I; replay operations use the
        // simulator's visible-cell anchor.  The other pieces share anchors.
        x: operation.type === 'I' ? operation.x + 1 : operation.x,
        y: operation.y,
        rotation,
        tspin: 0,
        time: 0,
        inputs: []
    };
    if (!search.root.state.board.valid(placement)) return null;
    return search.makeEdge(search.root, search.root.state, placement, false);
}

function edgeCanBeRecordedMove(edge, source) {
    if (!edge.hold) return edge.placement.type === (source.current || source.next[0]);
    // An empty HOLD would take NEXT[1], which is deliberately not a valid
    // reconstruction source for the editor's page format.
    return Boolean(source.hold) && edge.placement.type === source.hold;
}

function targetStateEvidence(search, edge, target) {
    const state = edge.child.state;
    const holdExact = (state.hold || null) === target.hold;
    const queue = queueEvidence(expectedNext(search, state), target.next);
    return { holdExact, queue, valid: holdExact && queue.compatible };
}

function sameColdClearBoard(board, target) {
    if (!board || !target || typeof board.occupied !== 'function') return false;
    for (let y = 0; y < BOARD_HEIGHT; y++) {
        for (let x = 0; x < BOARD_WIDTH; x++) {
            if (Boolean(board.occupied(x, y)) !== occupied(target[y][x])) return false;
        }
    }
    return true;
}

function matchingEdges(search, source, target, delta) {
    if (!search || !search.root || !search.root.children) return [];
    const targetBoard = normalizeLayout(target?.board);
    const matches = [];
    for (const edge of search.root.children) {
        if (!edgeCanBeRecordedMove(edge, source)) continue;
        // A normal snapshot exposes the four locked cells directly.  A native
        // Fumen page, however, stores the next page after line clear, so old
        // cells can disappear and the four new cells are no longer a literal
        // delta.  In that case compare the authoritative Cold Clear child
        // board instead of trying to reverse-engineer the cleared rows.
        if (delta.valid) {
            if (!sameCells(publicMove(edge).cells, delta.added)) continue;
        } else if (!sameColdClearBoard(edge.child?.state?.board, targetBoard)) {
            continue;
        }
        const state = targetStateEvidence(search, edge, target);
        if (!state.valid) continue;
        matches.push({ edge, state });
    }
    return matches;
}

// When HOLD/NEXT leaves two literal interpretations (for example identical
// pieces in HOLD and NEXT), retain the user's requested "better one" rule.
// This is only a tie-break among placements that already explain the exact
// observed four cells; it is never a nearest-legal reconstruction.
function selectMatchingEdge(search, matches) {
    if (!matches.length) return null;
    return [...matches].sort((left, right) => {
        const a = staticEdgeValue(search, left.edge);
        const b = staticEdgeValue(search, right.edge);
        if (a.value !== b.value) return b.value - a.value;
        if (a.spike !== b.spike) return b.spike - a.spike;
        return physicalMoveKey(left.edge).localeCompare(physicalMoveKey(right.edge));
    })[0];
}

function tryReconstruction(preMoveBoard, targetBoard, source, target, context, kind, garbageRows = 0) {
    const delta = strictDelta(preMoveBoard, targetBoard);
    const search = createSearch(preMoveBoard, source, context);
    if (!search) return null;
    const matches = matchingEdges(search, source, target, delta);
    if (!matches.length) return null;
    return {
        search,
        matches,
        delta,
        preMoveBoard,
        kind: !delta.valid && kind === 'delta-4' ? 'lock-result' : kind,
        garbageRows
    };
}

function canonicalGarbageBaseRow(row) {
    let garbage = 0;
    let nonGarbage = 0;
    const base = Array(BOARD_WIDTH).fill(null);
    for (let x = 0; x < BOARD_WIDTH; x++) {
        if (row[x] === 'G') {
            garbage++;
            base[x] = 'G';
        } else if (occupied(row[x])) {
            // A non-G block can only be the newly placed mino filling the
            // one garbage hole.  It is intentionally blank in the baseline.
            nonGarbage++;
        }
    }
    return garbage === 9 && nonGarbage <= 1 ? base : null;
}

// Recognise only an unambiguous incoming-garbage rise: the old board is
// shifted upward by k rows and every inserted bottom row is Gx9 plus one
// hole (possibly occupied by the just-placed mino).  Anything looser is not
// safe enough to turn into a recorded player action.
function garbageRaisedBaseline(sourceBoard, targetBoard, rows) {
    if (rows < 1 || rows >= BOARD_HEIGHT) return null;
    for (let y = 0; y < rows; y++) {
        if (sourceBoard[y].some(occupied)) return null;
    }
    const baseline = emptyLayout();
    for (let y = 0; y < BOARD_HEIGHT - rows; y++) baseline[y] = [...sourceBoard[y + rows]];
    for (let y = BOARD_HEIGHT - rows; y < BOARD_HEIGHT; y++) {
        const garbage = canonicalGarbageBaseRow(targetBoard[y]);
        if (!garbage) return null;
        baseline[y] = garbage;
    }
    return baseline;
}

function reconstructTransition(source, target, context) {
    const sourceBoard = normalizeLayout(source.board);
    const targetBoard = normalizeLayout(target.board);
    const normal = tryReconstruction(sourceBoard, targetBoard, source, target, context, 'delta-4');
    if (normal) return normal;

    const garbageMatches = [];
    for (let rows = 1; rows < BOARD_HEIGHT; rows++) {
        const baseline = garbageRaisedBaseline(sourceBoard, targetBoard, rows);
        if (!baseline) continue;
        const candidate = tryReconstruction(baseline, targetBoard, source, target, context, 'garbage-rise', rows);
        if (candidate) garbageMatches.push(candidate);
    }
    // Multiple rise distances would be a false-positive risk.  Normal moves
    // already won above; only a single explicit garbage explanation is used.
    return garbageMatches.length === 1 ? garbageMatches[0] : null;
}

function sameSnapshot(source, target) {
    return source.hold === target.hold && source.next.join('') === target.next.join('') &&
        sameOccupancy(normalizeLayout(source.board), normalizeLayout(target.board));
}

function passiveBoardTransition(source, target) {
    // The simulator can emit a second page solely for the post-lock line
    // clear.  It does not advance HOLD/NEXT, so retain the prior CC context.
    return source.hold === target.hold && source.next.join('') === target.next.join('');
}

function bestChild(search, node) {
    if (!node || !node.children || !node.children.length) return null;
    return [...node.children].sort((left, right) => {
        const a = staticEdgeValue(search, left);
        const b = staticEdgeValue(search, right);
        if (a.value !== b.value) return b.value - a.value;
        if (a.spike !== b.spike) return b.spike - a.spike;
        return physicalMoveKey(left).localeCompare(physicalMoveKey(right));
    })[0] || null;
}

function planMove(search, edge) {
    const move = publicMove(edge);
    const state = edge.child.state;
    // The canvas advances its own coloured board, while this authoritative
    // state keeps HOLD/NEXT synchronized with each animated AI step.
    move.stateAfter = {
        hold: state.hold || '',
        next: expectedNext(search, state).join('')
    };
    return move;
}

function principalVariation(search, firstEdge, limit = 4) {
    const moves = [];
    let edge = firstEdge;
    while (edge && moves.length < limit) {
        moves.push(planMove(search, edge));
        const node = edge.child;
        // Never manufacture an AI continuation over an unknown 7-bag draw.
        // For revealed pieces, expand this exact PV branch on demand so a
        // caller-selected plan length is not artificially capped by the
        // random rough-search frontier.
        if (!currentFor(search, node.state)) break;
        if (!node.children && !node.chanceGroups && !node.terminal) search.expand(node);
        if (!node.children) break;
        edge = bestChild(search, node);
    }
    return moves;
}

function scorePairLegacy(search, actualEdge) {
    const bestEdge = search.best(0) || actualEdge;
    const actual = staticEdgeValue(search, actualEdge);
    const best = staticEdgeValue(search, bestEdge);
    const scoreGap = Number.isFinite(actual.value) && Number.isFinite(best.value)
        ? Math.max(0, best.value - actual.value) : 0;
    return { actual, best, bestEdge, scoreGap };
}

function sameScoredMove(left, right) {
    return left && right && left.piece === right.piece &&
        Boolean(left.hold) === Boolean(right.hold) &&
        Number(left.rotation) === Number(right.rotation) &&
        Number(left.x) === Number(right.x) &&
        Number(left.y) === Number(right.y);
}

function candidatePublicMove(candidate) {
    const shape = TETROMINO_CELLS[candidate.piece]?.[candidate.rotation] || [];
    return {
        piece: candidate.piece,
        x: candidate.x,
        y: candidate.y,
        rotation: candidate.rotation,
        tspin: candidate.tspin,
        hold: Boolean(candidate.hold),
        cells: shape
            .map(([dx, dy]) => [candidate.x + dx, candidate.y + dy])
            .filter(([x, y]) => x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT)
    };
}

async function scorePair(search, actualEdge, wasmSearch) {
    if (!wasmSearch) return scorePairLegacy(search, actualEdge);
    const candidates = wasmSearch.bridge.candidates(wasmSearch.handle);
    const actualMove = publicMove(actualEdge);
    const actualCandidate = candidates.find(candidate => sameScoredMove(candidate, actualMove));
    if (!actualCandidate || !candidates.length) return scorePairLegacy(search, actualEdge);

    const bestCandidate = [...candidates].sort((left, right) =>
        right.value - left.value || right.spike - left.spike
    )[0];
    const bestMove = candidatePublicMove(bestCandidate);
    const bestEdge = search.root.children.find(edge => sameScoredMove(publicMove(edge), bestMove)) || actualEdge;
    const actual = { value: actualCandidate.value, spike: actualCandidate.spike };
    const best = { value: bestCandidate.value, spike: bestCandidate.spike };
    return {
        actual,
        best,
        bestEdge,
        bestMove,
        scoreGap: Math.max(0, best.value - actual.value),
        wasm: true
    };
}

function meetsThreshold(scoreGap, thresholdScore) {
    return scoreGap > 0 && scoreGap >= thresholdScore;
}

function contextFromEdge(edge) {
    if (!edge) return { b2b: false, ren: -1 };
    return {
        b2b: Boolean(edge.child.state.b2b),
        ren: edge.child.state.combo - 1
    };
}

function ignoredResult(pageIndex, reconstruction, context, preserveContext) {
    return {
        result: {
            pageIndex,
            status: 'ignored',
            reconstruction,
            candidateCount: 0,
            scoreReliable: false,
            blunder: false
        },
        nextContext: preserveContext ? { ...context } : { b2b: false, ren: -1 }
    };
}

function observedMove(search, edge, addedCells, reconstructionKind) {
    const move = planMove(search, edge);
    // For a plain transition this is deliberately the literal page-to-page
    // delta.  For a line-clear transition the delta is not the locked mino,
    // so the matched Cold Clear edge is the authoritative four-cell shape.
    move.cells = reconstructionKind !== 'lock-result' && addedCells.length === 4
        ? addedCells.map(([x, y]) => [x, y])
        : publicMove(edge).cells.map(([x, y]) => [x, y]);
    return move;
}

async function scoreTransition(pageIndex, source, target, context, nodeBudget, detailNodeBudget, thresholdScore, planLength, useWasm = true) {
    if (sameSnapshot(source, target)) return ignoredResult(pageIndex, 'unchanged-snapshot', context, true);

    const reconstructed = reconstructTransition(source, target, context);
    if (!reconstructed) {
        return ignoredResult(pageIndex, 'invalid-page-delta', context, passiveBoardTransition(source, target));
    }

    const { search, matches, delta, preMoveBoard } = reconstructed;
    let wasmSearch = null;
    if (useWasm) {
        try {
            wasmSearch = await createWasmSearch(preMoveBoard, source, context, detailNodeBudget);
        } catch (_) {
            // Keep the old JS path as a compatibility fallback if the static WASM
            // asset is unavailable (for example in an offline editor export).
        }
    }
    if (wasmSearch) wasmSearch.bridge.think(wasmSearch.handle, 3000, nodeBudget);
    else search.thinkNodes(nodeBudget, 3000);
    const actualMatch = selectMatchingEdge(search, matches);
    if (!actualMatch) {
        destroyWasmSearch(wasmSearch);
        return ignoredResult(pageIndex, 'invalid-page-delta', context, false);
    }

    const actualEdge = actualMatch.edge;
    const roughNodes = wasmSearch ? wasmSearch.bridge.nodeCount(wasmSearch.handle) : search.nodeCount;
    const rough = await scorePair(search, actualEdge, wasmSearch);
    let final = rough;
    let detailed = false;
    if (meetsThreshold(rough.scoreGap, thresholdScore)) {
        // `thinkNodes` takes a total-DAG target, so this extends rather than
        // restarts the rough search.
        if (wasmSearch) wasmSearch.bridge.think(wasmSearch.handle, 10000, detailNodeBudget);
        else search.thinkNodes(detailNodeBudget, 10000);
        final = await scorePair(search, actualEdge, wasmSearch);
        detailed = true;
    }

    // Fill the requested known-piece PV after the score search. This can
    // expand a few deterministic child nodes, so refresh the values and the
    // best edge before publishing both the plan and the final node count.
    let aiPlan = principalVariation(search, final.bestEdge, planLength);
    final = await scorePair(search, actualEdge, wasmSearch);
    aiPlan = principalVariation(search, final.bestEdge, planLength);
    final = await scorePair(search, actualEdge, wasmSearch);
    // A small PV expansion can surface a rough-threshold gap that was not
    // visible at the original frontier. Finish it with the same detailed DAG
    // pass rather than emitting a partially searched blunder.
    if (!detailed && meetsThreshold(final.scoreGap, thresholdScore)) {
        if (wasmSearch) wasmSearch.bridge.think(wasmSearch.handle, 10000, detailNodeBudget);
        else search.thinkNodes(detailNodeBudget, 10000);
        detailed = true;
        final = await scorePair(search, actualEdge, wasmSearch);
        aiPlan = principalVariation(search, final.bestEdge, planLength);
        final = await scorePair(search, actualEdge, wasmSearch);
    }

    const { actual: actualScore, best: bestScore, bestEdge, scoreGap } = final;
    const result = {
        pageIndex,
        status: 'scored',
        reconstruction: reconstructed.kind,
        sourceConvention: 'next-first',
        targetConvention: 'next-first',
        sourceBoardVariant: reconstructed.kind === 'garbage-rise' ? 'garbage-baseline' : 'raw',
        targetBoardVariant: reconstructed.kind === 'lock-result' ? 'post-lock' : 'raw',
        searchEngine: wasmSearch ? 'wasm' : 'javascript',
        displayBoard: cloneLayout(reconstructed.preMoveBoard),
        sourceBoard: cloneLayout(reconstructed.preMoveBoard),
        garbageRows: reconstructed.garbageRows || 0,
        contextReset: false,
        candidateCount: new Set(matches.map(match => physicalMoveKey(match.edge))).size,
        boardDistance: 0,
        queuePrefix: actualMatch.state.queue.count,
        queueExact: actualMatch.state.queue.exact,
        holdExact: actualMatch.state.holdExact,
        thresholdScore,
        detailNodeBudget,
        roughNodes,
        nodes: wasmSearch ? wasmSearch.bridge.nodeCount(wasmSearch.handle) : search.nodeCount,
        detailed,
        detailNodes: detailed ? (wasmSearch ? wasmSearch.bridge.nodeCount(wasmSearch.handle) : search.nodeCount) : null,
        actualMove: observedMove(search, actualEdge, delta.added, reconstructed.kind),
        bestMove: final.bestMove || publicMove(bestEdge),
        actualScore: actualScore.value,
        actualSpike: actualScore.spike,
        bestScore: bestScore.value,
        bestSpike: bestScore.spike,
        scoreGap,
        roughActualScore: rough.actual.value,
        roughBestScore: rough.best.value,
        roughScoreGap: rough.scoreGap,
        aiPlan,
        scoreReliable: true,
        blunder: meetsThreshold(scoreGap, thresholdScore)
    };
    destroyWasmSearch(wasmSearch);
    return { result, nextContext: contextFromEdge(actualEdge) };
}

async function scoreRecordedOperation(pageIndex, source, operation, context, nodeBudget, detailNodeBudget, thresholdScore, planLength, useWasm = true) {
    const preMoveBoard = normalizeLayout(source.board);
    const search = createSearch(preMoveBoard, source, context);
    const operationCells = recordedOperationCells(operation);
    if (!search || operationCells.length !== 4) {
        return ignoredResult(pageIndex, 'invalid-recorded-operation', context, false);
    }
    let matches = recordedOperationMatches(search, operation, source);
    let forced = false;
    if (!matches.length) {
        const edge = forcedRecordedOperationEdge(search, source, operation);
        if (edge) {
            matches = [{
                edge,
                state: {
                    holdExact: true,
                    queue: { count: 0, exact: true, compatible: true },
                    valid: true
                }
            }];
            forced = true;
        }
    }
    if (!matches.length) {
        return ignoredResult(pageIndex, 'invalid-recorded-operation', context, false);
    }
    let wasmSearch = null;
    if (useWasm) {
        try {
            wasmSearch = await createWasmSearch(preMoveBoard, source, context, detailNodeBudget);
        } catch (_) {
            // Fall back to the legacy JS search when the WASM asset is unavailable.
        }
    }
    if (wasmSearch) wasmSearch.bridge.think(wasmSearch.handle, 3000, nodeBudget);
    else search.thinkNodes(nodeBudget, 3000);
    const actualMatch = selectMatchingEdge(search, matches);
    if (!actualMatch) {
        destroyWasmSearch(wasmSearch);
        return ignoredResult(pageIndex, 'invalid-recorded-operation', context, false);
    }

    const actualEdge = actualMatch.edge;
    const roughNodes = wasmSearch ? wasmSearch.bridge.nodeCount(wasmSearch.handle) : search.nodeCount;
    const rough = await scorePair(search, actualEdge, wasmSearch);
    let final = rough;
    let detailed = false;
    if (meetsThreshold(rough.scoreGap, thresholdScore)) {
        if (wasmSearch) wasmSearch.bridge.think(wasmSearch.handle, 10000, detailNodeBudget);
        else search.thinkNodes(detailNodeBudget, 10000);
        final = await scorePair(search, actualEdge, wasmSearch);
        detailed = true;
    }
    let aiPlan = principalVariation(search, final.bestEdge, planLength);
    final = await scorePair(search, actualEdge, wasmSearch);
    aiPlan = principalVariation(search, final.bestEdge, planLength);
    final = await scorePair(search, actualEdge, wasmSearch);
    if (!detailed && meetsThreshold(final.scoreGap, thresholdScore)) {
        if (wasmSearch) wasmSearch.bridge.think(wasmSearch.handle, 10000, detailNodeBudget);
        else search.thinkNodes(detailNodeBudget, 10000);
        detailed = true;
        final = await scorePair(search, actualEdge, wasmSearch);
        aiPlan = principalVariation(search, final.bestEdge, planLength);
        final = await scorePair(search, actualEdge, wasmSearch);
    }

    const { actual: actualScore, best: bestScore, bestEdge, scoreGap } = final;
    const result = {
        pageIndex,
        status: 'scored',
        reconstruction: forced ? 'forced-recorded-operation' : 'recorded-operation',
        sourceConvention: 'operation',
        targetConvention: 'operation',
        sourceBoardVariant: 'raw',
        targetBoardVariant: 'raw',
        displayBoard: cloneLayout(preMoveBoard),
        sourceBoard: cloneLayout(preMoveBoard),
        garbageRows: 0,
        contextReset: false,
        candidateCount: new Set(matches.map(match => physicalMoveKey(match.edge))).size,
        boardDistance: 0,
        queuePrefix: source.next.length,
        queueExact: true,
        holdExact: true,
        thresholdScore,
        detailNodeBudget,
        roughNodes,
        nodes: wasmSearch ? wasmSearch.bridge.nodeCount(wasmSearch.handle) : search.nodeCount,
        detailed,
        detailNodes: detailed ? (wasmSearch ? wasmSearch.bridge.nodeCount(wasmSearch.handle) : search.nodeCount) : null,
        actualMove: observedMove(search, actualEdge, operationCells),
        bestMove: final.bestMove || publicMove(bestEdge),
        actualScore: actualScore.value,
        actualSpike: actualScore.spike,
        bestScore: bestScore.value,
        bestSpike: bestScore.spike,
        scoreGap,
        roughActualScore: rough.actual.value,
        roughBestScore: rough.best.value,
        roughScoreGap: rough.scoreGap,
        searchEngine: wasmSearch ? 'wasm' : 'javascript',
        aiPlan,
        scoreReliable: true,
        blunder: meetsThreshold(scoreGap, thresholdScore)
    };
    destroyWasmSearch(wasmSearch);
    return { result, nextContext: contextFromEdge(actualEdge) };
}

 self.onmessage = async event => {
        const data = event.data || {};
        if (data.type !== 'score') return;
        try {
            const pages = Array.isArray(data.pages) ? data.pages : [];
            const nodeBudget = Math.max(500, Math.floor(Number(data.nodeBudget) || 5000));
            const useWasm = data.useWasm !== false;
            const requestedDetailNodeBudget = Number(data.detailNodeBudget);
            const detailNodeBudget = Math.max(
                nodeBudget,
                5000,
                Number.isFinite(requestedDetailNodeBudget)
                    ? Math.min(200000, Math.floor(requestedDetailNodeBudget))
                    : 50000
            );
            const requestedThreshold = Number(data.thresholdScore);
            const thresholdScore = Math.max(0, Number.isFinite(requestedThreshold) ? requestedThreshold : 1000);
            const requestedPlanLength = Number(data.planLength);
            const planLength = Math.max(1, Math.min(12,
                Number.isFinite(requestedPlanLength) ? Math.floor(requestedPlanLength) : 6));

            if (data.replay) {
                const operationPages = Array.isArray(data.operationPages)
                    ? data.operationPages.map(Number).filter(index => Number.isInteger(index) && index >= 0 && index < pages.length)
                    : pages.map((page, index) => operationForWorker(page) ? index : -1).filter(index => index >= 0);
                const lastOrdinal = operationPages.length - 1;
                if (lastOrdinal < 0) {
                    self.postMessage({ type: 'done', runId: data.runId, results: [], total: 0 });
                    return;
                }
                const startOrdinal = Math.max(0, Math.min(lastOrdinal, Number(data.startPage) || 0));
                const endOrdinal = Math.max(startOrdinal, Math.min(lastOrdinal, Number(data.endPage) || lastOrdinal));
                const total = endOrdinal - startOrdinal + 1;
                const results = [];
                let context = { b2b: false, ren: -1 };
                let completed = 0;
                for (let ordinal = 0; ordinal <= endOrdinal; ordinal++) {
                    const pageIndex = operationPages[ordinal];
                    const operation = normalizeRecordedOperation(pages[pageIndex]?.p1?.operation);
                    const source = replaySourceAtPage(pages, data.replayInitial?.p1 || data.replayInitial || {}, pageIndex);
                    const scored = await scoreRecordedOperation(
                        pageIndex,
                        source,
                        operation,
                        context,
                        ordinal < startOrdinal ? 500 : nodeBudget,
                        ordinal < startOrdinal ? 500 : detailNodeBudget,
                        ordinal < startOrdinal ? Infinity : thresholdScore,
                        ordinal < startOrdinal ? 1 : planLength,
                        useWasm
                    );
                    context = scored.nextContext;
                    if (ordinal < startOrdinal) continue;
                    results.push(scored.result);
                    completed++;
                    self.postMessage({ type: 'progress', runId: data.runId, completed, total, result: scored.result });
                }
                self.postMessage({ type: 'done', runId: data.runId, results, total });
                return;
            }

            const lastMovePage = pages.length - 2;
        const startPage = Math.max(0, Math.min(lastMovePage, Number(data.startPage) || 0));
        const endPage = Math.max(startPage, Math.min(lastMovePage, Number(data.endPage) || lastMovePage));
        const total = endPage - startPage + 1;
        const results = [];
        let context = { b2b: false, ren: -1 };
        let completed = 0;

        if (lastMovePage < 0) {
            self.postMessage({ type: 'done', runId: data.runId, results, total: 0 });
            return;
        }

        // Establish B2B/REN only from literal four-cell reconstructions.
        // Passive post-clear pages deliberately preserve the prior context.
        for (let pageIndex = 0; pageIndex <= endPage; pageIndex++) {
            const source = pageState(pages[pageIndex]);
            const target = pageState(pages[pageIndex + 1]);
            if (pageIndex < startPage) {
                const primed = await scoreTransition(pageIndex, source, target, context, 500, 500, Infinity, 1, useWasm);
                context = primed.nextContext;
                continue;
            }

            const scored = await scoreTransition(pageIndex, source, target, context, nodeBudget, detailNodeBudget, thresholdScore, planLength, useWasm);
            context = scored.nextContext;
            results.push(scored.result);
            completed++;
            self.postMessage({ type: 'progress', runId: data.runId, completed, total, result: scored.result });
        }

        self.postMessage({ type: 'done', runId: data.runId, results, total });
    } catch (error) {
        self.postMessage({
            type: 'error',
            runId: data.runId,
            message: error && error.message ? error.message : String(error)
        });
    }
};
