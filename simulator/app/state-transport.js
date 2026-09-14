/* State serialization, sharing, Hub transport, game loop, and viewport scaling. */

function boardToString(board) {
    return board.map(row => 
        row.map(cell => cell === null ? '_' : cell).join('')
    ).join('');
}

function stringToBoard(str) {
    const board = [];
    if (!str || str.length !== BOARD_WIDTH * BOARD_HEIGHT) {
        console.error('Invalid board string length. Returning empty board.');
        return Array.from({ length: BOARD_HEIGHT }, () => Array(BOARD_WIDTH).fill(null));
    }
    for (let i = 0; i < BOARD_HEIGHT; i++) {
        const rowStr = str.substring(i * BOARD_WIDTH, (i + 1) * BOARD_WIDTH);
        const row = rowStr.split('').map(char => (char === '_') ? null : char);
        board.push(row);
    }
    return board;
}

// --- 圧縮ロジック (RLE & 差分) ---
const RLE_EQUAL_CHAR = 'E';

/**
 * 1次元配列をランレングス圧縮する
 * @param {Array<string>} data - 1次元配列 ('_' または 'E' を含む)
 * @returns {Array<[string, number]>} 圧縮データ [[value, count], ...]
 */
function encodeRLE(data) {
    if (!data || data.length === 0) return [];
    const rle = [];
    let lastValue = data[0];
    let count = 1;
    for (let i = 1; i < data.length; i++) {
        const currentValue = data[i];
        if (currentValue === lastValue) {
            count++;
        } else {
            rle.push([lastValue, count]);
            lastValue = currentValue;
            count = 1;
        }
    }
    rle.push([lastValue, count]); // 最後のデータを追加
    return rle;
}

/**
 * 2つの1次元配列ボードデータの差分を取得する
 * 'E' (Equal) は変更なしを示す
 * @param {Array<string>} prevBoard1D - 前のボード (1D, '_'含む)
 * @param {Array<string>} currentBoard1D - 現在のボード (1D, '_'含む)
 * @returns {Array<string>} 差分データ (1D)
 */
function getDifference(prevBoard1D, currentBoard1D) {
    const diff = [];
    const len = BOARD_WIDTH * BOARD_HEIGHT;
    for (let i = 0; i < len; i++) {
        const prev = prevBoard1D[i];
        const curr = currentBoard1D[i];

        if (prev === curr) {
            diff.push(RLE_EQUAL_CHAR); // 'E' (Equal) 変更なし
        } else {
            diff.push(curr); // 変更後の値 (例: '_', 'I', 'O'...)
        }
    }
    return diff;
}

let autoStartParams = { ss: false, nh: false, hb: false };
function getGameStateForExport(options = {}) {
    // データが未初期化の場合はダミーを返す
    if (!editorData.p1.board) editorData.p1.board = Array.from({ length: 40 }, () => Array(10).fill(null));
    if (gameMode === '2P' && !editorData.p2.board) editorData.p2.board = Array.from({ length: 40 }, () => Array(10).fill(null));

    const p1Data = { ...editorData.p1, nextQueue: [...editorData.p1.nextQueue] };
const p2Data = gameMode === '2P' ? { ...editorData.p2, nextQueue: [...editorData.p2.nextQueue] } : null;
if (options.noHold) {

        if (p1Data.hold) { p1Data.nextQueue.unshift(p1Data.hold); p1Data.hold = null; }
        if (p2Data && p2Data.hold) { p2Data.nextQueue.unshift(p2Data.hold); p2Data.hold = null; }
    }

    const data = {
        v: 2, m: gameMode,
        p1: { b: boardToString(p1Data.board), n: p1Data.nextQueue.join(''), h: p1Data.hold || '' }
    };
if (gameMode === '2P') {
        data.p2 = { b: boardToString(p2Data.board), n: p2Data.nextQueue.join(''), h: p2Data.hold || '' };
    }
    if (options.startSim) data.ss = 1;
    if (options.noHold) data.nh = 1;
    if (options.hideBack) data.hb = 1;
    
    if (editorData.rule.description) data.rd = editorData.rule.description;
    if (editorData.rule.code) data.rc = editorData.rule.code;
    return data;
}

