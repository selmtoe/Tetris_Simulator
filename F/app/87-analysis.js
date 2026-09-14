/* PC/REN route previews preserve the open replay and its current page. */
(() => {
    'use strict';
    let worker, timer, generation = 0, frames = [], route = [], frameIndex = 0, source, originPage, mode = 'pc';
    let dialog, status, canvas, slider, position, practice, stop, playerSelect;
    const clone = value => JSON.parse(JSON.stringify(value));
    const pieces = value => String(value || '').toUpperCase().split('E')[0].replace(/[^IOTLSJZ]/g, '');

    function stateAt(playerId, index) {
        const page = fumenPages[index][playerId];
        let queue = pieces(displayNextForPage(playerId, index)).split('');
        let current = Object.hasOwn(page, 'active') ? pieces(page.active)[0] : operationForPage(page)?.type || queue.shift();
        let hold = page.hold || null;
        if (currentCaseIsReplay()) {
            const state = replayStateAtPage(currentCase(), playerId, index);
            current = state.current; queue = state.queue; hold = state.hold || null;
        }
        if (!current && queue.length) current = queue.shift();
        return {current:current || null, queue:[...queue], hold, operation:operationForPage(page), page};
    }

    // Join rolling previews using the actual lock/HOLD transitions. Overlap
    // alone is ambiguous for repeated pieces and merged P1/P2 timeline pages.
    function extendKnownQueue(initial, following, recordedSequence = '') {
        const known = [...initial.queue];
        const sequence = pieces(recordedSequence);
        const recordedTail = () => {
            const prefix = known.join(''), start = prefix.length >= 3 ? sequence.indexOf(prefix) : -1;
            return start >= 0 && sequence.indexOf(prefix, start + 1) < 0 ? sequence.slice(start).split('') : null;
        };
        // Legacy replays already store the full remaining queue. Avoid walking
        // thousands of pages (and repeatedly cloning their long suffixes).
        const direct = recordedTail();
        if (direct) return direct;
        let previous = initial, offset = 0;
        for (const next of following) {
            // Merged timelines repeat the player's pending operation while
            // only the opponent advances. It must not be locked twice.
            if (previous.page && next.page && previous.current === next.current && previous.hold === next.hold &&
                previous.queue.join('') === next.queue.join('') &&
                JSON.stringify(previous.page.board) === JSON.stringify(next.page.board)) {previous = next;continue;}
            const queue = known.slice(offset), operation = previous.operation;
            let consumed = 0, current = previous.current, hold = previous.hold || null;
            if (operation) {
                if (operation.type !== current) {
                    if (hold === operation.type) {hold = current;}
                    else if (!hold && queue[0] === operation.type) {hold = current;consumed++;}
                    else break;
                }
                current = queue[consumed++];
            } else if (previous.page && next.page && !Object.hasOwn(previous.page, 'active') &&
                       JSON.stringify(previous.page.board) !== JSON.stringify(next.page.board)) break;
            const candidates = [{current, hold, consumed}];
            if (hold) candidates.push({current:hold, hold:current, consumed});
            else if (queue[consumed]) candidates.push({current:queue[consumed], hold:current, consumed:consumed + 1});
            const offsets = new Set(candidates.filter(candidate => candidate.current && candidate.current === next.current &&
                (candidate.hold || null) === (next.hold || null) &&
                next.queue.every((piece, i) => !queue[candidate.consumed + i] || queue[candidate.consumed + i] === piece))
                .map(candidate => candidate.consumed));
            // A gap or contradictory recognition result is not a random bag.
            if (offsets.size !== 1) break;
            offset += [...offsets][0];
            for (let i = Math.max(0, known.length - offset); i < next.queue.length; i++) known.push(next.queue[i]);
            previous = next;
        }
        // Recorded full streams are authoritative, but never choose arbitrarily
        // between repeated windows. Rolling transitions above disambiguate them.
        return recordedTail() || known;
    }

    function snapshot(playerId, kind = mode) {
        const page = fumenPages[currentPageIndex][playerId], state = stateAt(playerId, currentPageIndex);
        let queue = state.queue;
        if (kind === 'ren') {
            function* following() {for (let i = currentPageIndex + 1; i < fumenPages.length; i++) yield stateAt(playerId, i);}
            queue = extendKnownQueue(state, following(), currentCase()?.initial?.[playerId]?.sequence);
        }
        const current = state.current, hold = state.hold;
        return { board: clone(page.board), currentPiece: current, nextQueue: [...queue], holdPiece: hold,
            canHold: true, ren: Number.isInteger(page.ren) ? page.ren : -1 };
    }

    function routeFrames(initial, plan) {
        const result = [clone(initial)];
        for (const step of plan) {
            const next = clone(result[result.length - 1]);
            if (step.hold || step.piece !== next.currentPiece) {
                const previous = next.currentPiece;
                next.currentPiece = next.holdPiece || next.nextQueue.shift();
                next.holdPiece = previous;
            }
            if (step.piece !== next.currentPiece) throw new Error('手順のHOLD・NEXTが一致しません。');
            const seen = new Set();
            for (const { x, y } of step.cells || []) {
                if (!Number.isInteger(x) || !Number.isInteger(y) || x < 0 || x >= 10 || y < 0 || y >= 40 || next.board[y][x] || seen.has(`${x},${y}`)) throw new Error('手順の配置を検証できませんでした。');
                seen.add(`${x},${y}`); next.board[y][x] = step.piece;
            }
            if (seen.size !== 4) throw new Error('手順のミノを読み取れませんでした。');
            const remaining = next.board.filter(row => !row.every(Boolean));
            while (remaining.length < 40) remaining.unshift(Array(10).fill(null));
            next.board = remaining;
            next.currentPiece = next.nextQueue.shift() || null;
            result.push(next);
        }
        return result;
    }

    function draw() {
        if (!frames.length) return;
        const state = frames[frameIndex];
        const step = route[frameIndex];
        const move = step ? { ...step, cells: step.cells.map(cell => [cell.x, cell.y]) } : null;
        TetrisAnalysisCanvas.draw(canvas, { board: state.board, hold: state.holdPiece,
            next: state.nextQueue.slice(0, 10).join('') }, move, '#fff', { cellsOnly: true });
        position.textContent = step ? `${frameIndex + 1} / ${route.length}手${step.hold || step.piece !== state.currentPiece ? ' · HOLD' : ''}` : route.length ? '完了' : '開始局面';
        slider.value = frameIndex;
        practice.disabled = !state.currentPiece;
        dialog.querySelector('[data-route-prev]').disabled = frameIndex === 0;
        dialog.querySelector('[data-route-next]').disabled = frameIndex >= route.length;
    }

    function cancel() {
        generation++;
        clearTimeout(timer);
        worker?.terminate(); worker = null;
        if (stop) stop.hidden = true;
    }

    function run() {
        cancel();
        frames = []; route = []; frameIndex = 0;
        source = snapshot(playerSelect.value);
        originPage = fumenPages[currentPageIndex];
        frames = [clone(source)]; slider.max = 0; slider.disabled = true; draw();
        status.textContent = `${mode === 'pc' ? 'PC' : 'REN'}探索中… · 現在ミノ＋NEXT ${source.nextQueue.length}個`;
        stop.hidden = false;
        const id = generation;
        worker = new Worker('../simulator/workers/route-search-worker.js?v=ren-known-queue-v2');
        worker.onmessage = event => {
            if (generation !== id) return;
            if (fumenPages[currentPageIndex] !== originPage) { cancel(); dialog.close(); return; }
            const data = event.data;
            if (data.type !== 'progress') { clearTimeout(timer); stop.hidden = true; worker?.terminate(); worker = null; }
            if (data.type === 'error') { status.textContent = data.message; return; }
            if (data.plan?.length) {
                try {
                    route = data.plan; frames = routeFrames(source, route);
                    slider.max = route.length; slider.disabled = false;
                    frameIndex = Math.min(frameIndex, route.length); draw();
                    status.textContent = mode === 'ren'
                        ? `${data.complete ? '最大' : '探索中 · 暫定'} ${data.ren} REN（${route.length}回連続消去 · 既知NEXT ${source.nextQueue.length}個）`
                        : `${data.lines}ラインPC · ${route.length}手`;
                } catch (error) { cancel(); status.textContent = error.message; }
            } else if (data.type === 'progress') status.textContent = 'REN探索中…';
            else if (data.status === 'unsupported') status.textContent = data.reason === 'board_too_high' ? 'PC探索は下から24段までの局面に対応しています。' : 'このHOLD・NEXTの状態ではPC探索できません。';
            else status.textContent = mode === 'ren' ? 'この局面から連続消去できる手順はありません。' : '既知のNEXTではPCが見つかりませんでした。';
        };
        worker.onerror = event => {
            // A terminated Worker can still emit a failed script-load event.
            // It must not cancel the replacement query for another player.
            if (generation !== id) { event.preventDefault(); return; }
            cancel(); status.textContent = `探索に失敗しました: ${event.message}`;
        };
        worker.postMessage({ type: 'search', requestId: id, kind: mode, ...source });
        if (mode === 'pc') timer = setTimeout(() => { cancel(); status.textContent = 'PC探索は時間切れです。'; }, 10000);
    }

    function open(kind) {
        mode = kind;
        dialog.querySelector('h2').textContent = kind === 'pc' ? 'PC探索' : 'REN探索';
        playerSelect.hidden = gameMode !== '2P';
        if (gameMode !== '2P') playerSelect.value = 'p1';
        dialog.showModal(); run();
    }

    document.addEventListener('DOMContentLoaded', () => {
        dialog = document.createElement('dialog');
        dialog.className = 'route-dialog'; dialog.id = 'route-dialog'; dialog.setAttribute('aria-labelledby', 'route-title');
        dialog.innerHTML = '<h2 id="route-title"></h2><select aria-label="探索するプレイヤー"><option value="p1">P1</option><option value="p2">P2</option></select><p class="route-status" aria-live="polite"></p><canvas aria-label="探索した手順の盤面"></canvas><input type="range" min="0" max="0" value="0" aria-label="手順"><div class="route-actions"><button class="button" data-route-prev aria-label="前の手">←</button><span class="route-position"></span><button class="button" data-route-next aria-label="次の手">→</button></div><div class="route-actions"><button class="button" data-route-practice>シミュレータ</button><button class="button" data-route-stop>中止</button><button class="button" data-route-close>閉じる</button></div>';
        document.body.append(dialog);
        status = dialog.querySelector('.route-status'); canvas = dialog.querySelector('canvas');
        slider = dialog.querySelector('input'); position = dialog.querySelector('.route-position');
        playerSelect = dialog.querySelector('select'); stop = dialog.querySelector('[data-route-stop]'); practice = dialog.querySelector('[data-route-practice]');
        playerSelect.addEventListener('change', run);
        slider.addEventListener('input', () => { frameIndex = Number(slider.value); draw(); });
        dialog.querySelector('[data-route-prev]').onclick = () => { frameIndex = Math.max(0, frameIndex - 1); draw(); };
        dialog.querySelector('[data-route-next]').onclick = () => { frameIndex = Math.min(route.length, frameIndex + 1); draw(); };
        dialog.querySelector('[data-route-close]').onclick = () => dialog.close();
        stop.onclick = () => { cancel(); status.textContent += ' · 中止（最大値は未確定）'; };
        dialog.addEventListener('close', cancel);
        dialog.addEventListener('keydown', event => event.stopPropagation());
        practice.onclick = () => {
            const state = frames[frameIndex];
            if (!state?.currentPiece) return;
            const data = { v: 2, m: '1P', p1: { b: boardToString(state.board), h: state.holdPiece || '', n: [state.currentPiece, ...state.nextQueue].join('') } };
            dialog.close(); window.TetrisWorkspace.practice(data);
        };
        document.getElementById('viewer-pc-search-btn').onclick = () => open('pc');
        document.getElementById('viewer-ren-search-btn').onclick = () => open('ren');
    });
    window.PositionAnalysis = { snapshot, routeFrames, extendKnownQueue };
})();
