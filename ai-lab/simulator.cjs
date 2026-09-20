'use strict';

// Only the clock, browser boundary and policies live here. All placements,
// movement, attacks, cancellation, garbage and phases belong to production Player.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { createHash } = require('node:crypto');
const { performance: wallClock } = require('node:perf_hooks');

const root = path.resolve(__dirname, '..');
const scripts = ['runtime-config.js', 'player-engine.js'].map(name => new vm.Script(
    fs.readFileSync(path.join(root, 'simulator/app', name), 'utf8'), { filename: name }
));
const clone = value => structuredClone(value);
const TICK = 10;

function seededRandom(seed) {
    let state = 2166136261;
    for (const char of String(seed)) state = Math.imul(state ^ char.charCodeAt(0), 16777619) >>> 0;
    return () => {
        state = (state + 0x6d2b79f5) >>> 0;
        let x = Math.imul(state ^ state >>> 15, state | 1);
        x ^= x + Math.imul(x ^ x >>> 7, x | 61);
        return ((x ^ x >>> 14) >>> 0) / 4294967296;
    };
}

// Classic scripts share lexical bindings; assigning context.gameSettings would
// NOT replace runtime-config's `let gameSettings`. Access them through this API.
function load(globals = {}) {
    const context = vm.createContext({
        console, structuredClone, Promise,
        Image: class Image {},
        document: { getElementById: () => null },
        playerInputSuspended: () => true,
        logCurrentGameState: () => {},
        ...globals
    });
    vm.runInContext('globalThis.window = globalThis;', context);
    for (const script of scripts) script.runInContext(context);
    const api = vm.runInContext(`({
        Player, settings: gameSettings, editor: editorData, bindings: keyBindings,
        width: BOARD_WIDTH, height: BOARD_HEIGHT, pieces: Object.keys(TETROMINOS),
        sequence: createMinoGenerator,
        start(list) { players = list; gameMode = '2P'; gameState = 'PLAYING';
            gameStartTime = performance.now(); resetGarbageDeliveryBatch(); },
        stop() { gameState = 'EDITING'; resetGarbageDeliveryBatch(); },
        flush() { flushGarbageDeliveryBatch(); },
        hasDeliveries() { return garbageDeliveryBatch.length > 0; }
    })`, context);
    return { context, api };
}

/**
 * Policy: choose(snapshot) -> move (or Promise); optional reset(), commit(),
 * addNextPiece(letter), close(). Calls are ordered and computation consumes no
 * virtual time. Null/error/unreachable moves reject the match. Only an explicitly
 * proven {noLegalMove:true, verified:true} invokes production no-legal defeat.
 *
 * snapshot: production analyze fields, plus seat, nowMs, incomingPackets,
 * phase, pieces, averagePieceMs, opponent (same public state), timing and budget.
 * Boards are 40x10 top-down null/letters; x/y/rotation use Player coordinates.
 * `incoming` is a line count; packet arrivalMs and all times are match-relative.
 *
 * createSandbox exposes the actual Player and mutable boards for focused tests.
 * Match traces include game events/state, excluding rendering-only counters.
 */