function replayCollectionToGameState(collection) {
    if (typeof TetrisEventCodec !== 'undefined' && TetrisEventCodec.isEventReplay(collection)) {
        collection = TetrisEventCodec.decodeCollection(collection);
    }
    const cases = Array.isArray(collection?.cases) ? collection.cases : [];
    const caseIndex = Math.max(0, Math.min(Number(collection?.currentCase) || 0, cases.length - 1));
    const activeCase = cases[caseIndex];
    if (!activeCase || activeCase.kind !== 'replay' || !activeCase.initial) return null;
    const toPlayerState = initial => ({
        b: boardToString(initial?.board || Array.from({ length: BOARD_HEIGHT }, () => Array(BOARD_WIDTH).fill(null))),
        n: String(initial?.sequence || '').replace(/[^IOTLSJZ]/gi, ''),
        h: String(initial?.hold || '').replace(/[^IOTLSJZ]/gi, '')
    });
    const result = {
        v: 2,
        m: activeCase.gameMode || collection.m || '1P',
        p1: toPlayerState(activeCase.initial.p1)
    };
    if (result.m === '2P') result.p2 = toPlayerState(activeCase.initial.p2);
    return result;
}

function applyGameState(data) {
    try {
        if (data?.v === 3 || (typeof TetrisEventCodec !== 'undefined' && TetrisEventCodec.isEventReplay(data))) {
            data = replayCollectionToGameState(data);
        }
        if (!data || (data.v !== 1 && data.v !== 2)) {
            alert('無効または非対応のデータです。');
return false;
        }
        gameMode = data.m || '1P';
        document.getElementById('mode-1p').classList.toggle('active', gameMode === '1P');
document.getElementById('mode-2p').classList.toggle('active', gameMode === '2P');
        document.getElementById('p2-editor-col').style.display = (gameMode === '2P') ? 'flex' : 'none';
        document.getElementById('swapBtn').style.display = (gameMode === '2P') ? 'inline-block' : 'none';
        
        editorData.rule.description = data.rd || '';
editorData.rule.code = data.rc || '';

        if (data.p1) {
            editorData.p1.board = stringToBoard(data.p1.b);
            editorData.p1.nextQueue = data.p1.n ? data.p1.n.split('') : [];
            editorData.p1.hold = data.p1.h || null;
            drawEditorField('p1'); updateNextQueueDisplay('p1');
        }
        if (gameMode === '2P' && data.p2) {
            editorData.p2.board = stringToBoard(data.p2.b);
            editorData.p2.nextQueue = data.p2.n ? data.p2.n.split('') : [];
            editorData.p2.hold = data.p2.h || null;
            drawEditorField('p2'); updateNextQueueDisplay('p2');
        } else if (gameMode !== '2P') {
            editorData.p2.board = Array.from({ length: BOARD_HEIGHT }, () => Array(BOARD_WIDTH).fill(null));
            editorData.p2.nextQueue = [];
            editorData.p2.hold = null;
            if (document.getElementById('p2-editor-col').style.display !== 'none') {
                 drawEditorField('p2'); updateNextQueueDisplay('p2');
            }
        }
        updateScale(); return true;
    } catch (e) {
        console.error('Failed to apply game state:', e); alert('データの読み込みに失敗しました。'); return false;
    }
}

function loadStateFromURL() {
    if (window.location.hash) {
        try {
            const base64Data = window.location.hash.substring(1);
            const binaryString = atob(base64Data);
            const bytes = Uint8Array.from(binaryString, c => c.charCodeAt(0));
            const jsonString = new TextDecoder().decode(bytes);
            const data = JSON.parse(jsonString);

            autoStartParams.ss = !!data.ss;
            autoStartParams.nh = !!data.nh;
            autoStartParams.hb = !!data.hb;

            if (applyGameState(data)) {
                //alert('URLから盤面を読み込みました。');
                if (autoStartParams.ss) {
                    setTimeout(() => document.getElementById('startGameBtn').click(), 100);
                }
            }
            history.pushState("", document.title, window.location.pathname + window.location.search);
        } catch (e) {
            console.error('Failed to load state from URL hash:', e);
            alert('URLからのデータ読み込みに失敗しました。');
            history.pushState("", document.title, window.location.pathname + window.location.search);
        }
    }
}


