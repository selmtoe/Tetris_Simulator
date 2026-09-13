/* PC, Cold Clear and REN guides. The existing P shortcut remains PC search. */

(function () {
    'use strict';

    const SEARCH_TIMEOUT_MS = 4000;
    const BOARD_COLUMNS = 10;
    const PIECES = new Set(['I', 'O', 'T', 'L', 'J', 'S', 'Z']);

    let worker = null;
    let activeRequest = null;
    let timeoutId = null;
    let requestId = 0;
    let guidePlan = null;

    function button() {
        return document.getElementById('pcSearchBtn');
    }

    function setSearching(searching) {
        const element = document.getElementById('searchMenuBtn');
        if (!element) return;
        element.setAttribute('aria-busy', String(searching));
    }

    function snapshotFor(player, kind = 'pc') {
        const nextQueue = [...player.nextQueue];
        if (kind === 'ren') nextQueue.push(...(player.knownCustomSequence || []).slice(player.fullMinoSequence.length));
        return {
            board: player.board.map(row => row.map(cell => cell == null ? null : 'X')),
            currentPiece: player.player.pieceType,
            nextQueue,
            holdPiece: player.holdPiece || null,
            canHold: player.canHold,
            holdDisabled: player.holdDisabled,
            ren: player.ren,
            isB2B: player.isB2B,
            thinkTimeMs: gameSettings.aiThinkTime || 50,
            nodeLimit: gameSettings.aiNodeLimit || 120000,
            weights: gameSettings.aiWeights
        };
    }

    function fingerprint(snapshot) {
        return [
            snapshot.board.map(row => row.map(cell => cell == null ? '0' : '1').join('')).join('/'),
            snapshot.currentPiece,
            snapshot.nextQueue.join(''),
            snapshot.holdPiece || '-',
            snapshot.canHold ? '1' : '0',
            snapshot.holdDisabled ? '1' : '0'
        ].join('|');
    }

    function getPlayableP1() {
        if (gameState !== 'PLAYING') return null;
        const player = players[0];
        if (!player || player.gameOver || player.gameClear || player.isClearingLine || player.isSpawning ||
            player.isExecutingSequence || !PIECES.has(player.player.pieceType)) {
            return null;
        }
        return player;
    }

    function clearTimeoutForRequest() {
        if (timeoutId !== null) {
            clearTimeout(timeoutId);
            timeoutId = null;
        }
    }

    function disposeWorker() {
        if (worker) {
            worker.terminate();
            worker = null;
        }
    }

    function resetActiveRequest() {
        clearTimeoutForRequest();
        activeRequest = null;
        setSearching(false);
    }

    function cancelActiveSearchFor(player) {
        if (!activeRequest || activeRequest.player !== player) return false;
        resetActiveRequest();
        disposeWorker();
        return true;
    }

    function toOccupancy(board) {
        if (!Array.isArray(board) || board.length === 0) return null;
        const result = [];
        for (const row of board) {
            if (!Array.isArray(row) || row.length !== BOARD_COLUMNS) return null;
            result.push(row.map(cell => cell == null ? 0 : 1));
        }
        return result;
    }

    function boardsEqual(left, right) {
        if (!left || !right || left.length !== right.length) return false;
        for (let y = 0; y < left.length; y++) {
            if (!left[y] || !right[y] || left[y].length !== right[y].length) return false;
            for (let x = 0; x < left[y].length; x++) {
                if (left[y][x] !== right[y][x]) return false;
            }
        }
        return true;
    }

    function isEmptyBoard(board) {
        return Array.isArray(board) && board.every(row => row.every(cell => cell === 0));
    }

    function normalizeCells(cells) {
        if (!Array.isArray(cells) || cells.length !== 4) return null;
        const unique = new Map();
        for (const cell of cells) {
            const x = Number(cell && cell.x);
            const y = Number(cell && cell.y);
            if (!Number.isInteger(x) || !Number.isInteger(y) || x < 0 || x >= BOARD_COLUMNS || y < 0) {
                return null;
            }
            unique.set(`${x},${y}`, { x, y });
        }
        return unique.size === 4 ? [...unique.values()] : null;
    }

    function normalizePlan(plan) {
        if (!Array.isArray(plan) || plan.length === 0) return null;
        const normalized = [];
        for (const step of plan) {
            const cells = normalizeCells(step && step.cells);
            if (!step || !PIECES.has(step.piece) || !cells) return null;
            normalized.push({ piece: step.piece, cells });
        }
        return normalized;
    }

    function cellsMatch(left, right) {
        const normalizedLeft = normalizeCells(left);
        const normalizedRight = normalizeCells(right);
        if (!normalizedLeft || !normalizedRight) return false;
        const rightKeys = new Set(normalizedRight.map(cell => `${cell.x},${cell.y}`));
        return normalizedLeft.every(cell => rightKeys.has(`${cell.x},${cell.y}`));
    }

    function lockedCellsFor(player) {
        if (!player || !player.player || !PIECES.has(player.player.pieceType) ||
            typeof player.getShape !== 'function') {
            return null;
        }
        const shape = player.getShape(player.player.pieceType, player.player.rotation);
        if (!Array.isArray(shape) || shape.length !== 4) return null;
        return shape.map(block => ({
            x: Math.floor(player.player.x + block[0]),
            y: Math.floor(player.player.y + block[1])
        }));
    }

    function boardAfterPlacement(board, cells) {
        if (!board || !cells) return null;
        const result = board.map(row => [...row]);
        for (const cell of cells) {
            if (!result[cell.y] || cell.x < 0 || cell.x >= BOARD_COLUMNS || result[cell.y][cell.x] !== 0) {
                return null;
            }
            result[cell.y][cell.x] = 1;
        }

        const remainingRows = result.filter(row => !row.every(cell => cell === 1));
        while (remainingRows.length < result.length) {
            remainingRows.unshift(Array(BOARD_COLUMNS).fill(0));
        }
        return remainingRows;
    }

    function stepAvailability(player, step) {
        if (!player || !step || !PIECES.has(step.piece)) return { available: false };
        if (player.player.pieceType === step.piece) return { available: true, needsHold: false };
        if (!player.canHold || player.holdDisabled) return { available: false };

        const holdPiece = player.holdPiece || null;
        const nextPiece = Array.isArray(player.nextQueue) ? player.nextQueue[0] : null;
        if (holdPiece === step.piece || (!holdPiece && nextPiece === step.piece)) {
            return { available: true, needsHold: true };
        }
        return { available: false };
    }

    function clearGuidePlan(player) {
        if (guidePlan && (!player || guidePlan.player === player)) {
            guidePlan = null;
        }
        if (player && typeof player.clearPcGuide === 'function') {
            player.clearPcGuide();
        }
    }

    function showCurrentStep(player) {
        const chain = guidePlan;
        if (!chain || chain.player !== player || chain.phase !== 'guiding') return false;
        if (!boardsEqual(toOccupancy(player.board), chain.expectedBoard)) {
            clearGuidePlan(player);
            return false;
        }

        const step = chain.plan[chain.index];
        const availability = stepAvailability(player, step);
        if (!step || !availability.available) {
            clearGuidePlan(player);
            return false;
        }

        player.setPcGuide(step.cells, step.piece);
        if (!player.pcGuide) {
            clearGuidePlan(player);
            return false;
        }

        return true;
    }

    function startGuidePlan(player, data) {
        const plan = normalizePlan(data.plan);
        const expectedBoard = toOccupancy(player.board);
        if (!plan || !expectedBoard) {
            clearGuidePlan(player);
            return false;
        }

        guidePlan = {
            player,
            plan,
            index: 0,
            expectedBoard,
            pendingBoard: null,
            phase: 'guiding',
            kind: data.kind || 'pc'
        };
        return showCurrentStep(player);
    }

    function ensureWorker() {
        if (worker) return worker;

        worker = new Worker('./simulator/workers/route-search-worker.js?v=search-v1');
        const ownedWorker = worker;
        worker.onmessage = event => {
            const data = event.data || {};
            if (!activeRequest || data.requestId !== activeRequest.id) return;

            const request = activeRequest;
            const currentPlayer = getPlayableP1();
            if (!currentPlayer || currentPlayer !== request.player ||
                fingerprint(snapshotFor(currentPlayer, request.kind)) !== request.fingerprint) {
                resetActiveRequest();
                disposeWorker();
                return;
            }
            if (data.type === 'progress') {
                if (data.plan?.length) request.partial = data;
                return;
            }
            resetActiveRequest();

            if (data.type === 'error') {
                console.error('PC finder worker error:', data.message);
                return;
            }

            if (data.status === 'found' && Array.isArray(data.plan) && data.plan.length > 0) {
                startGuidePlan(currentPlayer, data);
                return;
            }

            if (data.status === 'not_found') {
                currentPlayer.clearPcGuide();
                return;
            }

            if (data.status === 'unsupported') {
                currentPlayer.clearPcGuide();
                return;
            }

        };
        worker.onerror = error => {
            if (worker !== ownedWorker) { error.preventDefault(); return; }
            console.error('PC finder worker failed:', error);
            resetActiveRequest();
            disposeWorker();
        };
        return worker;
    }

    function isBoundKeyboardKey(key) {
        const binding = typeof keyBindings !== 'undefined' ? keyBindings?.p1?.pcSearch : null;
        return binding?.type === 'key' && binding.value === String(key).toLowerCase();
    }

    // Used by the Hub when the simulator iframe does not have keyboard focus.
    // Returning a boolean lets the parent leave unrelated shortcuts alone.
    function searchIfBoundKey(key) {
        if (!isBoundKeyboardKey(key)) return false;
        search();
        return true;
    }

    function search(kind = 'pc') {
        if (typeof kind !== 'string') kind = 'pc';
        if (activeRequest) {
            const previous = activeRequest.kind;
            const request = activeRequest;
            resetActiveRequest(); disposeWorker();
            if (previous === kind) {
                if (request.partial && getPlayableP1() === request.player && fingerprint(snapshotFor(request.player, kind)) === request.fingerprint) startGuidePlan(request.player, request.partial);
                return;
            }
        }

        const player = getPlayableP1();
        if (!player) {
            return;
        }

        clearGuidePlan(player);
        const snapshot = snapshotFor(player, kind);
        const id = ++requestId;
        activeRequest = { id, player, kind, fingerprint: fingerprint(snapshot) };
        setSearching(true);

        try {
            ensureWorker().postMessage({ type: 'search', kind, requestId: id, ...snapshot });
        } catch (error) {
            console.error('Unable to request PC search:', error);
            resetActiveRequest();
            disposeWorker();
            return;
        }

        if (kind === 'ren') return;
        timeoutId = window.setTimeout(() => {
            if (!activeRequest || activeRequest.id !== id) return;
            resetActiveRequest();
            disposeWorker();
        }, kind === 'ai' ? 15000 : SEARCH_TIMEOUT_MS);
    }

    // Called by Player immediately before it writes the locked mino to board.
    function onBeforeLock(player) {
        if (cancelActiveSearchFor(player)) {
            clearGuidePlan(player);
            return false;
        }

        const chain = guidePlan;
        if (!chain || chain.player !== player) {
            if (player && typeof player.clearPcGuide === 'function') player.clearPcGuide();
            return false;
        }
        if (chain.phase !== 'guiding' || !boardsEqual(toOccupancy(player.board), chain.expectedBoard)) {
            clearGuidePlan(player);
            return false;
        }

        const step = chain.plan[chain.index];
        if (!step || player.player.pieceType !== step.piece || !cellsMatch(lockedCellsFor(player), step.cells)) {
            clearGuidePlan(player);
            return false;
        }

        const pendingBoard = boardAfterPlacement(chain.expectedBoard, step.cells);
        if (!pendingBoard) {
            clearGuidePlan(player);
            return false;
        }

        chain.pendingBoard = pendingBoard;
        chain.phase = 'locking';
        player.clearPcGuide();
        return true;
    }

    // Called after Player.clearLines(), while the board is still the direct
    // result of the lock and before garbage/spawn processing can change it.
    function onAfterLock(player) {
        const chain = guidePlan;
        if (!chain || chain.player !== player || chain.phase !== 'locking') return;
        if (!boardsEqual(toOccupancy(player.board), chain.pendingBoard)) {
            clearGuidePlan(player);
            return;
        }

        chain.expectedBoard = chain.pendingBoard;
        chain.pendingBoard = null;
        chain.index++;
        if (chain.index >= chain.plan.length) {
            if (chain.kind === 'pc' && !isEmptyBoard(chain.expectedBoard)) {
                clearGuidePlan(player);
                return;
            }
            clearGuidePlan(player);
            return;
        }

        chain.phase = 'waitingForSpawn';
    }

    // Called after Player.spawnNewPiece(). This is intentionally after line
    // clear and spawn delays so the next guide only appears for a live mino.
    function onSpawn(player) {
        const chain = guidePlan;
        if (!chain || chain.player !== player || chain.phase !== 'waitingForSpawn') return;
        if (player.gameOver || player.gameClear || gameState !== 'PLAYING') {
            clearGuidePlan(player);
            return;
        }
        if (!boardsEqual(toOccupancy(player.board), chain.expectedBoard)) {
            clearGuidePlan(player);
            return;
        }

        chain.phase = 'guiding';
        showCurrentStep(player);
    }

    // A HOLD is valid only when it makes the guided mino the active mino.
    function onHold(player) {
        const chain = guidePlan;
        if (!chain || chain.player !== player || chain.phase !== 'guiding') return;
        const step = chain.plan[chain.index];
        if (!boardsEqual(toOccupancy(player.board), chain.expectedBoard) || !step ||
            player.player.pieceType !== step.piece) {
            clearGuidePlan(player);
        }
    }

    function clearForPlayer(player) {
        cancelActiveSearchFor(player);
        clearGuidePlan(player);
    }

    document.addEventListener('keydown', event => {
        if (event.defaultPrevented || event.repeat ||
            (typeof isBindingKey !== 'undefined' && isBindingKey) ||
            !isBoundKeyboardKey(event.key)) return;
        const target = event.target;
        if (target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.isContentEditable)) return;
        if (gameState !== 'PLAYING') return;
        event.preventDefault();
        event.stopPropagation();
        search();
    }, true);

    window.addEventListener('beforeunload', disposeWorker);

    window.PCFinder = { search, searchIfBoundKey, clearForPlayer, onBeforeLock, onAfterLock, onSpawn, onHold };

    document.addEventListener('DOMContentLoaded', () => {
        const element = button();
        if (element) element.addEventListener('click', () => search('pc'));
        document.getElementById('aiSearchBtn')?.addEventListener('click', () => search('ai'));
        document.getElementById('renSearchBtn')?.addEventListener('click', () => search('ren'));
    });
})();
