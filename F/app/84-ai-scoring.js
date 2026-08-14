(() => {
    'use strict';

    let scoreWorker = null;
    let activeRunId = 0;
    let lastRun = null;
    let planAnimationObserver = null;
    let planAnimations = [];
    let scoreModalOpen = false;

    const cleanClone = value => JSON.parse(JSON.stringify(value));
    const byId = id => document.getElementById(id);

    function base64FromUtf8(text) {
        const bytes = new TextEncoder().encode(text);
        let binary = '';
        for (const byte of bytes) binary += String.fromCharCode(byte);
        return btoa(binary);
    }

    function copyText(text, successMessage) {
        const fallback = () => {
            const textarea = document.createElement('textarea');
            textarea.value = text;
            textarea.style.position = 'fixed';
            textarea.style.opacity = '0';
            document.body.appendChild(textarea);
            textarea.select();
            document.execCommand('copy');
            textarea.remove();
            alert(successMessage);
        };
        if (!navigator.clipboard || !navigator.clipboard.writeText) {
            fallback();
            return;
        }
        navigator.clipboard.writeText(text).then(() => alert(successMessage)).catch(fallback);
    }

    function formatScore(score) {
        return Number.isFinite(score) ? Math.round(score).toLocaleString() : '—';
    }

    function moveLabel(move) {
        if (!move) return '—';
        return `${move.hold ? 'Hold → ' : ''}${move.piece} (${move.x}, ${move.y}, R${move.rotation})${move.tspin ? ` T-Spin ${move.tspin}` : ''}`;
    }

    function moveBlocks(move, cellsOnly = false) {
        if (!move) return [];
        if (Array.isArray(move.cells)) {
            return move.cells
                .filter(cell => Array.isArray(cell) && cell.length >= 2)
                .map(cell => [Math.floor(cell[0]), Math.floor(cell[1])]);
        }
        // The recorded player move must only light the literal page-to-page
        // delta supplied by the worker.  Never make a reconstructed placement
        // look like an observed move when those four cells are unavailable.
        if (cellsOnly) return [];
        if (!move.piece || typeof getShape !== 'function') return [];
        return getShape(move.piece, move.rotation)
            .map(block => [Math.floor(move.x + block[0]), Math.floor(move.y + block[1])]);
    }

    function hasOwn(value, key) {
        return Boolean(value) && Object.prototype.hasOwnProperty.call(value, key);
    }

    function normalizeHold(value, fallback = '') {
        if (value === null) return '';
        if (typeof value === 'string') return value;
        if (Array.isArray(value)) return value[0] || '';
        return fallback;
    }

    function normalizeNext(value, fallback = '') {
        if (typeof value === 'string') return value;
        if (Array.isArray(value)) return value.join('');
        return fallback;
    }

    function initialComparisonPlayer(before) {
        return {
            board: Array.isArray(before?.board) ? before.board : [],
            hold: normalizeHold(before?.hold),
            next: normalizeNext(before?.next)
        };
    }

    function suppliedStateAfter(move) {
        const state = move?.stateAfter && typeof move.stateAfter === 'object' ? move.stateAfter : {};
        return {
            board: Array.isArray(state.board) ? state.board : (Array.isArray(move?.boardAfter) ? move.boardAfter : null),
            hasHold: hasOwn(state, 'hold') || hasOwn(move, 'holdAfter'),
            hold: hasOwn(state, 'hold') ? state.hold : move?.holdAfter,
            hasNext: hasOwn(state, 'next') || hasOwn(move, 'nextAfter'),
            next: hasOwn(state, 'next') ? state.next : move?.nextAfter
        };
    }

    function playerStateAfterMove(player, move) {
        const after = suppliedStateAfter(move);
        return {
            board: after.board || boardAfterMove(player.board, move),
            hold: after.hasHold ? normalizeHold(after.hold, player.hold) : player.hold,
            next: after.hasNext ? normalizeNext(after.next, player.next) : player.next
        };
    }

    // The glowing mino belongs to the board before it locks, while the side
    // queue intentionally shows the state after that exact move.  This makes
    // Hold choices and NEXT consumption visible without hiding the placement.
    function playerForHighlightedMove(beforePlayer, move) {
        const afterPlayer = playerStateAfterMove(beforePlayer, move);
        return {
            board: beforePlayer.board,
            hold: afterPlayer.hold,
            next: afterPlayer.next
        };
    }

    // Frame 0 is the source page.  Each later frame's board is the result of
    // the preceding plan move, i.e. the board before the currently glowing one.
    function playerBeforePlanMove(before, plan, moveIndex) {
        let player = initialComparisonPlayer(before);
        for (let index = 0; index < moveIndex; index++) {
            player = playerStateAfterMove(player, plan[index]);
        }
        return player;
    }

    function cloneComparisonBoard(board) {
        return Array.from({ length: BOARD_HEIGHT }, (_, y) => {
            const source = Array.isArray(board?.[y]) ? board[y] : [];
            return Array.from({ length: BOARD_WIDTH }, (_, x) => source[x] || null);
        });
    }

    function boardAfterMove(board, move) {
        const next = cloneComparisonBoard(board);
        for (const [x, y] of moveBlocks(move)) {
            if (x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT) next[y][x] = move.piece || 'G';
        }
        const remainingRows = next.filter(row => !row.every(Boolean));
        while (remainingRows.length < BOARD_HEIGHT) remainingRows.unshift(Array(BOARD_WIDTH).fill(null));
        return remainingRows;
    }

    function resultScore(result, kind) {
        const fields = kind === 'actual'
            ? ['detailedActualScore', 'finalActualScore', 'actualScore']
            : ['detailedBestScore', 'finalBestScore', 'bestScore'];
        for (const field of fields) {
            if (Number.isFinite(result?.[field])) return result[field];
        }
        return null;
    }

    function scoreGap(result) {
        const explicit = [result?.scoreGap, result?.scoreDifference, result?.lossScore, result?.loss]
            .find(Number.isFinite);
        if (Number.isFinite(explicit)) return Math.max(0, Math.round(explicit));
        const actual = resultScore(result, 'actual');
        const best = resultScore(result, 'best');
        return Number.isFinite(actual) && Number.isFinite(best) ? Math.max(0, Math.round(best - actual)) : 0;
    }

    function detailSearchLabel(result) {
        const nodes = [
            result?.detailNodes,
            result?.detailedNodes,
            result?.detailNodeBudget,
            result?.finalNodes,
            result?.finalNodeBudget
        ].find(Number.isFinite);
        if (Number.isFinite(nodes)) return `詳細探索 ${formatScore(nodes)} nodes`;
        return result?.detailedSearch || result?.detailSearched ? '詳細探索済み' : '';
    }

    function highlightedMoveStateLabel(afterPlayer, completedAiMoves = 0) {
        const firstNext = typeof afterPlayer?.next === 'string' && afterPlayer.next.length ? afterPlayer.next[0] : '—';
        const boardMoment = completedAiMoves === 0
            ? '盤面：この手を置く前'
            : `盤面：AI ${completedAiMoves}手後（この手を置く前）`;
        return `${boardMoment} ／ HOLD・NEXT：この手を置いた後（NEXT先頭：${firstNext}）`;
    }

    function hasHiddenRows(pages) {
        return pages.some(page => page.p1 && Array.isArray(page.p1.board) && page.p1.board.slice(0, 17)
            .some(row => Array.isArray(row) && row.some(cell => cell !== null && cell !== undefined)));
    }

    function comparisonViewY(board, move, cellsOnly = false) {
        const rows = [];
        if (Array.isArray(board)) {
            for (let y = 0; y < BOARD_HEIGHT; y++) {
                if (Array.isArray(board[y]) && board[y].some(Boolean)) rows.push(y);
            }
        }
        for (const [, y] of moveBlocks(move, cellsOnly)) if (y >= 0 && y < BOARD_HEIGHT) rows.push(y);
        if (!rows.length) return BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT;
        const top = Math.min(...rows);
        const bottom = Math.max(...rows);
        if (bottom - top < BOARD_VISIBLE_HEIGHT) {
            return Math.max(0, Math.min(BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT, top - 2));
        }
        const focus = Number.isFinite(move?.y) ? Math.floor(move.y) : bottom;
        return Math.max(0, Math.min(BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT, focus - 9));
    }

    function drawComparisonField(ctx, board, viewY) {
        ctx.fillStyle = 'rgba(0,0,0,0.5)';
        ctx.fillRect(0, 0, PLAYFIELD_WIDTH, BOARD_VISIBLE_HEIGHT * BLOCK_SIZE);
        ctx.strokeStyle = '#444';
        ctx.lineWidth = 0.5;
        for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y++) {
            for (let x = 0; x < BOARD_WIDTH; x++) {
                ctx.strokeRect(x * BLOCK_SIZE, y * BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE);
            }
        }
        ctx.strokeStyle = '#4b4b7c';
        ctx.lineWidth = 2;
        ctx.strokeRect(1, 1, PLAYFIELD_WIDTH - 2, BOARD_VISIBLE_HEIGHT * BLOCK_SIZE - 2);

        for (let visibleY = 0; visibleY < BOARD_VISIBLE_HEIGHT; visibleY++) {
            const boardY = viewY + visibleY;
            const row = Array.isArray(board?.[boardY]) ? board[boardY] : [];
            const isLineClear = row.length === BOARD_WIDTH && row.every(Boolean);
            for (let x = 0; x < BOARD_WIDTH; x++) {
                const piece = row[x];
                if (!piece) continue;
                const px = x * BLOCK_SIZE;
                const py = visibleY * BLOCK_SIZE;
                ctx.fillStyle = COLORS[piece] || '#fff';
                ctx.fillRect(px, py, BLOCK_SIZE, BLOCK_SIZE);
                if (isLineClear) {
                    ctx.fillStyle = 'rgba(255,255,255,0.3)';
                    ctx.fillRect(px, py, BLOCK_SIZE, BLOCK_SIZE);
                }
                ctx.strokeStyle = '#444';
                ctx.lineWidth = 0.5;
                ctx.strokeRect(px, py, BLOCK_SIZE, BLOCK_SIZE);
            }
        }
    }

    function drawComparisonMove(ctx, move, viewY, glow, cellsOnly = false) {
        if (!move || !move.piece) return;
        for (const [x, y] of moveBlocks(move, cellsOnly)) {
            if (x < 0 || x >= BOARD_WIDTH || y < viewY || y >= viewY + BOARD_VISIBLE_HEIGHT) continue;
            const px = x * BLOCK_SIZE;
            const py = (y - viewY) * BLOCK_SIZE;
            ctx.fillStyle = NEXT_COLORS[move.piece] || '#fff';
            ctx.globalAlpha = 1;
            ctx.fillRect(px, py, BLOCK_SIZE, BLOCK_SIZE);
            ctx.fillStyle = 'rgba(255,255,255,0.16)';
            ctx.fillRect(px, py, BLOCK_SIZE, BLOCK_SIZE);
            ctx.strokeStyle = '#444';
            ctx.lineWidth = 0.5;
            ctx.strokeRect(px, py, BLOCK_SIZE, BLOCK_SIZE);
        }
        ctx.globalAlpha = 1;
    }

    function drawComparisonCanvas(canvas, before, move, glow, options = {}) {
        const ctx = canvas.getContext('2d');
        const player = initialComparisonPlayer(before);
        canvas.width = PLAYER_CANVAS_WIDTH * RESOLUTION_SCALE;
        canvas.height = CANVAS_HEIGHT * RESOLUTION_SCALE;
        ctx.setTransform(RESOLUTION_SCALE, 0, 0, RESOLUTION_SCALE, 0, 0);
        ctx.clearRect(0, 0, PLAYER_CANVAS_WIDTH, CANVAS_HEIGHT);
        drawViewerUI(ctx, player, 0, player.next);
        const viewY = comparisonViewY(player.board, move, Boolean(options.cellsOnly));
        ctx.save();
        ctx.translate(PLAYFIELD_X_OFFSET, 0.5 * BLOCK_SIZE);
        drawComparisonField(ctx, player.board, viewY);
        drawComparisonMove(ctx, move, viewY, glow, Boolean(options.cellsOnly));
        ctx.restore();
    }

    function createMovePanel(title, kind, before, move, score, glow, detailLabel = '', options = {}) {
        const panel = document.createElement('section');
        panel.className = `ai-score-move-panel ${kind}`;
        const heading = document.createElement('h4');
        heading.textContent = title;
        const beforePlayer = initialComparisonPlayer(before);
        const displayPlayer = playerForHighlightedMove(beforePlayer, move);
        const startState = document.createElement('div');
        startState.className = 'ai-score-start-state';
        startState.textContent = highlightedMoveStateLabel(displayPlayer);
        const description = document.createElement('p');
        description.className = 'ai-score-move-description';
        description.textContent = moveLabel(move);
        const scoreText = document.createElement('div');
        scoreText.className = 'ai-score-move-score';
        scoreText.textContent = `評価値 ${formatScore(score)}`;
        const detail = document.createElement('div');
        detail.className = 'ai-score-detail-label';
        detail.textContent = detailLabel;
        detail.hidden = !detailLabel;
        const canvas = document.createElement('canvas');
        canvas.className = 'ai-score-board-canvas';
        canvas.setAttribute('aria-label', `${title}の盤面`);
        drawComparisonCanvas(canvas, displayPlayer, move, glow, options);
        panel.append(heading, startState, description, scoreText, detail, canvas);
        return panel;
    }

    function createPlanAnimation(canvas, description, progress, stateCaption, before, plan, glow) {
        let frame = 0;
        let timer = null;
        let inView = false;
        let destroyed = false;

        function renderFrame() {
            const move = plan[frame] || plan[0];
            const frameBefore = playerBeforePlanMove(before, plan, frame);
            const displayPlayer = playerForHighlightedMove(frameBefore, move);
            drawComparisonCanvas(canvas, displayPlayer, move, glow);
            description.textContent = moveLabel(move);
            progress.textContent = plan.length > 1 ? `AIの予定 ${frame + 1} / ${plan.length}` : 'AIの最善手';
            stateCaption.textContent = highlightedMoveStateLabel(displayPlayer, frame);
        }

        function pause() {
            if (timer !== null) window.clearTimeout(timer);
            timer = null;
        }

        function play() {
            if (timer !== null || plan.length < 2 || destroyed || !inView || !scoreModalOpen || document.hidden) return;
            timer = window.setTimeout(function advance() {
                timer = null;
                if (destroyed || !inView || !scoreModalOpen || document.hidden) return;
                frame = (frame + 1) % plan.length;
                renderFrame();
                play();
            }, 850);
        }

        renderFrame();
        return {
            element: canvas,
            setInView(visible) {
                inView = visible;
                if (visible) play(); else pause();
            },
            refresh() {
                if (inView) play(); else pause();
            },
            destroy() {
                destroyed = true;
                pause();
            }
        };
    }

    function stopPlanObservation() {
        if (planAnimationObserver) planAnimationObserver.disconnect();
        planAnimationObserver = null;
        for (const controller of planAnimations) controller.setInView(false);
    }

    function destroyPlanAnimations() {
        stopPlanObservation();
        for (const controller of planAnimations) controller.destroy();
        planAnimations = [];
    }

    function observePlanAnimations(scrollRoot) {
        stopPlanObservation();
        if (!scoreModalOpen || !planAnimations.length) return;
        if (typeof IntersectionObserver !== 'function') {
            for (const controller of planAnimations) controller.setInView(true);
            return;
        }
        planAnimationObserver = new IntersectionObserver(entries => {
            for (const entry of entries) {
                const controller = planAnimations.find(item => item.element === entry.target);
                if (controller) controller.setInView(entry.isIntersecting && entry.intersectionRatio >= 0.25);
            }
        }, { root: scrollRoot, threshold: [0, 0.25] });
        for (const controller of planAnimations) planAnimationObserver.observe(controller.element);
    }

    function createAiPlanPanel(before, result, detailLabel) {
        const plan = (Array.isArray(result.aiPlan) ? result.aiPlan : [])
            .filter(move => move && move.piece);
        if (!plan.length && result.bestMove) plan.push(result.bestMove);
        const panel = document.createElement('section');
        panel.className = 'ai-score-move-panel best';
        const heading = document.createElement('h4');
        heading.textContent = 'AIの手';
        const startState = document.createElement('div');
        startState.className = 'ai-score-start-state';
        startState.textContent = highlightedMoveStateLabel(initialComparisonPlayer(before));
        const description = document.createElement('p');
        description.className = 'ai-score-move-description';
        const scoreText = document.createElement('div');
        scoreText.className = 'ai-score-move-score';
        scoreText.textContent = `評価値 ${formatScore(resultScore(result, 'best'))}`;
        const detail = document.createElement('div');
        detail.className = 'ai-score-detail-label';
        detail.textContent = detailLabel;
        detail.hidden = !detailLabel;
        const progress = document.createElement('div');
        progress.className = 'ai-score-plan-progress';
        const canvas = document.createElement('canvas');
        canvas.className = 'ai-score-board-canvas';
        canvas.setAttribute('aria-label', 'AIの予定手順の盤面');
        panel.append(heading, startState, description, scoreText, detail, progress, canvas);

        if (plan.length) planAnimations.push(createPlanAnimation(canvas, description, progress, startState, before, plan, '#56d8ff'));
        else {
            description.textContent = '—';
            startState.textContent = '盤面・HOLD・NEXT：開始局面';
            progress.textContent = '';
            drawComparisonCanvas(canvas, before, null, '#56d8ff');
        }
        return panel;
    }

    function createScoreCard(run, result) {
        const before = run.pages[result.pageIndex]?.p1 || {};
        const displayBefore = {
            ...before,
            board: Array.isArray(result.sourceBoard)
                ? result.sourceBoard
                : (Array.isArray(result.displayBoard) ? result.displayBoard : before.board)
        };
        const card = document.createElement('article');
        card.className = 'ai-score-card';
        card.setAttribute('role', 'group');
        card.title = `Page ${result.pageIndex + 1} の局面を譜面エディタで開く`;
        const header = document.createElement('header');
        header.className = 'ai-score-card-header';
        header.setAttribute('role', 'link');
        header.tabIndex = 0;
        header.title = card.title;
        card.removeAttribute('title');
        const title = document.createElement('h4');
        title.className = 'ai-score-card-title';
        title.textContent = `Page ${result.pageIndex + 1} → Page ${result.pageIndex + 2} の手`;
        const gap = document.createElement('div');
        gap.className = 'ai-score-card-gap';
        gap.textContent = `差 ${formatScore(scoreGap(result))}点`;
        header.append(title, gap);
        const boards = document.createElement('div');
        boards.className = 'ai-score-card-boards';
        const detailLabel = detailSearchLabel(result);
        boards.append(
            createMovePanel(
                '実際の手',
                'actual',
                displayBefore,
                result.actualMove,
                resultScore(result, 'actual'),
                '#ff5c73',
                detailLabel,
                { cellsOnly: true }
            ),
            createAiPlanPanel(displayBefore, result, detailLabel)
        );
        card.append(header, boards);
        return card;
    }

    document.addEventListener('DOMContentLoaded', () => {
        const modal = byId('ai-score-modal');
        const modalContent = modal.querySelector('.ai-score-content');
        const openButtons = [byId('viewer-ai-score-btn')];
        const closeButton = byId('ai-score-close');
        const runButton = byId('ai-score-run');
        const stopButton = byId('ai-score-stop');
        const startInput = byId('ai-score-range-start');
        const endInput = byId('ai-score-range-end');
        const rangeTrack = byId('ai-score-range-track');
        const rangeLabel = byId('ai-score-range-label');
        const nodeInput = byId('ai-score-nodes');
        const nodeLabel = byId('ai-score-nodes-label');
        const planLengthInput = byId('ai-score-plan-length');
        const planLengthLabel = byId('ai-score-plan-length-label');
        const thresholdInput = byId('ai-score-threshold');
        const thresholdLabel = byId('ai-score-threshold-label');
        const status = byId('ai-score-status');
        const resultPanel = byId('ai-score-results');
        const resultSummary = byId('ai-score-summary');
        const resultList = byId('ai-score-list');
        const openEditorButton = byId('ai-score-open-editor');
        const copyEditorButton = byId('ai-score-copy-editor');
        const copyMobileButton = byId('ai-score-copy-mobile');
        const openMobileButton = byId('ai-score-open-mobile');

        let rangeWasOpened = false;

        function sameReplayBoard(left, right) {
            return JSON.stringify(left?.p1?.board || []) === JSON.stringify(right?.p1?.board || []);
        }

        function replayOperationIndices() {
            return fumenPages.map((page, index) => {
                if (!operationForPage(page?.p1)) return -1;
                const nextPage = fumenPages[index + 1];
                // 2P collection pages may carry P1's operation while only P2
                // changes. Score the operation on the final carried page,
                // where the following P1 board actually contains the lock.
                return nextPage && sameReplayBoard(page, nextPage) ? -1 : index;
            }).filter(index => index >= 0);
        }

        function scoringPageIndices() {
            if (typeof currentCaseIsReplay === 'function' && currentCaseIsReplay()) {
                return replayOperationIndices();
            }
            return fumenPages.map((_, index) => index).slice(0, -1);
        }

        function movePageCount() {
            return scoringPageIndices().length;
        }

        function updateRange(source) {
            const max = Math.max(1, movePageCount());
            startInput.max = String(max);
            endInput.max = String(max);
            if (!rangeWasOpened) {
                startInput.value = '1';
                endInput.value = String(max);
                rangeWasOpened = true;
            }
            let start = Math.max(1, Math.min(max, Number(startInput.value) || 1));
            let end = Math.max(1, Math.min(max, Number(endInput.value) || max));
            if (source === 'start' && start > end) end = start;
            if (source === 'end' && end < start) start = end;
            startInput.value = String(start);
            endInput.value = String(end);
            const startPercent = max === 1 ? 0 : (start - 1) * 100 / (max - 1);
            const endPercent = max === 1 ? 100 : (end - 1) * 100 / (max - 1);
            rangeTrack.style.background = `linear-gradient(to right, var(--primary-color) 0 ${startPercent}%, var(--primary-hover-color) ${startPercent}% ${endPercent}%, var(--primary-color) ${endPercent}% 100%)`;
            const disabled = movePageCount() === 0;
            startInput.disabled = disabled;
            endInput.disabled = disabled;
            runButton.disabled = disabled || Boolean(scoreWorker);
            rangeLabel.textContent = disabled ? '採点できる手がありません' : `Page ${start} 〜 Page ${end}`;
        }

        function updateControls() {
            nodeLabel.textContent = `${Number(nodeInput.value).toLocaleString()} nodes / 手（候補抽出）`;
            planLengthLabel.textContent = `${Number(planLengthInput.value)}手`;
            thresholdLabel.textContent = `${Number(thresholdInput.value).toLocaleString()}点以上の差を悪手にする`;
        }

        function stopScoring(message) {
            if (scoreWorker) scoreWorker.terminate();
            scoreWorker = null;
            runButton.disabled = movePageCount() === 0;
            stopButton.hidden = true;
            if (message) status.textContent = message;
        }

        function badSituationPages(run) {
            const indices = new Set();
            for (const result of run.results) {
                if (!result.blunder) continue;
                indices.add(result.pageIndex);
                indices.add(result.pageIndex + 1);
            }
            return [...indices]
                .filter(index => Number.isInteger(index) && index >= 0 && index < run.pages.length)
                .sort((a, b) => a - b)
                .map(index => cleanClone(run.pages[index]));
        }

        function renderResults(run) {
            destroyPlanAnimations();
            const scored = run.results.filter(result => result.status === 'scored' || result.status === 'inferred');
            const blunders = scored.filter(result => result.blunder);
            resultPanel.hidden = false;
            resultList.replaceChildren();
            const attempted = run.results.length;
            resultSummary.textContent = `${scored.length}/${attempted} 手を採点（${blunders.length} 件を要確認）`;
            if (!blunders.length) {
                const empty = document.createElement('p');
                empty.className = 'ai-score-empty';
                empty.textContent = '設定した閾値を超える悪手はありませんでした。';
                resultList.appendChild(empty);
            }

            for (const result of blunders) {
                const card = createScoreCard(run, result);
                const openSourcePage = () => { void openSourcePageInEditor(run, result); };
                const header = card.querySelector('.ai-score-card-header');
                header.addEventListener('click', openSourcePage);
                header.addEventListener('keydown', event => {
                    if (event.key !== 'Enter' && event.key !== ' ') return;
                    event.preventDefault();
                    openSourcePage();
                });
                resultList.appendChild(card);
            }

            const outputPages = badSituationPages(run);
            const outputs = [openEditorButton, copyEditorButton, copyMobileButton, openMobileButton];
            for (const button of outputs) button.disabled = outputPages.length === 0;
            const warning = byId('ai-score-mobile-warning');
            warning.hidden = !outputPages.length || !hasHiddenRows(outputPages);
            window.requestAnimationFrame(() => observePlanAnimations(modalContent));
        }

        function getOutputPages() {
            return lastRun ? badSituationPages(lastRun) : [];
        }

        function editorOutputData() {
            const pages = getOutputPages();
            return pages.length ? getFumenDataForExport(pages, lastRun.mode) : null;
        }

        async function editorOutputUrl() {
            const data = editorOutputData();
            if (!data) return null;
            const url = new URL(window.location.href);
            url.hash = await encodeSharedStateHash(JSON.stringify(data));
            return url.href;
        }

        async function sourcePageEditorUrl(run, result) {
            const sourcePage = run?.pages?.[result?.pageIndex];
            if (!sourcePage) return null;
            const data = getFumenDataForExport([cleanClone(sourcePage)], run.mode);
            const url = new URL(window.location.href);
            url.searchParams.delete('clean');
            url.searchParams.set('view', 'editor');
            url.searchParams.set('page', '1');
            url.hash = await encodeSharedStateHash(JSON.stringify(data));
            return url.href;
        }

        async function openSourcePageInEditor(run, result) {
            const url = await sourcePageEditorUrl(run, result);
            if (url) window.open(url, '_blank', 'noopener');
        }

        function openScoringModal() {
            updateRange();
            updateControls();
            scoreModalOpen = true;
            modal.setAttribute('aria-hidden', 'false');
            modal.style.display = 'flex';
            window.requestAnimationFrame(() => observePlanAnimations(modalContent));
        }

        function closeScoringModal() {
            if (scoreWorker) stopScoring('AI採点を中止しました。');
            scoreModalOpen = false;
            stopPlanObservation();
            modal.setAttribute('aria-hidden', 'true');
            modal.style.display = 'none';
        }

        for (const openButton of openButtons) openButton.addEventListener('click', openScoringModal);
        closeButton.addEventListener('click', closeScoringModal);
        modal.addEventListener('click', event => {
            if (event.target === modal) closeScoringModal();
        });
        document.addEventListener('visibilitychange', () => {
            for (const controller of planAnimations) controller.refresh();
        });
        startInput.addEventListener('input', () => updateRange('start'));
        endInput.addEventListener('input', () => updateRange('end'));
        nodeInput.addEventListener('input', updateControls);
        planLengthInput.addEventListener('input', updateControls);
        thresholdInput.addEventListener('input', updateControls);
        stopButton.addEventListener('click', () => stopScoring('AI採点を中止しました。'));

        runButton.addEventListener('click', () => {
            if (scoreWorker || movePageCount() === 0) return;
            const pages = cleanClone(fumenPages);
            const runId = ++activeRunId;
            const startPage = Number(startInput.value) - 1;
            const endPage = Number(endInput.value) - 1;
            const replay = typeof currentCaseIsReplay === 'function' && currentCaseIsReplay();
            const operationPages = replay ? scoringPageIndices() : null;
            const replayInitial = replay ? cleanClone(currentCase()?.initial || {}) : null;
            lastRun = null;
            destroyPlanAnimations();
            resultPanel.hidden = true;
            resultList.replaceChildren();
            runButton.disabled = true;
            stopButton.hidden = false;
            status.textContent = 'Cold Clear で局面を復元しています…';

            scoreWorker = new Worker('./app/84-ai-scoring-worker.js');
            scoreWorker.onmessage = event => {
                const message = event.data || {};
                if (message.runId !== runId) return;
                if (message.type === 'progress') {
                    status.textContent = `${message.completed} / ${message.total} 手を採点中…`;
                    return;
                }
                if (message.type === 'error') {
                    stopScoring(`AI採点に失敗しました: ${message.message}`);
                    return;
                }
                if (message.type === 'done') {
                    const run = {
                        pages,
                        mode: gameMode,
                        replay,
                        results: Array.isArray(message.results) ? message.results : []
                    };
                    scoreWorker.terminate();
                    scoreWorker = null;
                    stopButton.hidden = true;
                    runButton.disabled = movePageCount() === 0;
                    lastRun = run;
                    status.textContent = 'AI採点が完了しました。';
                    renderResults(run);
                }
            };
            scoreWorker.onerror = event => {
                stopScoring(`AI採点 Worker のエラー: ${event.message || '不明なエラー'}`);
            };
            scoreWorker.postMessage({
                type: 'score',
                runId,
                pages,
                startPage,
                endPage,
                replay,
                operationPages,
                replayInitial,
                nodeBudget: Number(nodeInput.value),
                planLength: Number(planLengthInput.value),
                thresholdScore: Number(thresholdInput.value)
            });
        });

        openEditorButton.addEventListener('click', async () => {
            const url = await editorOutputUrl();
            if (url) window.open(url, '_blank', 'noopener');
        });
        copyEditorButton.addEventListener('click', async () => {
            const url = await editorOutputUrl();
            if (url) copyText(url, '悪手局面の譜面エディタリンクをコピーしました。');
        });
        copyMobileButton.addEventListener('click', () => {
            const pages = getOutputPages();
            if (!pages.length) return;
            const code = FumenCodec.export(pages, 'p1');
            copyText(`https://knewjade.github.io/fumen-for-mobile/#?d=${code}`, 'Fumen for Mobile のリンクをコピーしました。');
        });
        openMobileButton.addEventListener('click', () => {
            const pages = getOutputPages();
            if (!pages.length) return;
            const code = FumenCodec.export(pages, 'p1');
            window.open(`https://knewjade.github.io/fumen-for-mobile/#?d=${code}`, '_blank', 'noopener');
        });
    });
})();