function generateAndDisplayLink(options = {
    startSim: document.getElementById('start-sim-checkbox').checked,
    noHold: document.getElementById('no-hold-checkbox').checked,
    hideBack: document.getElementById('start-sim-checkbox').checked && document.getElementById('hide-back-btn-checkbox').checked
}) {
    const stateData = getGameStateForExport(options);
    const jsonString = JSON.stringify(stateData);
    const uint8Array = new TextEncoder().encode(jsonString);
    const base64Data = btoa(String.fromCharCode.apply(null, uint8Array));
    const url = new URL(window.location);
    for (const parameter of ['workspace', 'entry', 'practice', 'motion']) url.searchParams.delete(parameter);
    url.hash = base64Data;
    document.getElementById('share-link-input').value = url.href;
}

function openShareModal() {
    generateAndDisplayLink();
    document.getElementById('advanced-link-options').style.display = 'block';
    document.getElementById('share-modal').style.display = 'flex';
}
function gamepadIndexForBinding(binding, defaultIndex) {
    if (!binding || !['pad_button', 'pad_axis'].includes(binding.type)) return null;
    // Older profiles kept the device number only in the displayed label.
    const legacy = /^Pad(\d+)-/.exec(binding.label || '');
    const preferred = Number.isInteger(binding.padIndex) ? binding.padIndex : legacy ? Number(legacy[1]) : defaultIndex;
    const connected = Object.keys(gamepads).map(Number);
    if (binding.padId) {
        if (gamepads[preferred]?.id === binding.padId) return preferred;
        return connected.find(index => gamepads[index].id === binding.padId) ?? null;
    }
    if (gamepads[preferred]) return preferred;
    // Reconnection can change an index. A single connected pad is unambiguous
    // for old profiles; newly registered bindings also remember the device ID.
    return connected.length === 1 ? connected[0] : null;
}

function gamepadBindingPressed(binding, pad) {
    if (!pad || !binding) return false;
    if (binding.type === 'pad_button') return !!pad.buttons[binding.value];
    if (binding.type === 'pad_axis') {
        const axis = /^(\d+)([+-])$/.exec(String(binding.value));
        if (!axis) return false;
        const value = pad.axes[Number(axis[1])] || 0;
        return axis[2] === '+' ? value > AXIS_THRESHOLD : value < -AXIS_THRESHOLD;
    }
    return false;
}

function playerInputSuspended() {
    return isBindingKey || ['settings-modal', 'share-modal', 'rule-modal', 'analysis-modal', 'ai-model-modal']
        .some(id => document.getElementById(id)?.getClientRects().length);
}

let bindingGamepadBaseline = false;
function pollGamepads() {
    let rawPads;
    try { rawPads = navigator.getGamepads?.() || []; }
    catch { gamepads = {}; prevGamepads = {}; return; }
    const previous = gamepads;
    gamepads = {};
    for (const pad of rawPads) {
        if (pad && pad.connected !== false) gamepads[pad.index] = {
            id: pad.id, buttons: Array.from(pad.buttons, b => b.pressed), axes: Array.from(pad.axes)
        };
    }
    prevGamepads = previous;
    if (bindingGamepadBaseline) { bindingGamepadBaseline = false; return; }

    for (const rawIndex of Object.keys(gamepads)) {
        const i = Number(rawIndex), pad = gamepads[i];
        if (isBindingKey) {
            if (prevGamepads[i]) {
                for (let j = 0; j < pad.buttons.length; j++) {
                    if (gamepads[i].buttons[j] && !prevGamepads[i].buttons[j]) {
                        bindKey({ type: 'pad_button', value: j, padIndex: i, padId: pad.id, label: `Pad${i}-Btn${j}` });
                        return;
                    }
                }
                for (let j = 0; j < pad.axes.length; j++) {
                    const val = gamepads[i].axes[j], prevVal = prevGamepads[i].axes[j];
                    if (Math.abs(val) > AXIS_THRESHOLD && Math.abs(prevVal) < AXIS_THRESHOLD) {
                        const dir = val > 0 ? '+' : '-';
                        bindKey({ type: 'pad_axis', value: `${j}${dir}`, padIndex: i, padId: pad.id, label: `Pad${i}-Axis${j}${dir}` });
                        return;
                    }
                }
            }
        } else if (gameState === 'PLAYING' && !playerInputSuspended()) {
             players.forEach(p => {
                if (!p.isAi) {
                    Object.keys(p.keyBindings).forEach(action => {
                        const binding = p.keyBindings[action];
                        if (gamepadIndexForBinding(binding, p.padIndex) === i &&
                            gamepadBindingPressed(binding, pad) && !gamepadBindingPressed(binding, prevGamepads[i])) p.handlePress(action);
                    });
                }
            });
        }
    }
}


