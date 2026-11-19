/* Independent one-shot queries; never alter a playing Cold Clear bot's DAG. */
'use strict';
importScripts('./pc-finder-worker.js', './cold-clear-core.js', './cold-clear-wasm.js?v=search-draws-v4');

function routeSnapshot(data) {
    const queue = [];
    for (const piece of data.nextQueue || []) {
        if (piece === 'E') break;
        if (!CC_PIECES.includes(piece)) throw new Error('NEXTを読み取れませんでした。');
        queue.push(piece);
    }
    if (!CC_PIECES.includes(data.currentPiece)) throw new Error('現在ミノがありません。');
    if (!Array.isArray(data.board) || data.board.length !== 40 || data.board.some(row => !Array.isArray(row) || row.length !== 10)) throw new Error('盤面を読み取れませんでした。');
    return { ...data, nextQueue: queue, canHold: data.canHold !== false && !data.holdDisabled };
}

function routeStep(placement, hold) {
    return { piece: placement.type, rotation: placement.rotation,
        x: placement.type === 'I' ? placement.x - 1 : placement.x, y: placement.y,
        hold, cells: ccCells(placement).map(([x, y]) => ({ x, y })) };
}

// Exact search of consecutive clearing locks, as in solution-finder's ren
// command. Memoization and the block-count bound do not discard valid routes.
// No random NEXT is invented. First clear = 0 REN, unless a REN is in progress.
function* renSearch(data) {
    const sequence = [data.currentPiece, ...data.nextQueue];
    const memo = new Map();
    let nodes = 0, best = [], lastYield = performance.now();
    const initialRen = Number.isInteger(data.ren) ? Math.max(-1, data.ren) : -1;
    const countBlocks = board => board.rows.reduce((sum, row) => {
        while (row) { sum++; row &= row - 1; } return sum;
    }, 0);
    const report = complete => ({ status: best.length ? 'found' : 'not_found', kind: 'ren',
        plan: best, depth: best.length, ren: initialRen + best.length, clears: best.length,
        complete, nodes, knownCount: sequence.length });

    function* visit(board, index, hold, canHold, path) {
        nodes++;
        if (path.length > best.length) best = [...path];
        if (performance.now() - lastYield > 24) {
            lastYield = performance.now(); yield report(false);
        }
        const upper = Math.min(sequence.length - index, Math.floor(countBlocks(board) / 6));
        if (upper <= 0) return [];
        const key = `${board.key()}/${index}/${hold || '-'}/${canHold ? 1 : 0}`;
        if (memo.has(key)) {
            const tail = memo.get(key);
            if (path.length + tail.length > best.length) best = [...path, ...tail];
            return tail;
        }
        const current = sequence[index];
        const choices = [{ piece: current, index: index + 1, hold, used: false }];
        if (canHold) {
            if (hold && hold !== current) choices.push({ piece: hold, index: index + 1, hold: current, used: true });
            else if (!hold && sequence[index + 1]) choices.push({ piece: sequence[index + 1], index: index + 2, hold: current, used: true });
        }
        let longest = [];
        for (const choice of choices) {
            const unique = new Set();
            for (const placement of ccFindMoves(board, choice.piece)) {
                const locked = board.lock(placement, false, 0);
                if (!locked.lock.lines || locked.lock.lockedOut) continue;
                const childKey = locked.board.key();
                if (unique.has(childKey)) continue;
                unique.add(childKey);
                const step = routeStep(placement, choice.used);
                const tail = yield* visit(locked.board, choice.index, choice.hold, !data.holdDisabled, [...path, step]);
                if (1 + tail.length > longest.length) longest = [step, ...tail];
                if (longest.length >= upper) break;
            }
            if (longest.length >= upper) break;
        }
        // Eviction only costs re-exploration; it never changes the answer.
        if (memo.size >= 80000) memo.clear();
        memo.set(key, longest);
        return longest;
    }
    yield* visit(CCBoard.fromSimulator(data.board), 0, data.holdPiece || null, data.canHold, []);
    return report(true);
}

async function aiSearch(data) {
    const bridge = await ColdClearWasmBridge.load('./cold-clear.wasm?v=search-draws-v4');
    const snapshot = { ...data, nodeLimit: data.nodeLimit || 120000 };
    const handle = bridge.create(snapshot);
    const plan = [];
    let board = CCBoard.fromSimulator(data.board);
    let current = data.currentPiece, hold = data.holdPiece || null, queue = [...data.nextQueue];
    try {
        // Only show the known portion of Cold Clear's route, never its bag guesses.
        while (current && plan.length < data.nextQueue.length + 1) {
            const limit = bridge.beginDecision(handle, snapshot.nodeLimit, data.incoming || 0);
            bridge.think(handle, data.thinkTimeMs || 50, limit);
            const move = bridge.suggest(handle, data.incoming || 0);
            if (!move) break;
            if (move.hold && data.holdDisabled) break;
            if (move.hold && !hold && !queue.length) break;
            const placement = ccFindMoves(board, move.piece).find(candidate => candidate.rotation === move.rotation &&
                candidate.x === move.x + (move.piece === 'I' ? 1 : 0) && candidate.y === move.y);
            if (!placement) throw new Error('Cold Clearの手順を検証できませんでした。');
            if (move.hold) {
                const previous = current; current = hold || queue.shift(); hold = previous;
            }
            if (current !== move.piece) throw new Error('Cold ClearのNEXTが一致しません。');
            plan.push(routeStep(placement, move.hold));
            const locked = board.lock(placement, false, 0);
            if (locked.lock.lockedOut) { plan.pop(); break; }
            board = locked.board;
            current = queue.shift();
            if (!bridge.commit(handle)) break;
        }
        return { status: plan.length ? 'found' : 'not_found', kind: 'ai', plan, depth: plan.length, complete: true };
    } finally { bridge.destroy(handle); }
}

self.onmessage = async event => {
    const input = event.data || {};
    if (input.type !== 'search') return;
    const send = (type, result) => self.postMessage({ type, requestId: input.requestId, kind: input.kind || 'pc', ...result });
    try {
        const data = routeSnapshot(input);
        if (data.kind === 'ren') {
            const iterator = renSearch(data);
            let lastReport = 0, lastDepth = -1;
            const advance = () => {
                try {
                    const result = iterator.next();
                    if (result.done || result.value.depth !== lastDepth || performance.now() - lastReport > 200) {
                        send(result.done ? 'result' : 'progress', result.value);
                        lastReport = performance.now(); lastDepth = result.value.depth;
                    }
                    if (!result.done) setTimeout(advance, 0);
                } catch (error) { send('error', { message: error.message }); }
            };
            advance();
        } else if (data.kind === 'ai') send('result', await aiSearch(data));
        else send('result', { ...(await findPerfectClear(data)), complete: true });
    } catch (error) { send('error', { message: error.message }); }
};
