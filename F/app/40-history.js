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
 * ランレングス圧縮データを1次元配列にデコードする
 * @param {Array<[string, number]>} rleData - 圧縮データ
 * @returns {Array<string>} 1次元配列 ('_' を含む)
 */
function decodeRLE(rleData) {
    const data = [];
    for (const [value, count] of rleData) {
        for (let i = 0; i < count; i++) {
            data.push(value);
        }
    }
    // 期待される長さ (400) をチェック
    if (data.length !== BOARD_WIDTH * BOARD_HEIGHT) {
        console.warn(`RLE decode length mismatch: ${data.length}. Expected ${BOARD_WIDTH * BOARD_HEIGHT}`);
        // 足りない分を '_' で埋める (フォールバック)
        while (data.length < BOARD_WIDTH * BOARD_HEIGHT) {
            data.push('_');
        }
        // 多い場合は切り詰める
        if (data.length > BOARD_WIDTH * BOARD_HEIGHT) {
            data.splice(BOARD_WIDTH * BOARD_HEIGHT);
        }
    }
    return data;
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
    for (let i = 0; i < prevBoard1D.length; i++) {
        const prev = prevBoard1D[i];
        const curr = currentBoard1D[i];
        if (prev === curr) {
            diff.push('E'); // 'E' (Equal) 変更なし
        } else {
            diff.push(curr); // 変更後の値 (例: '_', 'I', 'O'...)
        }
    }
    return diff;
}

/**
 * 差分データからボードを復元する
 * @param {Array<string>} prevBoard1D - 前のボード (1D, '_'含む)
 * @param {Array<string>} diffData1D - RLEデコード後の差分データ (1D)
 * @returns {Array<string>} 復元されたボード (1D, '_'含む)
 */
function applyDifference(prevBoard1D, diffData1D) {
    const currentBoard1D = [];
    for (let i = 0; i < prevBoard1D.length; i++) {
        const diffVal = diffData1D[i];
        if (diffVal === 'E') {
            currentBoard1D.push(prevBoard1D[i]);
        } else {
            currentBoard1D.push(diffVal);
        }
    }
    return currentBoard1D;
}

const historyPageSnapshots = new WeakMap();
const HISTORY_BOARD_KEY = '__tetrisCompactBoard';

function compactHistoryValue(value, key = '') {
    if (key === 'board') {
        const packed = typeof TetrisEventCodec !== 'undefined'
            ? TetrisEventCodec.packBoard(boardToString(value))
            : boardToString(value);
        return { [HISTORY_BOARD_KEY]: packed };
    }
    if (Array.isArray(value)) return value.map(item => compactHistoryValue(item));
    if (!value || typeof value !== 'object') return value;
    const result = {};
    Object.keys(value).forEach(childKey => {
        result[childKey] = compactHistoryValue(value[childKey], childKey);
    });
    return result;
}

function expandHistoryValue(value, key = '') {
    if (value && typeof value === 'object' && Object.prototype.hasOwnProperty.call(value, HISTORY_BOARD_KEY)) {
        const text = typeof TetrisEventCodec !== 'undefined'
            ? TetrisEventCodec.unpackBoard(value[HISTORY_BOARD_KEY])
            : value[HISTORY_BOARD_KEY];
        return createLazyBoard(text);
    }
    if (Array.isArray(value)) return value.map(item => expandHistoryValue(item));
    if (!value || typeof value !== 'object') return value;
    const result = {};
    Object.keys(value).forEach(childKey => {
        result[childKey] = expandHistoryValue(value[childKey], childKey);
    });
    return result;
}

function markHistoryPageChanged(page = fumenPages[currentPageIndex]) {
    if (page && typeof page === 'object') historyPageSnapshots.delete(page);
}

function markAllHistoryPagesChanged(caseData = fumenCases[currentCaseIndex]) {
    (caseData?.pages || []).forEach(markHistoryPageChanged);
}

function compactHistoryPage(page) {
    const cached = historyPageSnapshots.get(page);
    if (cached) return cached;
    const compact = compactHistoryValue(page);
    historyPageSnapshots.set(page, compact);
    return compact;
}

function compactHistoryCase(caseData) {
    const compact = {};
    Object.keys(caseData || {}).forEach(key => {
        compact[key] = key === 'pages'
            ? (caseData.pages || []).map(compactHistoryPage)
            : compactHistoryValue(caseData[key], key);
    });
    return compact;
}

function expandHistoryCase(caseData) {
    const expanded = {};
    Object.keys(caseData || {}).forEach(key => {
        expanded[key] = key === 'pages'
            ? (caseData.pages || []).map(page => {
                const livePage = expandHistoryValue(page);
                historyPageSnapshots.set(livePage, page);
                return livePage;
            })
            : expandHistoryValue(caseData[key], key);
    });
    return expanded;
}

function captureHistoryState() {
    return {
        cases: fumenCases.map(compactHistoryCase),
        caseIndex: currentCaseIndex,
        idx: currentPageIndex,
        mode: gameMode
    };
}

function pushHistory() {
    if (historyIndex < historyStack.length - 1) {
        historyStack = historyStack.slice(0, historyIndex + 1);
    }
    // Most edit handlers call pushHistory after mutating the current page.
    // Invalidate that page's shared snapshot; untouched pages remain shared
    // between history entries instead of being duplicated up to 50 times.
    markHistoryPageChanged();
    const state = captureHistoryState();
    historyStack.push(state);
    if (historyStack.length > MAX_HISTORY) {
        historyStack.shift();
    } else {
        historyIndex++;
    }
    updateUndoRedoButtons();
}

function undo() {
    if (historyIndex > 0) {
        historyIndex--;
        restoreState(historyStack[historyIndex]);
    }
}

function redo() {
    if (historyIndex < historyStack.length - 1) {
        historyIndex++;
        restoreState(historyStack[historyIndex]);
    }
}

function restoreState(state) {
    if (Array.isArray(state.cases) && state.cases.length) {
        fumenCases = state.cases.map(expandHistoryCase).map(normalizeCase);
        currentCaseIndex = Number.isInteger(state.caseIndex) ? state.caseIndex : 0;
        currentCaseIndex = Math.max(0, Math.min(currentCaseIndex, fumenCases.length - 1));
        fumenPages = fumenCases[currentCaseIndex].pages;
    } else {
        fumenPages = expandHistoryValue(state.pages || []);
        fumenCases = [createCase('Case 1', 'snapshot')];
        fumenCases[0].pages = fumenPages;
        currentCaseIndex = 0;
    }
    currentPageIndex = state.idx;
    gameMode = state.mode;
    
    document.getElementById('mode-1p').classList.toggle('active', gameMode === '1P');
    document.getElementById('mode-2p').classList.toggle('active', gameMode === '2P');
    document.getElementById('p2-editor-col').style.display = (gameMode === '2P') ? 'flex' : 'none';
    
    loadPage(currentPageIndex);
    updateUndoRedoButtons();
    updateScale();
}

function updateUndoRedoButtons() {
    document.getElementById('undo-btn').disabled = (historyIndex <= 0);
    document.getElementById('redo-btn').disabled = (historyIndex >= historyStack.length - 1);
}

// --- ONNX & Image Recognition Logic ---
let onnxSession = null;
