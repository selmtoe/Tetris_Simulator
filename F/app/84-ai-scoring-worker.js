'use strict';

// Editor scoring is intentionally isolated from a running simulator.  The
// search implementation is the same Cold Clear port, but each page pair owns
// a scratch DAG in this worker.
importScripts('../../simulator/workers/cold-clear-core.js');

const { Search } = self.ColdClearSimulatorCore;
const PIECES = Object.freeze(['I', 'O', 'T', 'L', 'J', 'S', 'Z']);
const BOARD_WIDTH = 10;
const BOARD_HEIGHT = 40;

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
        // New operation pages store the current lock explicitly. `next` is
        // the queue after that lock and is derived from later operations.
        next: cleanPieces(p1.next),
        operation: p1.operation && typeof p1.operation === 'object' ? {
            piece: cleanPieces(p1.operation.type || p1.operation.piece)[0] || null,
            x: Number(p1.operation.x),
            y: Number(p1.operation.y),
            rotation: typeof p1.operation.rotation === 'number'
                ? ((p1.operation.rotation % 4) + 4) % 4
                : ({ spawn: 0, right: 1, reverse: 2, left: 3 }[String(p1.operation.rotation || 'spawn').toLowerCase()] ?? 0),
            hold: Boolean(p1.operation.hold || p1.operation.holdUsed || p1.operation.useHold)
        } : null
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

function normalizeEngineBoard(board) {
    if (board && board.rows && typeof board.rows.length === 'number') {
        return Array.from({ length: BOARD_HEIGHT }, (_, y) =>
            Array.from({ length: BOARD_WIDTH }, (_, x) => (board.rows[y] & (1 << x)) ? 'G' : null));
    }
    return normalizeLayout(board);
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
    // There is no valid P1 action to reconstruct without NEXT[0].  Do not
    // invent one of the seven pieces or treat NEXT[1] as the current mino.
    if (!source.next.length) return null;
    const search = new Search();
    search.synchronize({
        board: preMoveBoard,
        currentPiece: source.next[0],
        nextQueue: source.next.slice(1),
        holdPiece: source.hold,
        canHold: true,
        isB2B: Boolean(context.b2b),
        ren: Number.isFinite(context.ren) ? context.ren : -1
    });
    search.expand(search.root);
    return search;
}