function createSandbox(options = {}) {
    const seed = options.seed ?? 1;
    const policies = options.policies ?? [() => { throw Error('Missing policy'); }, () => { throw Error('Missing policy'); }];
    if (!Array.isArray(policies) || policies.length !== 2) throw Error('Exactly two policies required');
    for (const policy of policies) {
        if (typeof policy !== 'function' && typeof policy?.choose !== 'function') throw Error('Policy must implement choose(snapshot)');
    }
    const clock = { nowMs: 0 };
    const timers = new Map();
    const workers = [];
    const messages = [];
    const executions = new Set();
    const trace = [];
    const digest = createHash('sha256');
    const stats = [0, 1].map(() => ({ pieces: 0, attack: 0, sent: 0, cancelled: 0,
        lines: 0, maxHeight: 0, received: 0, risen: 0, decisions: 0, nodeCount: 0 }));
    const consumedByRise = [0, 0];
    let nextTimer = 1, failure = null, closed = false, updates = 0;
    let random = seededRandom(`${seed}:runtime`);
    const garbageRandom = [0, 1].map(seat => seededRandom(`${seed}:garbage:${seat}`));
    let players, api;
    const record = (type, data = {}) => {
        const event = { timeMs: clock.nowMs, type, ...data };
        const json = JSON.stringify(event);
        digest.update(json).update('\n');
        if (options.trace) trace.push(JSON.parse(json));
    };
    const withRandom = (rng, action) => {
        const previous = random;
        random = rng;
        try { return action(); } finally { random = previous; }
    };
    const fail = (seat, error) => {
        failure ??= new Error(`Player ${seat} at ${clock.nowMs}ms: ${error.message ?? error}`, { cause: error });
        failure.seat = seat;
        failure.timeMs = clock.nowMs;
        failure.seed = seed;
        if (error.code) failure.code = error.code;
        if (players) failure.snapshot ??= clone({ ...state(seat), opponent: state(1 - seat), nowMs: clock.nowMs });
    };
    const check = () => { if (failure) throw failure; };
    const math = Object.create(Math);
    math.random = () => random();
    class PolicyWorker {
        constructor() { this.seat = workers.length; workers.push(this); }
        postMessage(data) {
            // Clone at SEND time, just as a browser Worker does. In particular,
            // later opponent locks cannot change a pending decision snapshot.
            const payload = data.type === 'analyze' ? decisionSnapshot(this.seat, data) : clone(data);
            messages.push({ seat: this.seat, data: payload });
        }
    }
    const loaded = load({
        Math: math, Worker: PolicyWorker,
        performance: { now: () => 1 + clock.nowMs },
        setTimeout(callback, delay = 0, ...args) {
            const id = nextTimer++;
            const due = clock.nowMs + Math.max(0, Number(delay) || 0);
            if (!Number.isFinite(due)) throw Error('Non-finite virtual timer');
            timers.set(id, { id, due, callback, args });
            record('timer-set', { id, due });
            return id;
        },
        clearTimeout(id) { timers.delete(id); }
    });
    api = loaded.api;
    const { initialBoards: ruleBoards, ...ruleOverrides } = options.rules ?? {};
    for (const [key, value] of Object.entries(ruleOverrides)) {
        if (!(key in api.settings)) throw Error(`Unknown production rule: ${key}`);
        api.settings[key] = clone(value);
    }
    // These modes inject human/custom behavior and invalidate idle-tick skipping.
    if (api.settings.pieceForPieceMode) throw Error('pieceForPieceMode is unsupported for AI-vs-AI');
    for (const key of ['aiThinkTime', 'aiMoveDelay', 'aiSdfDelay', 'lineClearDelay', 'spawnDelay', 'garbageGrace']) {
        if (!Number.isFinite(api.settings[key]) || api.settings[key] < 0) throw Error(`Invalid rule ${key}`);
    }
    if (!Number.isInteger(api.settings.maxNext) || api.settings.maxNext < 1) throw Error('maxNext must be a positive integer');
    const maxPieces = options.maxPieces ?? 600;
    if (!Number.isInteger(maxPieces) || maxPieces < 1) throw Error('maxPieces must be a positive integer');
    // Both seats receive the same real production seven-bag sequence. It is
    // generated independently of garbage and contains more than the match needs.
    const queue = withRandom(seededRandom(`${seed}:queue`), () => {
        const generator = api.sequence();
        return Array.from({ length: maxPieces + api.settings.maxNext + 16 }, () => generator.next().value);
    });
    const initialBoards = options.initialBoards ?? ruleBoards;
    for (let seat = 0; seat < 2; seat++) {
        const board = initialBoards?.[seat] ?? Array.from({ length: api.height }, () => Array(api.width).fill(null));
        if (board.length !== api.height || board.some(row => !Array.isArray(row) || row.length !== api.width ||
            row.some(cell => cell !== null && (typeof cell !== 'string' || !cell)))) throw Error('Board must be 40x10 null/occupied strings');
        const customQueue = options.queues?.[seat] ?? queue;
        if (!Array.isArray(customQueue) || !customQueue.length || customQueue.some(piece => !api.pieces.includes(piece))) throw Error('Invalid piece queue');
        api.editor[`p${seat + 1}`].board = clone(board);
        api.editor[`p${seat + 1}`].nextQueue = [...customQueue, ...queue];
        api.editor[`p${seat + 1}`].hold = options.holds?.[seat] ?? null;
        if (api.editor[`p${seat + 1}`].hold && !api.pieces.includes(api.editor[`p${seat + 1}`].hold)) throw Error('Invalid HOLD');
    }
    players = [0, 1].map(seat => new api.Player(String(seat + 1), 0, api.bindings[`p${seat + 1}`], seat, true));
    players[0].opponent = players[1];
    players[1].opponent = players[0];
    api.start(players);

    function height(board) {
        const row = board.findIndex(row => row.some(cell => cell !== null));
        return row < 0 ? 0 : board.length - row;
    }
    function state(seat) {
        const player = players[seat];
        const observation = player.aiPlayerSnapshot(player, clock.nowMs);
        // Production's helper has no spawn-delay phase and retains the locked
        // piece. Expose the actual phase/deadline and raw active position as well.
        if (player.isSpawning) observation.phase = { kind: 'spawn', endsMs: player.spawnDelayEndsAtMs - 1 };
        return {
            ...observation,
            incomingPackets: observation.incoming,
            incoming: player.pendingGarbage + player.garbageQueue.reduce((sum, packet) => sum + packet.lines, 0),
            pendingGarbage: player.pendingGarbage,
            activePiece: { ...player.player },
            canHold: player.canHold && !player.holdDisabled,
            gameOver: player.gameOver, gameClear: player.gameClear,
            maxHeight: height(player.board)
        };
    }
    function decisionSnapshot(seat, payload) {
        return clone({
            ...payload, ...state(seat), seat, nowMs: clock.nowMs,
            opponent: state(1 - seat), hasOpponent: true,
            budget: { nodeLimit: payload.nodeLimit, thinkTimeMs: payload.thinkTimeMs },
            timing: {
                inputIntervalMs: api.settings.aiMoveDelay, softDropIntervalMs: api.settings.aiSdfDelay,
                lineClearDelayMs: api.settings.lineClearDelay, spawnDelayMs: api.settings.spawnDelay,
                garbageGraceMs: api.settings.garbageGrace, decisionLatencyMs: api.settings.aiThinkTime
            }
        });
    }
    function observe(type, seat, data = {}) {
        stats[seat].maxHeight = Math.max(stats[seat].maxHeight, height(players[seat].board));
        record(type, { seat, ...data, state: state(seat) });
    }
    function wrap(player, name, callback) {
        const original = player[name];
        player[name] = function (...args) { return callback(original.bind(this), ...args); };
    }
    for (const [seat, player] of players.entries()) {
        stats[seat].maxHeight = height(player.board);
        wrap(player, 'executeAiMove', (original, move) => {
            const before = player.pieceCount;
            const promise = original(move);
            executions.add(promise);
            promise.then(() => {
                executions.delete(promise);
                if (!closed && player.pieceCount === before && !player.gameOver && !player.gameClear) fail(seat, Error('Move execution did not lock a piece'));
            }, error => { executions.delete(promise); fail(seat, error); });
            return promise;
        });
        wrap(player, 'invalidateAiSearch', (original, reason) => {
            if (reason === 'unreachable placement' || reason === 'stale placement') throw Error(reason);
            return original(reason);
        });
        wrap(player, 'hold', original => { original(); observe('hold', seat); });
        wrap(player, 'clearLines', original => {
            stats[seat].maxHeight = Math.max(stats[seat].maxHeight, height(player.board));
            return original();
        });
        wrap(player, 'lockPiece', original => {
            const before = state(seat).incoming;
            const receiverBefore = players[1 - seat].garbageQueue.reduce((sum, p) => sum + p.lines, 0);
            const oldConsumed = consumedByRise[seat];
            let sent = 0;
            // Observe production queueGarbageDelivery rather than recalculate attack.
            loaded.context.__observeSend = (source, target, lines) => {
                const sender = players.indexOf(source);
                record('send', { seat: sender, target: players.indexOf(target), lines });
                if (sender === seat) sent += lines;
            };
            try { original(); } finally { loaded.context.__observeSend = null; }
            const cancelled = before - state(seat).incoming - (consumedByRise[seat] - oldConsumed);
            stats[seat].pieces = player.pieceCount;
            stats[seat].lines += player.linesClearedLastLock;
            stats[seat].sent += sent;
            stats[seat].cancelled += cancelled;
            stats[seat].attack += sent + cancelled;
            if (receiverBefore !== players[1 - seat].garbageQueue.reduce((sum, p) => sum + p.lines, 0)) throw Error('Production delivery escaped the batch');
            observe('lock', seat, { lines: player.linesClearedLastLock, attack: sent + cancelled, sent, cancelled });
        });
        wrap(player, 'addGarbage', (original, lines) => {
            original(lines); stats[seat].received += lines; observe('receive', seat, { lines });
        });
        wrap(player, 'processGarbageQueue', original => {
            const pending = player.pendingGarbage;
            original();
            if (pending !== player.pendingGarbage) observe('mature', seat, { lines: player.pendingGarbage - pending });
        });
        wrap(player, 'riseGarbage', original => {
            if (player.pendingGarbage <= 0) return original();
            // Count actual shifted rows, including a partial rise before top-out.
            let count = 0;
            const pending = player.pendingGarbage;
            const board = player.board;
            const shift = board.shift;
            board.shift = function () { count++; return shift.call(this); };
            try { withRandom(garbageRandom[seat], original); }
            finally { delete board.shift; }
            stats[seat].risen += count;
            consumedByRise[seat] += pending - player.pendingGarbage;
            observe('rise', seat, { lines: count });
        });
    }
    vm.runInContext(`{
        const productionQueue = queueGarbageDelivery;
        queueGarbageDelivery = function(source, target, lines) {
            if (globalThis.__observeSend) globalThis.__observeSend(source, target, lines);
            return productionQueue(source, target, lines);
        };
    }`, loaded.context);
    record('start', { seed, rules: clone(api.settings), players: [state(0), state(1)] });

    async function settle() {
        // A VM async function awaiting our host Promise crosses two microtask
        // queues. Drain both before inspecting newly scheduled virtual timers.
        for (let guard = 0; guard < 100000; guard++) {
            await Promise.resolve();
            await Promise.resolve();
            check();
            if (messages.length) {
                const { seat, data } = messages.shift();
                const policy = policies[seat];
                try {
                    if (data.type === 'analyze') {
                        stats[seat].decisions++;
                        record('choose', { seat, snapshot: data });
                        const choose = typeof policy === 'function' ? policy : policy.choose.bind(policy);
                        const move = await choose(clone(data));
                        const work = Number(move?.stats?.nodesAdded ?? move?.nodesAdded ?? move?.nodes ?? 0);
                        if (Number.isFinite(work) && work > 0) stats[seat].nodeCount += work;
                        if (move?.noLegalMove === true && move.verified === true) {
                            record('no-legal-move', { seat, requestId: data.requestId });
                            workers[seat].onmessage({ data: { type: 'noLegalMove', requestId: data.requestId } });
                            continue;
                        }
                        if (!move || !api.pieces.includes(move.piece) || !Number.isInteger(move.x) || !Number.isInteger(move.y) ||
                            !Number.isInteger(move.rotation) || move.rotation < 0 || move.rotation > 3) throw Error('Policy returned no move or an invalid move');
                        for (const key of ['waitMs', 'controllerMs']) if (move[key] !== undefined && (!Number.isFinite(move[key]) || move[key] < 0)) throw Error(`Invalid move ${key}`);
                        if (move.hold === true || move.piece !== data.currentPiece) {
                            if (!data.canHold || move.piece !== (data.holdPiece ?? data.nextQueue[0])) throw Error('Invalid HOLD move');
                        }
                        record('move', { seat, move: Object.fromEntries(['piece', 'x', 'y', 'rotation', 'hold', 'tspin', 'waitMs', 'controllerMs', 'controllerInputs', 'nodes', 'depth'].filter(key => move[key] !== undefined).map(key => [key, move[key]])) });
                        workers[seat].onmessage({ data: { ...clone(move), type: 'move', requestId: data.requestId } });
                    } else {
                        record('policy', { seat, message: data });
                        const method = { reset: 'reset', commit: 'commit', addNextPiece: 'addNextPiece' }[data.type];
                        if (method && typeof policy[method] === 'function') await policy[method](...(method === 'addNextPiece' ? [data.piece] : []));
                    }
                } catch (error) { fail(seat, error); }
                check();
                continue;
            }
            const due = [...timers.values()].filter(timer => timer.due <= clock.nowMs).sort((a, b) => a.due - b.due || a.id - b.id);
            if (!due.length) return;
            // Resolve every simultaneous timer before running continuations;
            // outgoing attacks stay batched until the next production tick.
            for (const timer of due) {
                if (!timers.delete(timer.id)) continue;
                record('timer-fire', { id: timer.id });
                timer.callback(...timer.args);
            }
        }
        throw Error('Virtual event runaway');
    }
    function tick(dt = TICK) {
        check();
        api.flush();
        for (const player of players) player.update(dt);
        api.flush();
        updates++;
    }
    function nextUsefulTick(nextTick) {
        if (api.hasDeliveries()) return nextTick;
        let next = Infinity;
        const atOrAfter = value => Math.max(nextTick, Math.ceil(value / TICK) * TICK);
        for (const player of players) {
            if (player.gameOver || player.gameClear) continue;
            if (player.isClearingLine || player.isSpawning) {
                const end = (player.isClearingLine ? player.lineClearDelayEndsAtMs : player.spawnDelayEndsAtMs) - 1;
                next = Math.min(next, atOrAfter(end));
            } else if (!player.isAiThinking) next = Math.min(next, nextTick);
            for (const packet of player.garbageQueue) {
                // Strict `>`: equality must wait one more tick, including grace=0.
                const deadline = packet.receivedTime - 1 + api.settings.garbageGrace;
                next = Math.min(next, Math.max(nextTick, (Math.floor(deadline / TICK) + 1) * TICK));
            }
        }
        return next;
    }
    async function close() {
        if (closed) return;
        closed = true;
        api.stop();
        for (const player of players) player.isAiThinking = false;
        // Release pending execution Promises so early limits do not retain VMs.
        for (const timer of timers.values()) timer.callback(...timer.args);
        timers.clear();
        await Promise.allSettled([...executions]);
        const results = await Promise.allSettled(policies.map(policy => Promise.resolve().then(() => policy.close?.())));
        const errors = results.filter(result => result.status === 'rejected').map(result => result.reason);
        if (errors.length) throw new AggregateError(errors, 'Policy close failed');
    }
    return { ...loaded, Player: api.Player, players, settings: api.settings, clock, timers, stats, trace,
        snapshot: seat => clone(state(seat)), settle, tick, nextUsefulTick, close, record, check,
        get updates() { return updates; }, traceDigest: () => digest.copy().digest('hex') };
}

