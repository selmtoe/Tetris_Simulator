/* On-demand PC guide UI. Use the configured 1P shortcut to search the live state. */

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

    // PC search feedback is the board guide itself. Keep this no-op so no
    // visible or screen-reader status log is produced, even with stale markup.
    function setStatus() {
    }

    function setSearching(searching) {
        const element = button();
        if (!element) return;
        element.disabled = searching;
        element.textContent = 'PC探索';
    }

    function snapshotFor(player) {
        return {
            board: player.board.map(row => row.map(cell => cell == null ? null : 'X')),
            currentPiece: player.player.pieceType,
            nextQueue: [...player.nextQueue],
            holdPiece: player.holdPiece || null,
            canHold: player.canHold,
            holdDisabled: player.holdDisabled
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
        if (gameMode !== '1P') return null;
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

    function discardGuidePlan(player, message = '', tone = 'muted') {
        clearGuidePlan(player);
        if (message) setStatus(message, tone);
    }

    function showCurrentStep(player, continuing) {
        const chain = guidePlan;
        if (!chain || chain.player !== player || chain.phase !== 'guiding') return false;
        if (!boardsEqual(toOccupancy(player.board), chain.expectedBoard)) {
            discardGuidePlan(player, '盤面が変わったため、PCガイドを解除しました。');
            return false;
        }

        const step = chain.plan[chain.index];
        const availability = stepAvailability(player, step);
        if (!step || !availability.available) {
            discardGuidePlan(player, 'HOLDまたはNEXTが変わったため、PCガイドを解除しました。');
            return false;
        }

        player.setPcGuide(step.cells, step.piece);
        if (!player.pcGuide) {
            discardGuidePlan(player, 'PCガイドを表示できませんでした。', 'error');
            return false;
        }

        const holdHint = availability.needsHold ? `（先にHOLDして ${step.piece} を出します）` : '';
        const remaining = chain.plan.length - chain.index;
        const prefix = continuing
            ? `PCガイド継続: 残り${remaining}手。`
            : `PCあり: ${chain.lines}ライン・${chain.depth}手。`;
        setStatus(`${prefix} 次は ${step.piece} を枠に置く ${holdHint}`, 'success');
        return true;
    }

    function startGuidePlan(player, data) {
        const plan = normalizePlan(data.plan);
        const expectedBoard = toOccupancy(player.board);
        if (!plan || !expectedBoard) {
            discardGuidePlan(player, 'PC探索の結果を解釈できませんでした。', 'error');
            return false;
        }

        guidePlan = {
            player,
            plan,
            index: 0,
            expectedBoard,
            pendingBoard: null,
            phase: 'guiding',
            lines: Number.isInteger(data.lines) ? data.lines : 0,
            depth: Number.isInteger(data.depth) ? data.depth : plan.length
        };
        return showCurrentStep(player, false);
    }

    function ensureWorker() {
        if (worker) return worker;

        worker = new Worker('./simulator/workers/pc-finder-worker.js?v=app-v5');
        worker.onmessage = event => {
            const data = event.data || {};
            if (!activeRequest || data.requestId !== activeRequest.id) return;

            const request = activeRequest;
            resetActiveRequest();

            const currentPlayer = getPlayableP1();
            if (!currentPlayer || currentPlayer !== request.player ||
                fingerprint(snapshotFor(currentPlayer)) !== request.fingerprint) {
                setStatus('局面が変わったため、PCガイドは表示しません。', 'muted');
                return;
            }

            if (data.type === 'error') {
                console.error('PC finder worker error:', data.message);
                setStatus('PC探索でエラーが発生しました。', 'error');
                return;
            }

            if (data.status === 'found' && Array.isArray(data.plan) && data.plan.length > 0) {
                startGuidePlan(currentPlayer, data);
                return;
            }

            if (data.status === 'not_found') {
                currentPlayer.clearPcGuide();
                setStatus('見えているNEXTの範囲ではPCは見つかりませんでした。', 'muted');
                return;
            }

            if (data.status === 'unsupported') {
                currentPlayer.clearPcGuide();
                setStatus(unsupportedMessage(data.reason), 'muted');
                return;
            }

            setStatus('PC探索の結果を解釈できませんでした。', 'error');
        };
        worker.onerror = error => {
            console.error('PC finder worker failed:', error);
            resetActiveRequest();
            disposeWorker();
            setStatus('PC探索Workerを開始できませんでした。', 'error');
        };
        return worker;
    }

    function unsupportedMessage(reason) {
        const messages = {
            board_too_high: '24段より上にブロックがあるため、PC探索の対象外です。',
            hold_unavailable: 'HOLD直後は探索できません。次のミノ出現後にPを押してください。',
            invalid_board: '盤面をPC探索用に読み取れませんでした。',
            invalid_current_piece: '現在ミノを読み取れませんでした。',
            invalid_next_piece: 'NEXTに未対応のミノがあります。',
            invalid_hold_piece: 'HOLDを読み取れませんでした。'
        };
        return messages[reason] || 'この局面はPC探索の対象外です。';
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

    function search() {
        if (activeRequest) return;

        const player = getPlayableP1();
        if (!player) {
            setStatus(gameMode === '2P' ? 'PC探索は現在1Pで利用できます。' : 'プレイ中のP1局面でPC探索できます。', 'muted');
            return;
        }

        clearGuidePlan(player);
        const snapshot = snapshotFor(player);
        const id = ++requestId;
        activeRequest = { id, player, fingerprint: fingerprint(snapshot) };
        setSearching(true);
        setStatus('見えているNEXTでPCを探索中…', 'pending', 0);

        try {
            ensureWorker().postMessage({ type: 'search', requestId: id, ...snapshot });
        } catch (error) {
            console.error('Unable to request PC search:', error);
            resetActiveRequest();
            disposeWorker();
            setStatus('PC探索を開始できませんでした。', 'error');
            return;
        }

        timeoutId = window.setTimeout(() => {
            if (!activeRequest || activeRequest.id !== id) return;
            resetActiveRequest();
            disposeWorker();
            setStatus('PC探索は時間切れです。局面を変えるか、もう一度Pを押してください。', 'muted');
        }, SEARCH_TIMEOUT_MS);
    }

    // Called by Player immediately before it writes the locked mino to board.
    function onBeforeLock(player) {
        if (cancelActiveSearchFor(player)) {
            discardGuidePlan(player, '局面が変わったため、PC探索を中止しました。');
            return false;
        }

        const chain = guidePlan;
        if (!chain || chain.player !== player) {
            if (player && typeof player.clearPcGuide === 'function') player.clearPcGuide();
            return false;
        }
        if (chain.phase !== 'guiding' || !boardsEqual(toOccupancy(player.board), chain.expectedBoard)) {
            discardGuidePlan(player, '盤面が変わったため、PCガイドを解除しました。');
            return false;
        }

        const step = chain.plan[chain.index];
        if (!step || player.player.pieceType !== step.piece || !cellsMatch(lockedCellsFor(player), step.cells)) {
            discardGuidePlan(player, 'ガイドと異なる配置のため、PCガイドを解除しました。');
            return false;
        }

        const pendingBoard = boardAfterPlacement(chain.expectedBoard, step.cells);
        if (!pendingBoard) {
            discardGuidePlan(player, '盤面が変わったため、PCガイドを解除しました。');
            return false;
        }

        chain.pendingBoard = pendingBoard;
        chain.phase = 'locking';
        player.clearPcGuide();
        setStatus('PCガイドを確認中…', 'pending', 0);
        return true;
    }

    // Called after Player.clearLines(), while the board is still the direct
    // result of the lock and before garbage/spawn processing can change it.
    function onAfterLock(player) {
        const chain = guidePlan;
        if (!chain || chain.player !== player || chain.phase !== 'locking') return;
        if (!boardsEqual(toOccupancy(player.board), chain.pendingBoard)) {
            discardGuidePlan(player, '盤面が変わったため、PCガイドを解除しました。');
            return;
        }

        chain.expectedBoard = chain.pendingBoard;
        chain.pendingBoard = null;
        chain.index++;
        if (chain.index >= chain.plan.length) {
            if (!isEmptyBoard(chain.expectedBoard)) {
                discardGuidePlan(player, 'PC手順の検証に失敗したため、ガイドを解除しました。', 'error');
                return;
            }
            clearGuidePlan(player);
            setStatus('PC手順を完了しました！', 'success');
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
            discardGuidePlan(player, 'PCガイドを終了しました。');
            return;
        }
        if (!boardsEqual(toOccupancy(player.board), chain.expectedBoard)) {
            discardGuidePlan(player, '盤面が変わったため、PCガイドを解除しました。');
            return;
        }

        chain.phase = 'guiding';
        showCurrentStep(player, true);
    }

    // A HOLD is valid only when it makes the guided mino the active mino.
    function onHold(player) {
        const chain = guidePlan;
        if (!chain || chain.player !== player || chain.phase !== 'guiding') return;
        const step = chain.plan[chain.index];
        if (!boardsEqual(toOccupancy(player.board), chain.expectedBoard) || !step ||
            player.player.pieceType !== step.piece) {
            discardGuidePlan(player, 'HOLDで局面が変わったため、PCガイドを解除しました。');
        }
    }

    function clearForPlayer(player) {
        const hadPendingSearch = cancelActiveSearchFor(player);
        const hadGuidePlan = Boolean(guidePlan && guidePlan.player === player);
        clearGuidePlan(player);
        if (hadPendingSearch) {
            setStatus('局面が変わったため、PC探索を中止しました。');
        } else if (hadGuidePlan) {
            setStatus('PCガイドを解除しました。');
        }
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
        if (element) element.addEventListener('click', search);
    });
})();
