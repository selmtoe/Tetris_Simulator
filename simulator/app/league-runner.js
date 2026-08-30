/* Deterministic paired league runner built on the production Player class. */

(() => {
    'use strict';

    if (window.TetrisLeague) return;

    const SCHEMA = 'tetris-simulator-league.v1';
    const PIECES = Object.freeze(['I', 'O', 'T', 'L', 'J', 'S', 'Z']);
    const nativeSetTimeout = window.setTimeout;
    const nativeClearTimeout = window.clearTimeout;
    const nativeRequestAnimationFrame = window.requestAnimationFrame;
    const nativePerformanceNow = performance.now.bind(performance);
    const productionFlushGarbage = flushGarbageDeliveryBatch;
    let activeSession = null;
    let leagueRunning = false;

    // The regular rendering loop is still alive while a league match owns the
    // Player instances. Do not let that loop consume the deterministic garbage
    // batch; the league's fixed tick flushes it with both Players in scope.
    flushGarbageDeliveryBatch = function leagueAwareGarbageFlush() {
        if (activeSession && !activeSession.allowGarbageFlush) return;
        return productionFlushGarbage();
    };

    function emptyBoard() {
        return Array.from({ length: BOARD_HEIGHT }, () => Array(BOARD_WIDTH).fill(null));
    }

    function clone(value) {
        if (typeof structuredClone === 'function') return structuredClone(value);
        return JSON.parse(JSON.stringify(value));
    }

    function clampInteger(value, minimum, maximum, fallback) {
        const numeric = Number(value);
        if (!Number.isFinite(numeric)) return fallback;
        return Math.max(minimum, Math.min(maximum, Math.floor(numeric)));
    }

    function hashSeed(value) {
        const text = String(value ?? '0');
        let hash = 2166136261;
        for (let index = 0; index < text.length; index++) {
            hash ^= text.charCodeAt(index);
            hash = Math.imul(hash, 16777619);
        }
        hash ^= hash >>> 16;
        hash = Math.imul(hash, 0x7feb352d);
        hash ^= hash >>> 15;
        hash = Math.imul(hash, 0x846ca68b);
        return (hash ^ (hash >>> 16)) >>> 0;
    }

    function deriveSeed(seed, label) {
        return hashSeed(`${seed >>> 0}:${label}`);
    }

    function mulberry32(seed) {
        let state = seed >>> 0;
        return () => {
            state = (state + 0x6d2b79f5) >>> 0;
            let value = state;
            value = Math.imul(value ^ (value >>> 15), value | 1);
            value ^= value + Math.imul(value ^ (value >>> 7), value | 61);
            return ((value ^ (value >>> 14)) >>> 0) / 4294967296;
        };
    }

    function createPieceSequence(seed, minimumPieces) {
        const random = mulberry32(seed);
        const sequence = [];
        while (sequence.length < minimumPieces) {
            const bag = [...PIECES];
            for (let index = bag.length - 1; index > 0; index--) {
                const swapIndex = Math.floor(random() * (index + 1));
                [bag[index], bag[swapIndex]] = [bag[swapIndex], bag[index]];
            }
            sequence.push(...bag);
        }
        return sequence;
    }

    function normalizedBoard(board) {
        if (!Array.isArray(board) || board.length !== BOARD_HEIGHT) return emptyBoard();
        return board.map(row => {
            if (!Array.isArray(row) || row.length !== BOARD_WIDTH) return Array(BOARD_WIDTH).fill(null);
            return row.map(cell => cell === null ? null : String(cell || 'G').slice(0, 1));
        });
    }

    function scenarioBoards(name, suppliedBoards) {
        if (suppliedBoards?.p1 || suppliedBoards?.p2) {
            return {
                name: 'custom',
                p1: normalizedBoard(suppliedBoards.p1),
                p2: normalizedBoard(suppliedBoards.p2)
            };
        }
        if (name === 'near-top-p1') {
            const p1 = emptyBoard();
            for (let row = BOARD_HEIGHT - 19; row < BOARD_HEIGHT; row++) {
                const hole = (row - (BOARD_HEIGHT - 19)) % 2 === 0 ? 0 : BOARD_WIDTH - 1;
                p1[row] = Array(BOARD_WIDTH).fill('G');
                p1[row][hole] = null;
            }
            return { name, p1, p2: emptyBoard() };
        }
        if (name === 'spawn-pocket-p1') {
            const p1 = emptyBoard();
            for (let row = BOARD_HEIGHT - 19; row < BOARD_HEIGHT; row++) {
                const hole = (row - (BOARD_HEIGHT - 19)) % 2 === 0 ? 0 : BOARD_WIDTH - 1;
                p1[row] = Array(BOARD_WIDTH).fill('G');
                p1[row][hole] = null;
            }
            for (const row of [19, 20]) {
                p1[row] = Array(BOARD_WIDTH).fill('G');
                for (let column = 3; column <= 6; column++) p1[row][column] = null;
            }
            return { name, p1, p2: emptyBoard() };
        }
        return { name: 'empty', p1: emptyBoard(), p2: emptyBoard() };
    }

    function percentile(values, ratio) {
        if (!values.length) return null;
        const sorted = [...values].sort((left, right) => left - right);
        const index = Math.min(sorted.length - 1, Math.max(0, Math.ceil(sorted.length * ratio) - 1));
        return Number(sorted[index].toFixed(3));
    }

    function summarizeSamples(values) {
        if (!values.length) {
            return { samples: 0, meanMs: null, p50Ms: null, p95Ms: null, maxMs: null };
        }
        const total = values.reduce((sum, value) => sum + value, 0);
        return {
            samples: values.length,
            meanMs: Number((total / values.length).toFixed(3)),
            p50Ms: percentile(values, 0.5),
            p95Ms: percentile(values, 0.95),
            maxMs: Number(Math.max(...values).toFixed(3))
        };
    }

    function addCount(target, key, amount = 1) {
        const normalized = String(key || 'Unknown');
        target[normalized] = (target[normalized] || 0) + amount;
    }

    function incomingLines(player) {
        return player.pendingGarbage + player.garbageQueue.reduce((sum, packet) => sum + packet.lines, 0);
    }

    function attackForCompletedLock(before, player) {
        const lines = player.linesClearedLastLock;
        if (lines <= 0) return 0;

        if (player.stats.perfectClear > before.perfectClear) return 10;

        let attack;
        if (before.tspin === 'TSPIN') {
            attack = [0, 2, 4, 6][lines] || 0;
        } else if (before.tspin === 'MINI_TSPIN') {
            attack = [0, 0, 1, 2, 4][lines] || 0;
        } else {
            attack = [0, 0, 1, 2, 4][lines] || 0;
        }

        const b2bEligible = lines === 4 || Boolean(before.tspin);
        if (b2bEligible && before.isB2B) attack++;
        const ren = before.ren + 1;
        const renBonus = [0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 4, 5][Math.min(ren, 13)] || 0;
        return attack + renBonus;
    }

    function makePlayerMetric(seat, model, garbageSeed, chargeReleaseWindowMs) {
        return {
            seat,
            model,
            garbageSeed,
            locks: 0,
            linesCleared: 0,
            attacks: {
                raw: 0,
                cancelled: 0,
                sent: 0,
                delivered: 0,
                received: 0
            },
            intents: {},
            strategyEvents: {},
            policyOverrides: {},
            moves: [],
            waits: { count: 0, totalMs: 0, maxMs: 0 },
            holdFire: {
                count: 0,
                receivedDuringWait: 0,
                totalWaitMs: 0,
                byIntent: {},
                events: []
            },
            garbageReceivedEvents: [],
            garbageRiseEvents: [],
            ren: {
                max: -1,
                starts: 0,
                continuations: 0,
                bonusAttack: 0
            },
            charge: {
                moves: 0,
                consecutive: {
                    sequences: 0,
                    totalLength: 0,
                    maxLength: 0,
                    lengths: [],
                    histogram: {}
                },
                firstAttackAfterCharge: { count: 0, byIntent: {}, events: [] },
                chargeRelease: {
                    count: 0,
                    byIntent: {},
                    windowMs: chargeReleaseWindowMs,
                    events: []
                }
            },
            noLegalMove: { count: 0, firstAtMs: null },
            workerErrors: [],
            nodeCount: { samples: 0, last: null, max: null },
            _workerResponseMs: [],
            _reportedSearchMs: [],
            _requestRealMs: new Map(),
            _garbageRandom: mulberry32(garbageSeed),
            _chargeStreak: null,
            _pendingCharge: null,
            _activeDecision: null
        };
    }

    function finalizeChargeStreak(metric, endedAtMs) {
        const streak = metric._chargeStreak;
        if (!streak) return;
        const consecutive = metric.charge.consecutive;
        consecutive.sequences++;
        consecutive.totalLength += streak.length;
        consecutive.maxLength = Math.max(consecutive.maxLength, streak.length);
        consecutive.lengths.push(streak.length);
        addCount(consecutive.histogram, streak.length);
        metric._pendingCharge = { ...streak, endedAtMs };
        metric._chargeStreak = null;
    }

    function observeIntent(metric, decision) {
        const intent = decision.intent;
        const strategyEvent = decision.strategyEvent;
        const isChargeStep = intent === 'Charge'
            || strategyEvent === 'Charge enter'
            || strategyEvent === 'Charge continue'
            || strategyEvent === 'Armed';
        if (isChargeStep) {
            metric.charge.moves++;
            if (!metric._chargeStreak) {
                metric._chargeStreak = {
                    startedAtMs: decision.atMs,
                    lastChargeAtMs: decision.atMs,
                    length: 0
                };
            }
            metric._chargeStreak.length++;
            metric._chargeStreak.lastChargeAtMs = decision.atMs;
            metric._pendingCharge = { ...metric._chargeStreak, endedAtMs: null };
            return;
        }

        finalizeChargeStreak(metric, decision.atMs);
        if (strategyEvent === 'Charge abort') {
            metric._pendingCharge = null;
            return;
        }
        const isIncomingEdgeRelease = strategyEvent === 'Charge release incoming edge';
        const isGarbageRiseRelease = strategyEvent === 'Charge release garbage rise edge';
        const releaseClass = isIncomingEdgeRelease
            ? 'Charge emergency counter'
            : isGarbageRiseRelease
                ? 'Charge after garbage rise'
                : strategyEvent === 'Release dodge'
            ? 'Cancellation dodge'
            : strategyEvent === 'Release counter'
                ? 'Counter'
                : strategyEvent === 'Release immediate'
                    ? 'Immediate'
                    : intent === 'Spike now' || intent === 'Spike'
                        ? 'Spike'
                        : intent === 'Cancellation dodge' || intent === 'Counter'
                            ? intent
                            : null;
        if (!releaseClass || !metric._pendingCharge) return;

        const charge = metric._pendingCharge;
        const edgeEvents = isGarbageRiseRelease
            ? metric.garbageRiseEvents
            : metric.garbageReceivedEvents;
        const receipt = [...edgeEvents]
            .reverse()
            .find(event => event.atMs >= charge.startedAtMs && event.atMs <= decision.atMs);
        const firstAttackEvent = {
            chargeStartedAtMs: charge.startedAtMs,
            chargeEndedAtMs: charge.endedAtMs ?? charge.lastChargeAtMs,
            chargeLength: charge.length,
            releaseAtMs: decision.atMs,
            releaseIntent: intent,
            releaseClass,
            receivedAtMs: receipt?.atMs ?? null,
            receivedLines: receipt?.lines ?? 0,
            receiveToReleaseMs: receipt ? decision.atMs - receipt.atMs : null,
            observedEdge: isGarbageRiseRelease
                ? 'garbage-rise'
                : isIncomingEdgeRelease
                    ? 'incoming-queue'
                    : null
        };
        metric.charge.firstAttackAfterCharge.count++;
        addCount(metric.charge.firstAttackAfterCharge.byIntent, releaseClass);
        metric.charge.firstAttackAfterCharge.events.push(firstAttackEvent);

        const isChargeRelease = isGarbageRiseRelease || (receipt
            && firstAttackEvent.receiveToReleaseMs >= 0
            && firstAttackEvent.receiveToReleaseMs <= metric.charge.chargeRelease.windowMs);
        if (isChargeRelease) {
            const releaseEvent = { ...firstAttackEvent, attack: null };
            const eventIndex = metric.charge.chargeRelease.events.length;
            metric.charge.chargeRelease.count++;
            addCount(metric.charge.chargeRelease.byIntent, releaseClass);
            metric.charge.chargeRelease.events.push(releaseEvent);
            decision.chargeRelease = true;
            decision.chargeReleaseEventIndex = eventIndex;
        } else {
            decision.chargeRelease = false;
        }
        metric._pendingCharge = null;
    }

    function patchPlayerForLeague(player, metric, session, headless) {
        player.__leagueMetric = metric;

        const productionLockPiece = player.lockPiece.bind(player);
        player.lockPiece = function instrumentedLockPiece(...args) {
            const before = {
                incoming: incomingLines(this),
                isB2B: this.isB2B,
                ren: this.ren,
                tspin: this.checkForTSpin(),
                perfectClear: this.stats.perfectClear
            };
            const result = productionLockPiece(...args);
            const rawAttack = attackForCompletedLock(before, this);
            const completedRen = this.linesClearedLastLock > 0 ? before.ren + 1 : -1;
            const renBonus = this.linesClearedLastLock > 0
                ? ([0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 4, 5][Math.min(completedRen, 13)] || 0)
                : 0;
            const cancelled = Math.min(rawAttack, Math.max(0, before.incoming - incomingLines(this)));
            metric.locks++;
            metric.linesCleared += this.linesClearedLastLock;
            metric.attacks.raw += rawAttack;
            metric.attacks.cancelled += cancelled;
            metric.attacks.sent += Math.max(0, rawAttack - cancelled);
            metric.ren.max = Math.max(metric.ren.max, completedRen);
            if (completedRen === 0) metric.ren.starts++;
            if (completedRen > 0) metric.ren.continuations++;
            metric.ren.bonusAttack += renBonus;
            if (metric._activeDecision) {
                const decision = metric._activeDecision;
                decision.lockAtMs = session.clockMs;
                decision.attack = {
                    raw: rawAttack,
                    cancelled,
                    sent: Math.max(0, rawAttack - cancelled),
                    lines: this.linesClearedLastLock
                };
                decision.ren = { before: before.ren, after: completedRen, bonusAttack: renBonus };
                if (decision.waitMs > 0 && rawAttack > 0) {
                    const receipt = metric.garbageReceivedEvents.find(event =>
                        event.atMs >= decision.atMs && event.atMs <= session.clockMs
                    );
                    const holdEvent = {
                        decisionAtMs: decision.atMs,
                        lockAtMs: session.clockMs,
                        plannedWaitMs: decision.waitMs,
                        actualHoldMs: session.clockMs - decision.atMs,
                        intent: decision.intent,
                        strategyEvent: decision.strategyEvent,
                        receivedDuringWait: Boolean(receipt),
                        receivedAtMs: receipt?.atMs ?? null,
                        receivedLines: receipt?.lines ?? 0,
                        attack: clone(decision.attack)
                    };
                    metric.holdFire.count++;
                    metric.holdFire.receivedDuringWait += receipt ? 1 : 0;
                    metric.holdFire.totalWaitMs += decision.waitMs;
                    addCount(metric.holdFire.byIntent, decision.strategyEvent || decision.intent);
                    metric.holdFire.events.push(holdEvent);
                    decision.holdFire = true;
                    decision.receivedDuringWait = Boolean(receipt);
                }
                if (Number.isInteger(decision.chargeReleaseEventIndex)) {
                    const releaseEvent = metric.charge.chargeRelease.events[decision.chargeReleaseEventIndex];
                    if (releaseEvent) releaseEvent.attack = clone(decision.attack);
                }
                metric._activeDecision = null;
            }
            return result;
        };

        const productionAddGarbage = player.addGarbage.bind(player);
        player.addGarbage = function instrumentedAddGarbage(lines) {
            const normalized = Math.max(0, Math.floor(Number(lines) || 0));
            metric.attacks.received += normalized;
            if (normalized > 0) metric.garbageReceivedEvents.push({ atMs: session.clockMs, lines: normalized });
            if (this.opponent?.__leagueMetric) {
                this.opponent.__leagueMetric.attacks.delivered += normalized;
            }
            return productionAddGarbage(lines);
        };

        const productionRiseGarbage = player.riseGarbage.bind(player);
        player.riseGarbage = function deterministicGarbageRise(...args) {
            const productionRandom = Math.random;
            Math.random = metric._garbageRandom;
            try {
                const lines = this.pendingGarbage;
                const result = productionRiseGarbage(...args);
                if (lines > 0 && this.pendingGarbage === 0) {
                    metric.garbageRiseEvents.push({ atMs: session.clockMs, lines });
                }
                return result;
            } finally {
                Math.random = productionRandom;
            }
        };

        if (headless) player.draw = () => {};

        const productionRequestAiMove = player.requestAiMove.bind(player);
        player.requestAiMove = function instrumentedRequestAiMove(...args) {
            const result = productionRequestAiMove(...args);
            const requestId = this.aiRequestId;
            const requestedRealMs = nativePerformanceNow();
            metric._requestRealMs.set(requestId, requestedRealMs);
            session.pendingWorkerMoves.set(`${this.id}:${requestId}`, {
                player: this,
                metric,
                requestId,
                readyAtMs: session.clockMs + session.settings.aiThinkTime,
                requestedRealMs,
                result: null
            });
            if (session.forceNoLegalMoveSeat === `p${this.id}` && !session.forcedNoLegalMoveUsed) {
                const pending = session.pendingWorkerMoves.get(`${this.id}:${requestId}`);
                pending.result = { data: { type: 'move', requestId } };
                pending.respondedRealMs = requestedRealMs;
                pending.syntheticNoLegalMove = true;
                session.forcedNoLegalMoveUsed = true;
            }
            return result;
        };

        const worker = player.aiWorker;
        if (!worker) return;
        const productionOnMessage = worker.onmessage;
        player.__leagueProductionOnMessage = productionOnMessage;
        worker.onmessage = event => {
            const data = event.data || {};
            if (data.type === 'nodeCount') {
                const count = Number(data.count);
                if (Number.isFinite(count)) {
                    metric.nodeCount.samples++;
                    metric.nodeCount.last = count;
                    metric.nodeCount.max = metric.nodeCount.max === null ? count : Math.max(metric.nodeCount.max, count);
                }
                return;
            }
            if (data.type === 'debug') return;
            if (data.type === 'error') {
                metric.workerErrors.push(String(data.message || 'Unknown worker error'));
                productionOnMessage.call(worker, event);
                return;
            }

            // A typed terminal answer is still the result of the pending
            // decision. Queue it on the same logical readiness boundary as a
            // normal move so terminal handling stays deterministic.
            const isMove = data.type === 'move' || data.type === 'noLegalMove' || Boolean(data.piece);
            if (!isMove) {
                productionOnMessage.call(worker, event);
                return;
            }

            const requestId = data.requestId ?? player.aiRequestId;
            const key = `${player.id}:${requestId}`;
            const pending = session.pendingWorkerMoves.get(key);
            if (!pending) {
                productionOnMessage.call(worker, event);
                return;
            }
            if (pending.syntheticNoLegalMove) return;
            pending.result = { data: clone(data) };
            pending.respondedRealMs = nativePerformanceNow();
        };
    }

    function finalPlayerMetric(player, metric) {
        finalizeChargeStreak(metric, activeSession?.clockMs ?? 0);
        const charge = clone(metric.charge);
        charge.consecutive.meanLength = charge.consecutive.sequences
            ? Number((charge.consecutive.totalLength / charge.consecutive.sequences).toFixed(3))
            : 0;
        const result = {
            seat: metric.seat,
            model: metric.model,
            terminal: {
                gameOver: Boolean(player.gameOver),
                gameClear: Boolean(player.gameClear)
            },
            pieces: player.pieceCount,
            locks: metric.locks,
            linesCleared: metric.linesCleared,
            attacks: clone(metric.attacks),
            intents: clone(metric.intents),
            strategyEvents: clone(metric.strategyEvents),
            policyOverrides: clone(metric.policyOverrides),
            moves: clone(metric.moves),
            waits: clone(metric.waits),
            holdFire: clone(metric.holdFire),
            garbageReceivedEvents: clone(metric.garbageReceivedEvents),
            garbageRiseEvents: clone(metric.garbageRiseEvents),
            ren: clone(metric.ren),
            charge,
            noLegalMove: clone(metric.noLegalMove),
            workerErrors: [...metric.workerErrors],
            nodeCount: clone(metric.nodeCount),
            inference: {
                workerResponse: summarizeSamples(metric._workerResponseMs),
                reportedSearch: summarizeSamples(metric._reportedSearchMs)
            },
            stats: clone(player.stats),
            finalState: {
                pendingGarbage: player.pendingGarbage,
                queuedGarbage: player.garbageQueue.reduce((sum, packet) => sum + packet.lines, 0),
                ren: player.ren,
                isB2B: player.isB2B,
                maxHeight: maximumBoardHeight(player.board)
            }
        };
        return result;
    }

    function maximumBoardHeight(board) {
        for (let row = 0; row < board.length; row++) {
            if (board[row].some(cell => cell !== null)) return board.length - row;
        }
        return 0;
    }

    function installVirtualClock(session) {
        const hadOwnNow = Object.prototype.hasOwnProperty.call(performance, 'now');
        const ownNowDescriptor = hadOwnNow ? Object.getOwnPropertyDescriptor(performance, 'now') : null;
        const virtualBaseMs = nativePerformanceNow();
        session.virtualBaseMs = virtualBaseMs;
        session.nextTimerId = 1;
        session.virtualTimers = new Map();

        Object.defineProperty(performance, 'now', {
            configurable: true,
            value: () => virtualBaseMs + session.clockMs
        });

        window.setTimeout = (callback, delay = 0, ...args) => {
            const timerId = session.nextTimerId++;
            session.virtualTimers.set(timerId, {
                id: timerId,
                dueMs: session.clockMs + Math.max(0, Number(delay) || 0),
                callback,
                args
            });
            return timerId;
        };
        window.clearTimeout = timerId => {
            if (!session.virtualTimers.delete(timerId)) {
                nativeClearTimeout.call(window, timerId);
            }
        };

        return () => {
            window.setTimeout = nativeSetTimeout;
            window.clearTimeout = nativeClearTimeout;
            session.virtualTimers.clear();
            if (hadOwnNow) {
                Object.defineProperty(performance, 'now', ownNowDescriptor);
            } else {
                delete performance.now;
            }
        };
    }

    async function drainVirtualTimers(session) {
        for (let guard = 0; guard < 10000; guard++) {
            const due = [...session.virtualTimers.values()]
                .filter(timer => timer.dueMs <= session.clockMs)
                .sort((left, right) => left.dueMs - right.dueMs || left.id - right.id);
            if (!due.length) {
                await Promise.resolve();
                const newDue = [...session.virtualTimers.values()].some(timer => timer.dueMs <= session.clockMs);
                if (!newDue) return;
                continue;
            }
            for (const timer of due) {
                if (!session.virtualTimers.delete(timer.id)) continue;
                if (typeof timer.callback === 'function') timer.callback(...timer.args);
            }
            await Promise.resolve();
            await Promise.resolve();
        }
        throw new Error('Virtual timer runaway detected');
    }

    function cleanupStaleWorkerRequests(session) {
        for (const [key, pending] of session.pendingWorkerMoves) {
            const player = pending.player;
            if (player.gameOver || player.gameClear || player.aiRequestId !== pending.requestId || !player.isAiThinking) {
                session.pendingWorkerMoves.delete(key);
                pending.metric._requestRealMs.delete(pending.requestId);
            }
        }
    }

    function hasBlockingWorkerRequest(session) {
        for (const pending of session.pendingWorkerMoves.values()) {
            if (pending.readyAtMs <= session.clockMs && !pending.result) return true;
        }
        return false;
    }

    function dispatchReadyWorkerMoves(session) {
        const ready = [...session.pendingWorkerMoves.entries()]
            .filter(([, pending]) => pending.readyAtMs <= session.clockMs && pending.result)
            .sort((left, right) => left[1].readyAtMs - right[1].readyAtMs || left[1].player.id.localeCompare(right[1].player.id));

        for (const [key, pending] of ready) {
            session.pendingWorkerMoves.delete(key);
            const { player, metric, requestId } = pending;
            const data = pending.result.data;
            const requestedRealMs = metric._requestRealMs.get(requestId) ?? pending.requestedRealMs;
            metric._requestRealMs.delete(requestId);
            const responseMs = Math.max(0, (pending.respondedRealMs ?? nativePerformanceNow()) - requestedRealMs);
            metric._workerResponseMs.push(responseMs);

            if (Number.isFinite(Number(data.searchElapsedMs))) {
                metric._reportedSearchMs.push(Math.max(0, Number(data.searchElapsedMs)));
            }
            if (data.intent) addCount(metric.intents, data.intent);
            if (data.strategyEvent) addCount(metric.strategyEvents, data.strategyEvent);
            if (data.policyOverride) addCount(metric.policyOverrides, data.policyOverride);
            const waitMs = Math.max(0, Number(data.waitMs) || 0);
            if (waitMs > 0) {
                metric.waits.count++;
                metric.waits.totalMs += waitMs;
                metric.waits.maxMs = Math.max(metric.waits.maxMs, waitMs);
            }

            const decision = {
                index: metric.moves.length,
                requestId,
                requestedAtMs: pending.readyAtMs - session.settings.aiThinkTime,
                atMs: session.clockMs,
                piece: data.piece || null,
                x: Number.isFinite(Number(data.x)) ? Number(data.x) : null,
                y: Number.isFinite(Number(data.y)) ? Number(data.y) : null,
                rotation: Number.isFinite(Number(data.rotation)) ? Number(data.rotation) : null,
                intent: data.intent || null,
                strategyEvent: data.strategyEvent || null,
                policyOverride: data.policyOverride || null,
                waitMs,
                searchElapsedMs: Number.isFinite(Number(data.searchElapsedMs)) ? Number(data.searchElapsedMs) : null,
                noLegalMove: !data.piece,
                chargeRelease: false,
                lockAtMs: null,
                attack: null
            };
            metric.moves.push(decision);
            observeIntent(metric, decision);
            if (data.piece) metric._activeDecision = decision;

            player.__leagueProductionOnMessage.call(player.aiWorker, { data });
            if (!data.piece) {
                metric.noLegalMove.count++;
                if (metric.noLegalMove.firstAtMs === null) metric.noLegalMove.firstAtMs = session.clockMs;
                player.gameOver = true;
                player.isAiThinking = false;
            }
        }
    }

    function nextVirtualEventMs(session, nextTickMs, maximumDurationMs) {
        let next = Math.min(nextTickMs, maximumDurationMs);
        for (const timer of session.virtualTimers.values()) next = Math.min(next, timer.dueMs);
        for (const pending of session.pendingWorkerMoves.values()) next = Math.min(next, pending.readyAtMs);
        return Math.max(session.clockMs, next);
    }

    function runProductionTick(session, dtMs) {
        players = session.players;
        session.allowGarbageFlush = true;
        try {
            productionFlushGarbage();
            session.players.forEach(player => player.update(dtMs));
            productionFlushGarbage();
            if (!session.headless && ctx && mainCanvas) {
                ctx.clearRect(0, 0, mainCanvas.width, mainCanvas.height);
                session.players.forEach(player => player.draw());
            }
        } finally {
            session.allowGarbageFlush = false;
            players = [];
        }
    }

    function terminalResult(session, options, wallStartedMs) {
        const [p1, p2] = session.players;
        const p1Dead = p1.gameOver || (p2.gameClear && !p1.gameClear);
        const p2Dead = p2.gameOver || (p1.gameClear && !p2.gameClear);
        const pieceLimit = p1.pieceCount >= options.maxPiecesPerPlayer || p2.pieceCount >= options.maxPiecesPerPlayer;
        const wallExpired = nativePerformanceNow() - wallStartedMs >= options.wallTimeoutMs;
        const timeExpired = session.clockMs >= options.maxDurationMs;

        if (!p1Dead && !p2Dead && !pieceLimit && !wallExpired && !timeExpired) return null;

        let winnerSeat = null;
        let loserSeat = null;
        let reason;
        if (p1Dead && !p2Dead) {
            winnerSeat = 'p2';
            loserSeat = 'p1';
            reason = session.metrics.p1.noLegalMove.count ? 'noLegalMove' : 'topOut';
        } else if (p2Dead && !p1Dead) {
            winnerSeat = 'p1';
            loserSeat = 'p2';
            reason = session.metrics.p2.noLegalMove.count ? 'noLegalMove' : 'topOut';
        } else if (p1Dead && p2Dead) {
            reason = session.metrics.p1.noLegalMove.count || session.metrics.p2.noLegalMove.count
                ? 'simultaneousNoLegalMove'
                : 'simultaneousTopOut';
        } else if (wallExpired) {
            reason = 'infrastructureWallTimeout';
        } else if (pieceLimit) {
            reason = 'pieceLimit';
        } else {
            reason = 'timeLimit';
        }
        return { winnerSeat, loserSeat, reason };
    }

    async function runSingleGame(plan, options) {
        resetGarbageDeliveryBatch();
        analysisData = [];
        gameHistoryLog = [];
        gameMode = '2P';
        gameState = 'LEAGUE_PREP';
        players = [];

        const requiredSequenceLength = options.maxPiecesPerPlayer + gameSettings.maxNext + 32;
        const sequence = createPieceSequence(deriveSeed(plan.seed, 'shared-pieces'), requiredSequenceLength);
        editorData.p1.board = clone(options.initialBoards.p1);
        editorData.p2.board = clone(options.initialBoards.p2);
        editorData.p1.nextQueue = [...sequence];
        editorData.p2.nextQueue = [...sequence];
        editorData.p1.hold = null;
        editorData.p2.hold = null;
        editorData.rule.description = '';
        editorData.rule.code = '';

        const session = {
            clockMs: 0,
            headless: options.headless,
            settings: options.settings,
            allowGarbageFlush: false,
            pendingWorkerMoves: new Map(),
            forceNoLegalMoveSeat: options.forceNoLegalMoveSeat,
            forcedNoLegalMoveUsed: false,
            players: [],
            metrics: {}
        };
        activeSession = session;
        const restoreClock = installVirtualClock(session);
        const wallStartedMs = nativePerformanceNow();

        try {
            gameStartTime = performance.now();
            const p1 = new Player('1', 0, keyBindings.p1, 0, true, plan.p1Model);
            const p2 = new Player('2', PLAYER_CANVAS_WIDTH, keyBindings.p2, 1, true, plan.p2Model);
            p1.opponent = p2;
            p2.opponent = p1;
            session.players = [p1, p2];
            session.metrics.p1 = makePlayerMetric('p1', plan.p1Model, deriveSeed(plan.seed, 'garbage-p1'), session.settings.chargeReleaseWindowMs);
            session.metrics.p2 = makePlayerMetric('p2', plan.p2Model, deriveSeed(plan.seed, 'garbage-p2'), session.settings.chargeReleaseWindowMs);
            patchPlayerForLeague(p1, session.metrics.p1, session, options.headless);
            patchPlayerForLeague(p2, session.metrics.p2, session, options.headless);

            gameState = 'PLAYING';
            runProductionTick(session, 0);
            let nextTickMs = options.fixedTickMs;
            let terminal = null;

            while (!terminal) {
                cleanupStaleWorkerRequests(session);
                dispatchReadyWorkerMoves(session);
                await drainVirtualTimers(session);
                cleanupStaleWorkerRequests(session);
                terminal = terminalResult(session, options, wallStartedMs);
                if (terminal) break;

                const targetMs = nextVirtualEventMs(session, nextTickMs, options.maxDurationMs);
                session.clockMs = targetMs;

                if (hasBlockingWorkerRequest(session)) {
                    if (nativePerformanceNow() - wallStartedMs >= options.wallTimeoutMs) {
                        terminal = terminalResult(session, options, wallStartedMs);
                        break;
                    }
                    await new Promise(resolve => nativeSetTimeout.call(window, resolve, 1));
                    continue;
                }

                dispatchReadyWorkerMoves(session);
                await drainVirtualTimers(session);
                if (nextTickMs <= session.clockMs) {
                    runProductionTick(session, options.fixedTickMs);
                    nextTickMs += options.fixedTickMs;
                }
                terminal = terminalResult(session, options, wallStartedMs);
            }

            gameState = 'LEAGUE_FINISHED';
            players = [];
            const p1Result = finalPlayerMetric(p1, session.metrics.p1);
            const p2Result = finalPlayerMetric(p2, session.metrics.p2);
            const winnerModel = terminal.winnerSeat === 'p1'
                ? plan.p1Model
                : terminal.winnerSeat === 'p2'
                    ? plan.p2Model
                    : null;

            return {
                pairIndex: plan.pairIndex,
                leg: plan.leg,
                seed: plan.seed,
                sideSwap: plan.leg === 2,
                seats: { p1: plan.p1Model, p2: plan.p2Model },
                winnerSeat: terminal.winnerSeat,
                winnerModel,
                loserSeat: terminal.loserSeat,
                reason: terminal.reason,
                durationMs: session.clockMs,
                wallDurationMs: Number((nativePerformanceNow() - wallStartedMs).toFixed(3)),
                players: { p1: p1Result, p2: p2Result }
            };
        } finally {
            gameState = 'LEAGUE_CLEANUP';
            players = [];
            for (const player of session.players) {
                try { player.aiWorker?.postMessage({ type: 'stop' }); } catch (_) {}
                try { player.aiWorker?.terminate(); } catch (_) {}
                player.isAiThinking = false;
            }
            resetGarbageDeliveryBatch();
            restoreClock();
            activeSession = null;
        }
    }

    function normalizedOptions(options = {}) {
        const modelA = normalizeAiModelId(options.modelA || 'kasane-strategy');
        const modelB = normalizeAiModelId(options.modelB || 'cold-clear');
        const baseSeed = hashSeed(options.seed ?? 20260830);
        const pairs = clampInteger(options.pairs, 1, 10000, 1);
        const aiThinkTime = clampInteger(options.aiThinkTime, 0, 5000, 180);
        const aiNodeLimit = clampInteger(options.aiNodeLimit, 1000, 2000000, 120000);
        const settings = {
            aiMoveDelay: 50,
            aiSdfDelay: 50,
            lineClearDelay: 750,
            garbageGrace: 1000,
            chargeReleaseWindowMs: 750,
            perfectClearAttack: 10,
            garbageRandomness: 0.3,
            aiThinkTime,
            aiNodeLimit,
            maxNext: 8,
            spawnDelay: 0,
            gravity: 9999999,
            lockDelay: 9999999
        };
        const initialBoards = scenarioBoards(options.scenario, options.initialBoards);
        return {
            modelA,
            modelB,
            baseSeed,
            pairs,
            headless: options.headless !== false,
            fixedTickMs: clampInteger(options.fixedTickMs, 1, 50, 10),
            maxDurationMs: clampInteger(options.maxDurationMs, 100, 3600000, 120000),
            wallTimeoutMs: clampInteger(options.wallTimeoutMs, 1000, 3600000, 300000),
            maxPiecesPerPlayer: clampInteger(options.maxPiecesPerPlayer, 1, 10000, 600),
            forceNoLegalMoveSeat: options.forceNoLegalMoveSeat === 'p1' || options.forceNoLegalMoveSeat === 'p2'
                ? options.forceNoLegalMoveSeat
                : null,
            scenario: initialBoards.name,
            initialBoards,
            settings,
            onProgress: typeof options.onProgress === 'function' ? options.onProgress : null
        };
    }

    function applyLeagueSettings(options) {
        Object.assign(gameSettings, {
            ...options.settings,
            aiModels: { p1: options.modelA, p2: options.modelB },
            pieceForPieceMode: false,
            banPC: false,
            showEffects: !options.headless,
            showTimer: false,
            touchControlsEnabled: false,
            debugEnabled: false
        });
    }

    function saveEnvironment() {
        return {
            gameSettings: clone(gameSettings),
            editorP1: clone(editorData.p1),
            editorP2: clone(editorData.p2),
            editorRule: clone(editorData.rule),
            players,
            gameMode,
            gameState,
            gameStartTime,
            analysisData,
            gameHistoryLog,
            lastTime
        };
    }

    function restoreEnvironment(saved) {
        for (const key of Object.keys(gameSettings)) delete gameSettings[key];
        Object.assign(gameSettings, saved.gameSettings);
        editorData.p1 = saved.editorP1;
        editorData.p2 = saved.editorP2;
        editorData.rule = saved.editorRule;
        players = saved.players;
        gameMode = saved.gameMode;
        gameState = saved.gameState;
        gameStartTime = saved.gameStartTime;
        analysisData = saved.analysisData;
        gameHistoryLog = saved.gameHistoryLog;
        lastTime = saved.lastTime;
    }

    function aggregateGames(games, modelA, modelB) {
        const byModel = {};
        for (const model of [modelA, modelB]) {
            if (!byModel[model]) {
                byModel[model] = {
                    games: 0,
                    wins: 0,
                    losses: 0,
                    draws: 0,
                    noLegalMoveLosses: 0,
                    pieces: 0,
                    linesCleared: 0,
                    attacks: { raw: 0, cancelled: 0, sent: 0, delivered: 0, received: 0 },
                    intents: {},
                    strategyEvents: {},
                    policyOverrides: {},
                    holdFire: { count: 0, receivedDuringWait: 0, totalWaitMs: 0, byIntent: {} },
                    charge: {
                        moves: 0,
                        consecutive: { sequences: 0, totalLength: 0, maxLength: 0, histogram: {} },
                        firstAttackAfterCharge: { count: 0, byIntent: {} },
                        chargeRelease: { count: 0, byIntent: {} }
                    },
                    ren: { max: -1, starts: 0, continuations: 0, bonusAttack: 0 },
                    _workerResponseMs: [],
                    _reportedSearchMs: []
                };
            }
        }

        for (const game of games) {
            for (const seat of ['p1', 'p2']) {
                const player = game.players[seat];
                const aggregate = byModel[player.model];
                aggregate.games++;
                if (game.winnerSeat === seat) aggregate.wins++;
                else if (game.winnerSeat === null) aggregate.draws++;
                else aggregate.losses++;
                if (game.loserSeat === seat && game.reason === 'noLegalMove') aggregate.noLegalMoveLosses++;
                aggregate.pieces += player.pieces;
                aggregate.linesCleared += player.linesCleared;
                for (const key of Object.keys(aggregate.attacks)) aggregate.attacks[key] += player.attacks[key];
                for (const [intent, count] of Object.entries(player.intents)) addCount(aggregate.intents, intent, count);
                for (const [event, count] of Object.entries(player.strategyEvents || {})) {
                    addCount(aggregate.strategyEvents, event, count);
                }
                for (const [policyOverride, count] of Object.entries(player.policyOverrides || {})) {
                    addCount(aggregate.policyOverrides, policyOverride, count);
                }
                aggregate.holdFire.count += player.holdFire?.count || 0;
                aggregate.holdFire.receivedDuringWait += player.holdFire?.receivedDuringWait || 0;
                aggregate.holdFire.totalWaitMs += player.holdFire?.totalWaitMs || 0;
                for (const [intent, count] of Object.entries(player.holdFire?.byIntent || {})) {
                    addCount(aggregate.holdFire.byIntent, intent, count);
                }
                aggregate.charge.moves += player.charge.moves;
                aggregate.charge.consecutive.sequences += player.charge.consecutive.sequences;
                aggregate.charge.consecutive.totalLength += player.charge.consecutive.totalLength;
                aggregate.charge.consecutive.maxLength = Math.max(
                    aggregate.charge.consecutive.maxLength,
                    player.charge.consecutive.maxLength
                );
                for (const [length, count] of Object.entries(player.charge.consecutive.histogram)) {
                    addCount(aggregate.charge.consecutive.histogram, length, count);
                }
                aggregate.charge.firstAttackAfterCharge.count += player.charge.firstAttackAfterCharge.count;
                for (const [intent, count] of Object.entries(player.charge.firstAttackAfterCharge.byIntent)) {
                    addCount(aggregate.charge.firstAttackAfterCharge.byIntent, intent, count);
                }
                aggregate.charge.chargeRelease.count += player.charge.chargeRelease.count;
                for (const [intent, count] of Object.entries(player.charge.chargeRelease.byIntent)) {
                    addCount(aggregate.charge.chargeRelease.byIntent, intent, count);
                }
                aggregate.ren.max = Math.max(aggregate.ren.max, player.ren?.max ?? -1);
                aggregate.ren.starts += player.ren?.starts || 0;
                aggregate.ren.continuations += player.ren?.continuations || 0;
                aggregate.ren.bonusAttack += player.ren?.bonusAttack || 0;
                if (player.inference.workerResponse.samples) {
                    aggregate._workerResponseMs.push(player.inference.workerResponse.meanMs);
                }
                if (player.inference.reportedSearch.samples) {
                    aggregate._reportedSearchMs.push(player.inference.reportedSearch.meanMs);
                }
            }
        }

        for (const aggregate of Object.values(byModel)) {
            aggregate.winRate = aggregate.games ? Number((aggregate.wins / aggregate.games).toFixed(6)) : null;
            aggregate.averagePieces = aggregate.games ? Number((aggregate.pieces / aggregate.games).toFixed(3)) : null;
            aggregate.charge.consecutive.meanLength = aggregate.charge.consecutive.sequences
                ? Number((aggregate.charge.consecutive.totalLength / aggregate.charge.consecutive.sequences).toFixed(3))
                : 0;
            aggregate.inference = {
                workerResponsePerGame: summarizeSamples(aggregate._workerResponseMs),
                reportedSearchPerGame: summarizeSamples(aggregate._reportedSearchMs)
            };
            delete aggregate._workerResponseMs;
            delete aggregate._reportedSearchMs;
        }

        return {
            games: games.length,
            decisiveGames: games.filter(game => game.winnerSeat !== null).length,
            draws: games.filter(game => game.winnerSeat === null).length,
            byModel
        };
    }

    async function pauseRegularGameLoop() {
        window.requestAnimationFrame = () => -1;
        await new Promise(resolve => nativeSetTimeout.call(window, resolve, 25));
    }

    function resumeRegularGameLoop() {
        window.requestAnimationFrame = nativeRequestAnimationFrame;
        lastTime = nativePerformanceNow();
        nativeRequestAnimationFrame.call(window, gameLoop);
    }

    async function run(options = {}) {
        if (leagueRunning) throw new Error('A simulator league is already running');
        if (gameState === 'PLAYING' && players.length) {
            throw new Error('End the visible match before starting a league run');
        }

        const normalized = normalizedOptions(options);
        const saved = saveEnvironment();
        const games = [];
        leagueRunning = true;
        await pauseRegularGameLoop();
        applyLeagueSettings(normalized);

        try {
            for (let pairIndex = 0; pairIndex < normalized.pairs; pairIndex++) {
                const pairSeed = deriveSeed(normalized.baseSeed, `pair-${pairIndex}`);
                const plans = [
                    { pairIndex, leg: 1, seed: pairSeed, p1Model: normalized.modelA, p2Model: normalized.modelB },
                    { pairIndex, leg: 2, seed: pairSeed, p1Model: normalized.modelB, p2Model: normalized.modelA }
                ];
                for (const plan of plans) {
                    const game = await runSingleGame(plan, normalized);
                    games.push(game);
                    normalized.onProgress?.({
                        completedGames: games.length,
                        totalGames: normalized.pairs * 2,
                        latest: game
                    });
                    await new Promise(resolve => nativeSetTimeout.call(window, resolve, 0));
                }
            }

            return {
                schema: SCHEMA,
                createdAt: new Date().toISOString(),
                engine: {
                    gameLogic: 'simulator/app/player-engine.js::Player',
                    aiWorkers: 'production',
                    clock: 'deterministic-fixed-tick',
                    fixedTickMs: normalized.fixedTickMs,
                    rendering: normalized.headless ? 'omitted' : 'production-Player.draw'
                },
                pairing: {
                    baseSeed: normalized.baseSeed,
                    generator: 'FNV-1a seed derivation + Mulberry32 + 7-bag',
                    pairs: normalized.pairs,
                    games: normalized.pairs * 2,
                    sameSeedWithinPair: true,
                    samePieceSequenceForBothSeats: true,
                    sideSwap: true,
                    modelA: normalized.modelA,
                    modelB: normalized.modelB
                },
                scenario: normalized.scenario,
                testHooks: {
                    forcedNoLegalMoveSeat: normalized.forceNoLegalMoveSeat
                },
                settings: clone(normalized.settings),
                limits: {
                    maxDurationMs: normalized.maxDurationMs,
                    wallTimeoutMs: normalized.wallTimeoutMs,
                    maxPiecesPerPlayer: normalized.maxPiecesPerPlayer
                },
                aggregate: aggregateGames(games, normalized.modelA, normalized.modelB),
                games
            };
        } finally {
            activeSession = null;
            restoreEnvironment(saved);
            leagueRunning = false;
            resumeRegularGameLoop();
        }
    }

    function canonicalGame(game) {
        const deterministicMoves = moves => moves
            .filter(move => move.lockAtMs !== null || move.noLegalMove)
            .map(move => {
                const { searchElapsedMs, ...deterministic } = move;
                return deterministic;
            });
        const playerCore = player => ({
            model: player.model,
            terminal: player.terminal,
            pieces: player.pieces,
            locks: player.locks,
            linesCleared: player.linesCleared,
            attacks: player.attacks,
            intents: player.intents,
            moves: deterministicMoves(player.moves),
            garbageReceivedEvents: player.garbageReceivedEvents,
            holdFire: player.holdFire,
            charge: player.charge,
            noLegalMove: player.noLegalMove,
            stats: player.stats,
            finalState: player.finalState
        });
        return {
            pairIndex: game.pairIndex,
            leg: game.leg,
            seed: game.seed,
            seats: game.seats,
            winnerSeat: game.winnerSeat,
            winnerModel: game.winnerModel,
            loserSeat: game.loserSeat,
            reason: game.reason,
            durationMs: game.durationMs,
            players: { p1: playerCore(game.players.p1), p2: playerCore(game.players.p2) }
        };
    }

    async function verifyRenderConsistency(options = {}) {
        const base = { ...options, headless: false };
        const rendered = await run(base);
        const headless = await run({ ...options, headless: true });
        const mismatches = [];
        const length = Math.max(rendered.games.length, headless.games.length);
        for (let index = 0; index < length; index++) {
            const renderedGame = rendered.games[index] ? canonicalGame(rendered.games[index]) : null;
            const headlessGame = headless.games[index] ? canonicalGame(headless.games[index]) : null;
            if (JSON.stringify(renderedGame) !== JSON.stringify(headlessGame)) {
                mismatches.push({ index, rendered: renderedGame, headless: headlessGame });
            }
        }
        return {
            schema: 'tetris-simulator-league-render-consistency.v1',
            createdAt: new Date().toISOString(),
            passed: mismatches.length === 0,
            comparedGames: length,
            ignoredFields: ['wallDurationMs', 'inference', 'nodeCount', 'moves[].searchElapsedMs', 'unfinished moves at time/piece limit'],
            mismatches,
            rendered,
            headless
        };
    }

    function downloadJson(result, filename = `tetris-league-${Date.now()}.json`) {
        const blob = new Blob([JSON.stringify(result, null, 2)], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const anchor = document.createElement('a');
        anchor.href = url;
        anchor.download = filename;
        anchor.click();
        nativeSetTimeout.call(window, () => URL.revokeObjectURL(url), 0);
    }

    function ensureOutputElement() {
        let output = document.getElementById('league-output');
        if (output) return output;
        output = document.createElement('pre');
        output.id = 'league-output';
        output.style.cssText = 'position:fixed;inset:12px;z-index:20000;overflow:auto;padding:12px;background:#090b10;color:#d7f7df;border:1px solid #4a6;font:12px/1.45 monospace;white-space:pre-wrap;';
        document.body.appendChild(output);
        return output;
    }

    function queryInteger(params, name, fallback) {
        return params.has(name) ? Number(params.get(name)) : fallback;
    }

    async function autoRunFromLocation() {
        const params = new URLSearchParams(location.search);
        if (params.get('league') !== '1' && params.get('leagueVerify') !== '1') return null;
        const output = ensureOutputElement();
        const options = {
            modelA: params.get('leagueModelA') || 'kasane-strategy',
            modelB: params.get('leagueModelB') || 'cold-clear',
            seed: params.get('leagueSeed') || '20260830',
            pairs: queryInteger(params, 'leaguePairs', 1),
            maxDurationMs: queryInteger(params, 'leagueMaxDurationMs', 120000),
            wallTimeoutMs: queryInteger(params, 'leagueWallTimeoutMs', 300000),
            maxPiecesPerPlayer: queryInteger(params, 'leagueMaxPieces', 600),
            fixedTickMs: queryInteger(params, 'leagueTickMs', 10),
            aiThinkTime: queryInteger(params, 'leagueThinkMs', 180),
            aiNodeLimit: queryInteger(params, 'leagueNodeLimit', 120000),
            headless: params.get('leagueHeadless') !== '0',
            scenario: params.get('leagueScenario') || 'empty',
            forceNoLegalMoveSeat: params.get('leagueForceNoLegalMove'),
            onProgress: progress => {
                window.__tetrisLeagueProgress = progress;
                output.textContent = `League running ${progress.completedGames}/${progress.totalGames}\nLast: ${progress.latest.seats.p1} vs ${progress.latest.seats.p2} -> ${progress.latest.winnerModel || 'draw'} (${progress.latest.reason})`;
            }
        };
        window.__tetrisLeagueStatus = 'running';
        try {
            const result = params.get('leagueVerify') === '1'
                ? await verifyRenderConsistency(options)
                : await run(options);
            window.__tetrisLeagueResult = result;
            window.__tetrisLeagueStatus = 'complete';
            output.textContent = JSON.stringify(result, null, 2);
            document.dispatchEvent(new CustomEvent('tetris-league-complete', { detail: result }));
            return result;
        } catch (error) {
            window.__tetrisLeagueStatus = 'error';
            window.__tetrisLeagueError = String(error?.stack || error);
            output.textContent = window.__tetrisLeagueError;
            throw error;
        }
    }

    window.TetrisLeague = Object.freeze({
        schema: SCHEMA,
        run,
        verifyRenderConsistency,
        autoRunFromLocation,
        downloadJson
    });
})();