let lastAnalysisSample = 0;
function gameLoop(currentTime) {
    if (gameSettings.debugEnabled) {
        const memMon = document.getElementById('memory-monitor');
        if (memMon) {
            if (performance && performance.memory) {
                const used = Math.round(performance.memory.usedJSHeapSize / 1024 / 1024);
                const total = Math.round(performance.memory.totalJSHeapSize / 1024 / 1024);
                memMon.textContent = `Mem: ${used}MB / ${total}MB`;
            } else {
                memMon.textContent = `Mem: N/A`;
            }
        }
    }

    if (!ctx) return;
    pollGamepads();
    const dt = currentTime - lastTime;
lastTime = currentTime;
    ctx.clearRect(0, 0, mainCanvas.width, mainCanvas.height);
    if (gameState === 'PLAYING') {

        flushGarbageDeliveryBatch();
        players.forEach(p => p.update(dt || 0));
        // Gravity/lock-delay placements may have fired during update(). Keep
        // those attacks in the same frame, then expose them before next
        // frame's AI observation.
        flushGarbageDeliveryBatch();
        players.forEach(p => p.draw());

        // 分析用データサンプリング (約200ms毎)
        if (gameSettings.debugEnabled && currentTime - lastAnalysisSample > 200 && players.length > 0) {
            const countBlocks = (board) => {
                let c = 0;
                for(let y=0; y<board.length; y++) for(let x=0; x<board[y].length; x++) if(board[y][x]!==null) c++;
                return c;
            };
            const sample = {
                time: currentTime - gameStartTime,
                type: 'resource',
                p1_R: countBlocks(players[0].board),
                p2_R: players[1] ? countBlocks(players[1].board) : 0
            };
            analysisData.push(sample);
            lastAnalysisSample = currentTime;
        }

    } else if (gameState.startsWith('SCAN')) {
        drawScanner();
    }
    requestAnimationFrame(gameLoop);
}

const mainContainer = document.querySelector('.main-container');

function updateScale() {
    if (gameState.startsWith('SCAN')) {
        mainContainer.style.transform = '';
        return;
    }

    mainContainer.style.transform = 'none';

    const rect = mainContainer.getBoundingClientRect();
    const nativeWidth = rect.width;
    const nativeHeight = rect.height;
    
    if (nativeWidth === 0 || nativeHeight === 0) {
        mainContainer.style.transform = '';
        return;
    }

        const viewportWidth = window.innerWidth;
    const viewportHeight = window.innerHeight;
    const scale = Math.min(
        viewportWidth / nativeWidth,
        viewportHeight / nativeHeight
    ) * 0.98;
    mainContainer.style.transform = `scale(${scale})`;
}

// Hubからのメッセージ受信リスナー (DOMContentLoadedの外に出す)
window.addEventListener('message', (e) => {
    if (e.data && e.data.type === 'loadState') {
        const stateData = e.data.data;
        autoStartParams.ss = false;
        if (typeof applyGameState === 'function' && applyGameState(stateData)) {
             console.log("Applied game state from Hub");
        }
    } else if (e.data && e.data.type === 'requestState') {
        try {
            const stateData = getGameStateForExport();
            window.parent.postMessage({
                target: 'hub',
                type: 'saveSnapshotResponse',
                source: 'sim',
                data: stateData
            }, '*');
        } catch (err) {
            console.error("Export Error:", err);
        }
    }
});