function edgeCanBeRecordedMove(edge, source) {
    if (!edge.hold) return edge.placement.type === source.next[0];
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

function matchingEdges(search, source, target, delta) {
    if (!search || !search.root || !search.root.children) return [];
    const matches = [];
    for (const edge of search.root.children) {
        if (!edgeCanBeRecordedMove(edge, source)) continue;
        if (!sameCells(publicMove(edge).cells, delta.added)) continue;
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
    if (!delta.valid) return null;
    const search = createSearch(preMoveBoard, source, context);
    if (!search) return null;
    const matches = matchingEdges(search, source, target, delta);
    if (!matches.length) return null;
    return { search, matches, delta, preMoveBoard, kind, garbageRows };
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

    if (source.operation && source.operation.piece && Number.isFinite(source.operation.x) && Number.isFinite(source.operation.y)) {
        // The page itself is authoritative. Search is still used for the
        // exact Cold Clear score, but the recorded operation selects the
        // actual edge instead of reconstructing it from a four-cell delta.
        const op = source.operation;
        const searchSource = {
            ...source,
            next: op.hold ? (source.next.length ? source.next : [op.piece]) : [op.piece, ...source.next]
        };
        const search = createSearch(sourceBoard, searchSource, context);
        if (search && search.root && Array.isArray(search.root.children)) {
            const matches = search.root.children.filter(edge => {
                const placement = edge.placement || {};
                const move = publicMove(edge);
                return move.piece === op.piece &&
                    move.x === op.x && move.y === op.y &&
                    move.rotation === op.rotation &&
                    Boolean(edge.hold) === op.hold;
            });
            if (matches.length) {
                const edge = matches[0];
                const cells = publicMove(edge).cells;
                const targetOperation = target.operation && target.operation.piece
                    ? [target.operation.piece, ...target.next]
                    : target.next;
                const expected = expectedNext(search, edge.child.state);
                const queue = queueEvidence(expected, targetOperation);
                const holdExact = (edge.child.state.hold || null) === target.hold;
                if (sameOccupancy(normalizeEngineBoard(edge.child.state.board), targetBoard) && queue.compatible && holdExact) {
                    return {
                        search,
                        matches: [{ edge, state: { queue, holdExact } }],
                        delta: { added: cells, removed: [], valid: true },
                        preMoveBoard: sourceBoard,
                        kind: 'recorded-operation',
                        garbageRows: 0
                    };
                }
            }
        }
    }
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

function scorePair(search, actualEdge) {
    const bestEdge = search.best(0) || actualEdge;
    const actual = staticEdgeValue(search, actualEdge);
    const best = staticEdgeValue(search, bestEdge);
    const scoreGap = Number.isFinite(actual.value) && Number.isFinite(best.value)
        ? Math.max(0, best.value - actual.value) : 0;
    return { actual, best, bestEdge, scoreGap };
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

function observedMove(search, edge, addedCells) {
    const move = planMove(search, edge);
    // This is deliberately the literal page-to-page delta, never a visual
    // reconstruction from the edge.  The equality was checked before here.
    move.cells = addedCells.map(([x, y]) => [x, y]);
    return move;
}

function scoreTransition(pageIndex, source, target, context, nodeBudget, detailNodeBudget, thresholdScore, planLength) {
    if (sameSnapshot(source, target)) return ignoredResult(pageIndex, 'unchanged-snapshot', context, true);

    const reconstructed = reconstructTransition(source, target, context);
    if (!reconstructed) {
        return ignoredResult(pageIndex, 'invalid-page-delta', context, passiveBoardTransition(source, target));
    }

    const { search, matches, delta } = reconstructed;
    search.thinkNodes(nodeBudget, 3000);
    const actualMatch = selectMatchingEdge(search, matches);
    if (!actualMatch) return ignoredResult(pageIndex, 'invalid-page-delta', context, false);

    const actualEdge = actualMatch.edge;
    const roughNodes = search.nodeCount;
    const rough = scorePair(search, actualEdge);
    let final = rough;
    let detailed = false;
    if (meetsThreshold(rough.scoreGap, thresholdScore)) {
        // `thinkNodes` takes a total-DAG target, so this extends rather than
        // restarts the rough search.
        search.thinkNodes(detailNodeBudget, 10000);
        final = scorePair(search, actualEdge);
        detailed = true;
    }

    // Fill the requested known-piece PV after the score search. This can
    // expand a few deterministic child nodes, so refresh the values and the
    // best edge before publishing both the plan and the final node count.
    let aiPlan = principalVariation(search, final.bestEdge, planLength);
    final = scorePair(search, actualEdge);
    aiPlan = principalVariation(search, final.bestEdge, planLength);
    final = scorePair(search, actualEdge);
    // A small PV expansion can surface a rough-threshold gap that was not
    // visible at the original frontier. Finish it with the same detailed DAG
    // pass rather than emitting a partially searched blunder.
    if (!detailed && meetsThreshold(final.scoreGap, thresholdScore)) {
        search.thinkNodes(detailNodeBudget, 10000);
        detailed = true;
        final = scorePair(search, actualEdge);
        aiPlan = principalVariation(search, final.bestEdge, planLength);
        final = scorePair(search, actualEdge);
    }

    const { actual: actualScore, best: bestScore, bestEdge, scoreGap } = final;
    const result = {
        pageIndex,
        status: 'scored',
        reconstruction: reconstructed.kind,
        sourceConvention: 'next-first',
        targetConvention: 'next-first',
        sourceBoardVariant: reconstructed.kind === 'garbage-rise' ? 'garbage-baseline' : 'raw',
        targetBoardVariant: 'raw',
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
        roughNodes,
        nodes: search.nodeCount,
        detailed,
        detailNodes: detailed ? search.nodeCount : null,
        actualMove: observedMove(search, actualEdge, delta.added),
        bestMove: publicMove(bestEdge),
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
    return { result, nextContext: contextFromEdge(actualEdge) };
}

self.onmessage = event => {
    const data = event.data || {};
    if (data.type !== 'score') return;
    try {
        const pages = Array.isArray(data.pages) ? data.pages : [];
        const lastMovePage = pages.length - 2;
        const startPage = Math.max(0, Math.min(lastMovePage, Number(data.startPage) || 0));
        const endPage = Math.max(startPage, Math.min(lastMovePage, Number(data.endPage) || lastMovePage));
        const total = endPage - startPage + 1;
        const nodeBudget = Math.max(500, Math.floor(Number(data.nodeBudget) || 5000));
        const detailNodeBudget = Math.max(nodeBudget, 15000);
        const requestedThreshold = Number(data.thresholdScore);
        const thresholdScore = Math.max(0, Number.isFinite(requestedThreshold) ? requestedThreshold : 1000);
        const requestedPlanLength = Number(data.planLength);
        const planLength = Math.max(1, Math.min(12,
            Number.isFinite(requestedPlanLength) ? Math.floor(requestedPlanLength) : 6));
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
                const primed = scoreTransition(pageIndex, source, target, context, 500, 500, Infinity, 1);
                context = primed.nextContext;
                continue;
            }

            const scored = scoreTransition(pageIndex, source, target, context, nodeBudget, detailNodeBudget, thresholdScore, planLength);
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