/**
 * Frame reference: ticks every 10ms, exact timer deadlines between ticks.
 * Event mode skips ticks only until a decision, phase end, packet maturity or
 * delivery can change gameplay. Both modes flush before/after the SAME tick,
 * process simultaneous timers together, and never sleep against wall time.
 * winner is seat 0/1/null; limits produce draws. nodeCount sums reported work
 * (stats.nodesAdded, nodesAdded, or nodes when the policy exposes only that).
 */
async function runMatch(options = {}) {
    const started = wallClock.now();
    const mode = options.mode ?? 'event';
    if (!['event', 'frame'].includes(mode)) throw Error('mode must be event or frame');
    const maxTimeMs = options.maxTimeMs ?? 120000;
    if (!Number.isFinite(maxTimeMs) || maxTimeMs < 0) throw Error('maxTimeMs must be finite and nonnegative');
    const maxPieces = options.maxPieces ?? 600;
    const sandbox = createSandbox(options);
    let nextTick = TICK, lastTick = 0, reason;
    let matchError;
    try {
        const terminal = () => {
            if (sandbox.players.some(player => player.gameOver || player.gameClear)) return 'top-out';
            if (sandbox.players.some(player => player.pieceCount >= maxPieces)) return 'piece-limit';
            if (sandbox.clock.nowMs >= maxTimeMs) return 'time-limit';
            return null;
        };
        reason = terminal();
        if (!reason) sandbox.tick(0);
        await sandbox.settle();
        while (!(reason = terminal())) {
            const tickAt = mode === 'frame' ? nextTick : sandbox.nextUsefulTick(nextTick);
            let next = Math.min(tickAt, maxTimeMs);
            for (const timer of sandbox.timers.values()) next = Math.min(next, timer.due);
            if (next < sandbox.clock.nowMs) throw Error('Virtual clock moved backwards');
            sandbox.clock.nowMs = next;
            await sandbox.settle();
            const usefulNow = mode === 'event' && next >= nextTick && next % TICK === 0 && sandbox.nextUsefulTick(next) <= next;
            if (!terminal() && (next === tickAt || usefulNow)) {
                sandbox.tick(next - lastTick);
                lastTick = next;
                nextTick = next + TICK;
                await sandbox.settle();
            } else if (mode === 'event') nextTick = Math.max(nextTick, Math.floor(next / TICK) * TICK + TICK);
        }
        sandbox.check();
        const dead = sandbox.players.map((player, seat) => player.gameOver || (sandbox.players[1 - seat].gameClear && !player.gameClear));
        const winner = dead[0] === dead[1] ? null : dead[0] ? 1 : 0;
        const result = {
            winner, outcome: winner === null ? 'draw' : 'win', draw: winner === null, reason,
            durationMs: sandbox.clock.nowMs, players: clone(sandbox.stats),
            decisions: sandbox.stats.reduce((sum, stats) => sum + stats.decisions, 0),
            nodeCount: sandbox.stats.reduce((sum, stats) => sum + stats.nodeCount, 0),
            finalState: [sandbox.snapshot(0), sandbox.snapshot(1)],
            trace: sandbox.trace, ticks: sandbox.updates
        };
        sandbox.record('end', { winner, reason, durationMs: result.durationMs, players: result.players, finalState: result.finalState });
        result.traceDigest = sandbox.traceDigest();
        return { ...result, wallMs: wallClock.now() - started };
    } catch (error) { matchError = error; throw error; }
    finally {
        try { await sandbox.close(); }
        catch (error) { if (!matchError) throw error; }
    }
}

module.exports = { runMatch, createSandbox, load, seededRandom };
