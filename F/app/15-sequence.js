/*
 * Independent case collection and operation/replay helpers.
 *
 * Snapshot cases keep the original page model: every page may describe a
 * completely unrelated position. Replay cases add one initial piece stream
 * and a list of locked operations; NEXT is derived from that case only.
 */

const PIECE_TYPES = ['I', 'O', 'T', 'L', 'J', 'S', 'Z'];
const OPERATION_ROTATIONS = ['spawn', 'right', 'reverse', 'left'];

function normalizePieceType(value, fallback = '') {
    const type = String(value || '').toUpperCase();
    return PIECE_TYPES.includes(type) ? type : fallback;
}

function normalizeRotation(value) {
    if (typeof value === 'number') return OPERATION_ROTATIONS[((value % 4) + 4) % 4];
    const rotation = String(value || '').toLowerCase();
    return OPERATION_ROTATIONS.includes(rotation) ? rotation : 'spawn';
}

function rotationIndex(value) {
    return OPERATION_ROTATIONS.indexOf(normalizeRotation(value));
}

function normalizeOperation(value) {
    if (!value || typeof value !== 'object') return null;
    const type = normalizePieceType(value.type || value.piece);
    if (!type) return null;
    const x = Number(value.x);
    const y = Number(value.y);
    if (!Number.isFinite(x) || !Number.isFinite(y)) return null;
    return {
        type,
        rotation: normalizeRotation(value.rotation),
        x: Math.round(x),
        y: Math.round(y),
        lock: value.lock !== false,
        holdUsed: Boolean(value.holdUsed || value.hold || value.source === 'hold'),
        source: value.source === 'hold' ? 'hold' : 'next',
        coordinateSpace: String(value.coordinateSpace || '')
    };
}

function operationForPage(playerData) {
    // `operation` is the editor's in-memory name; `o` is the compact key
    // used by native video exports and the fumen-compatible payload.
    return normalizeOperation(playerData?.operation || playerData?.o || playerData?.placement);
}

// Older native recovery exports used the candidate generator's anchor for I
// and O. The browser/editor uses the simulator shape table instead. The four
// occupied cells are identical; only the stored anchor differs. New native
// exports carry coordinateSpace=simulator, while an unmarked replay
// operation is treated as the legacy native form during import.
const LEGACY_NATIVE_OPERATION_SHAPES = {
    I: {
        spawn: [[-1, 0], [0, 0], [1, 0], [2, 0]],
        right: [[0, -1], [0, 0], [0, 1], [0, 2]]
    },
    O: { spawn: [[0, 0], [1, 0], [0, 1], [1, 1]] }
};

function canonicalizeLegacyNativeOperation(operation) {
    const normalized = normalizeOperation(operation);
    if (!normalized || normalized.coordinateSpace || typeof getShape !== 'function') return normalized;
    const legacyShape = LEGACY_NATIVE_OPERATION_SHAPES[normalized.type]?.[normalized.rotation];
    if (!legacyShape) return { ...normalized, coordinateSpace: 'simulator' };
    const target = legacyShape.map(([x, y]) => [normalized.x + x, normalized.y + y]);
    const shape = getShape(normalized.type, rotationIndex(normalized.rotation))
        .map(([x, y]) => [Math.round(x), Math.round(y)]);
    for (const [targetX, targetY] of target) {
        for (const [shapeX, shapeY] of shape) {
            const x = targetX - shapeX;
            const y = targetY - shapeY;
            const translated = shape.map(([dx, dy]) => [x + dx, y + dy]);
            if (cellSet(translated) === cellSet(target)) {
                return { ...normalized, x, y, coordinateSpace: 'simulator' };
            }
        }
    }
    return { ...normalized, coordinateSpace: 'simulator' };
}

function normalizeReplayOperationCoordinates(caseData) {
    if (!caseData || caseData.kind !== 'replay') return;
    caseData.pages.forEach(page => {
        ['p1', 'p2'].forEach(playerId => {
            const player = page[playerId];
            if (!player?.operation || player.operation.coordinateSpace) return;
            player.operation = canonicalizeLegacyNativeOperation(player.operation);
        });
    });
}

function cloneBoard(board) {
    return Array.from({ length: BOARD_HEIGHT }, (_, y) =>
        Array.from({ length: BOARD_WIDTH }, (_, x) => board?.[y]?.[x] || null));
}

function clonePage(page) {
    return JSON.parse(JSON.stringify(page || createBlankPage()));
}

function createCase(name = 'Case 1', kind = 'snapshot') {
    const page = createBlankPage();
    return {
        id: `case-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`,
        name,
        kind: kind === 'replay' ? 'replay' : 'snapshot',
        gameMode: '1P',
        initial: {
            p1: { board: cloneBoard(page.p1.board), hold: '', sequence: '' },
            p2: { board: cloneBoard(page.p2.board), hold: '', sequence: '' }
        },
        pages: [page]
    };
}

function normalizeCase(caseData) {
    const normalized = caseData && typeof caseData === 'object' ? caseData : createCase();
    normalized.name = String(normalized.name || 'Case');
    normalized.kind = normalized.kind === 'replay' ? 'replay' : 'snapshot';
    normalized.gameMode = normalized.gameMode === '2P' ? '2P' : '1P';
    normalized.initial = normalized.initial || {};
    ['p1', 'p2'].forEach(playerId => {
        normalized.initial[playerId] = normalized.initial[playerId] || {};
        normalized.initial[playerId].board = cloneBoard(normalized.initial[playerId].board);
        normalized.initial[playerId].hold = normalizePieceType(normalized.initial[playerId].hold);
        normalized.initial[playerId].sequence = String(normalized.initial[playerId].sequence || '')
            .toUpperCase().split('').filter(piece => PIECE_TYPES.includes(piece)).join('');
    });
    normalized.pages = Array.isArray(normalized.pages) && normalized.pages.length
        ? normalized.pages.map(page => {
            const normalizedPage = page || createBlankPage();
            ['p1', 'p2'].forEach(playerId => {
                normalizedPage[playerId] = normalizedPage[playerId] || createBlankPage()[playerId];
                normalizedPage[playerId].board = cloneBoard(normalizedPage[playerId].board);
                normalizedPage[playerId].hold = normalizePieceType(normalizedPage[playerId].hold);
                normalizedPage[playerId].next = String(normalizedPage[playerId].next || '')
                    .toUpperCase().split('').filter(piece => PIECE_TYPES.includes(piece)).join('');
                // Native video replay pages carry the active piece separately
                // (their compact active+queue form is retained in `n`). Keep
                // the property absent for old hand-authored replay cases,
                // which use `next` as visible queue only.
                if (Object.prototype.hasOwnProperty.call(normalizedPage[playerId], 'active')) {
                    normalizedPage[playerId].active = normalizePieceType(normalizedPage[playerId].active);
                }
                normalizedPage[playerId].operation = operationForPage(normalizedPage[playerId]);
                normalizedPage[playerId].placementDraft = Array.isArray(normalizedPage[playerId].placementDraft)
                    ? normalizedPage[playerId].placementDraft.map(cell => [Number(cell[0]), Number(cell[1])])
                        .filter(cell => Number.isFinite(cell[0]) && Number.isFinite(cell[1]))
                    : [];
                normalizedPage[playerId].placementMode = Boolean(normalizedPage[playerId].placementMode);
            });
            return normalizedPage;
        })
        : [createBlankPage()];
    if (normalized.kind === 'replay') {
        normalizeReplayOperationCoordinates(normalized);
        normalizeReplayCase(normalized);
    }
    return normalized;
}

function normalizeActiveCase() {
    if (!fumenCases.length) {
        const initialCase = createCase('Case 1', 'snapshot');
        initialCase.pages = fumenPages.length ? fumenPages : initialCase.pages;
        fumenCases = [initialCase];
        currentCaseIndex = 0;
    }
    currentCaseIndex = Math.max(0, Math.min(currentCaseIndex, fumenCases.length - 1));
    fumenCases[currentCaseIndex] = normalizeCase(fumenCases[currentCaseIndex]);
    fumenCases[currentCaseIndex].pages = fumenPages;
    fumenCases[currentCaseIndex].gameMode = gameMode;
    if (fumenCases[currentCaseIndex].kind === 'replay') normalizeReplayCase(fumenCases[currentCaseIndex]);
    if (typeof updateCaseControls === 'function') updateCaseControls();
}

function currentCase() {
    normalizeActiveCase();
    return fumenCases[currentCaseIndex];
}

function currentCaseIsReplay() {
    return currentCase()?.kind === 'replay';
}

function saveCurrentCase() {
    if (!fumenCases.length) return;
    fumenCases[currentCaseIndex] = normalizeCase(fumenCases[currentCaseIndex]);
    fumenCases[currentCaseIndex].pages = fumenPages;
    fumenCases[currentCaseIndex].gameMode = gameMode;
}

function switchCase(index) {
    saveCurrentCase();
    const nextIndex = Math.max(0, Math.min(Number(index) || 0, fumenCases.length - 1));
    const selected = normalizeCase(fumenCases[nextIndex]);
    fumenCases[nextIndex] = selected;
    currentCaseIndex = nextIndex;
    fumenPages = selected.pages;
    gameMode = selected.gameMode;
    currentPageIndex = 0;
    document.getElementById('mode-1p')?.classList.toggle('active', gameMode === '1P');
    document.getElementById('mode-2p')?.classList.toggle('active', gameMode === '2P');
    const p2 = document.getElementById('p2-editor-col');
    if (p2) p2.style.display = gameMode === '2P' ? 'flex' : 'none';
    loadPage(0);
    updateScale();
}

function addCase(kind = 'snapshot') {
    saveCurrentCase();
    const number = fumenCases.length + 1;
    const newCase = createCase(kind === 'replay' ? `Replay ${number}` : `Case ${number}`, kind);
    fumenCases.push(newCase);
    switchCase(fumenCases.length - 1);
    pushHistory();
}

function deleteCurrentCase() {
    if (fumenCases.length <= 1) return;
    fumenCases.splice(currentCaseIndex, 1);
    currentCaseIndex = Math.min(currentCaseIndex, fumenCases.length - 1);
    fumenPages = normalizeCase(fumenCases[currentCaseIndex]).pages;
    currentPageIndex = 0;
    loadPage(0);
    if (typeof updateCaseControls === 'function') updateCaseControls();
    pushHistory();
}

function operationCells(operation) {
    const normalized = normalizeOperation(operation);
    if (!normalized || typeof getShape !== 'function') return [];
    return getShape(normalized.type, rotationIndex(normalized.rotation))
        .map(([x, y]) => [normalized.x + Math.round(x), normalized.y + Math.round(y)]);
}

const FUMEN_BASE_SHAPES = {
    I: [[0, 0], [-1, 0], [1, 0], [2, 0]],
    O: [[0, 0], [1, 0], [0, 1], [1, 1]],
    T: [[0, 0], [-1, 0], [1, 0], [0, 1]],
    L: [[0, 0], [-1, 0], [1, 0], [1, 1]],
    J: [[0, 0], [-1, 0], [1, 0], [-1, 1]],
    S: [[0, 0], [-1, 0], [0, 1], [1, 1]],
    Z: [[0, 0], [1, 0], [0, 1], [-1, 1]]
};

function fumenShape(type, rotation) {
    let shape = FUMEN_BASE_SHAPES[type]?.map(([x, y]) => [x, y]) || [];
    for (let i = 0; i < rotationIndex(rotation); i++) shape = shape.map(([x, y]) => [y, -x]);
    return shape;
}

function cellSet(cells) {
    return cells.map(([x, y]) => `${Math.round(x)},${Math.round(y)}`).sort().join('|');
}

function findOperationAnchor(type, rotation, targetCells, xHint, yHint) {
    const shape = getShape(type, rotationIndex(rotation)).map(([x, y]) => [Math.round(x), Math.round(y)]);
    const target = cellSet(targetCells);
    for (let y = Math.round(yHint) - 4; y <= Math.round(yHint) + 4; y++) {
        for (let x = Math.round(xHint) - 4; x <= Math.round(xHint) + 4; x++) {
            if (cellSet(shape.map(([dx, dy]) => [x + dx, y + dy])) === target) return { x, y };
        }
    }
    return null;
}

function editorOperationToFumen(operation) {
    const normalized = normalizeOperation(operation);
    if (!normalized) return null;
    const target = operationCells(normalized).map(([x, y]) => [x, 39 - y]);
    const shape = fumenShape(normalized.type, normalized.rotation);
    const targetKey = cellSet(target);
    const hintY = 39 - normalized.y;
    for (let y = hintY - 4; y <= hintY + 4; y++) {
        for (let x = normalized.x - 4; x <= normalized.x + 4; x++) {
            if (cellSet(shape.map(([dx, dy]) => [x + dx, y + dy])) === targetKey) {
                return { ...normalized, x, y };
            }
        }
    }
    return { ...normalized, y: hintY };
}

function fumenOperationToEditor(operation) {
    if (!operation) return null;
    const type = normalizePieceType(operation.type);
    if (!type) return null;
    const rotation = normalizeRotation(operation.rotation);
    const shape = fumenShape(type, rotation);
    const target = shape.map(([x, y]) => [Number(operation.x) + x, 39 - (Number(operation.y) + y)]);
    const anchor = findOperationAnchor(type, rotation, target, Number(operation.x), 39 - Number(operation.y));
    return normalizeOperation({
        ...operation,
        type,
        rotation,
        coordinateSpace: 'simulator',
        x: anchor?.x ?? Number(operation.x),
        y: anchor?.y ?? (39 - Number(operation.y))
    });
}

function canFillOperation(board, operation) {
    const cells = operationCells(operation);
    if (cells.length !== 4) return false;
    return cells.every(([x, y]) => x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT && !board[y][x]);
}

function canLockOperation(board, operation) {
    if (!canFillOperation(board, operation)) return false;
    return !canFillOperation(board, { ...operation, y: operation.y + 1 });
}

function clearLines(board) {
    const kept = board.filter(row => !row.every(Boolean));
    while (kept.length < BOARD_HEIGHT) kept.unshift(Array(BOARD_WIDTH).fill(null));
    return kept;
}

function advanceBoardByOperation(board, operation) {
    const nextBoard = cloneBoard(board);
    for (const [x, y] of operationCells(operation)) {
        if (x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT) nextBoard[y][x] = operation.type;
    }
    return clearLines(nextBoard);
}

function detectOperationFromDraft(board, draft) {
    if (!Array.isArray(draft) || draft.length !== 4) return null;
    const target = draft.map(([x, y]) => `${Math.round(x)},${Math.round(y)}`).sort().join('|');
    const candidates = [];
    PIECE_TYPES.forEach(type => {
        for (let rotation = 0; rotation < 4; rotation++) {
            const shape = getShape(type, rotation).map(([x, y]) => [Math.round(x), Math.round(y)]);
            for (const [anchorX, anchorY] of draft) {
                const [shapeX, shapeY] = shape[0];
                const operation = {
                    type,
                    rotation,
                    x: Math.round(anchorX) - shapeX,
                    y: Math.round(anchorY) - shapeY,
                    coordinateSpace: 'simulator'
                };
                const actual = operationCells(operation).map(([x, y]) => `${x},${y}`).sort().join('|');
                if (actual !== target || !canLockOperation(board, operation)) continue;
                candidates.push({ ...operation, rotation: OPERATION_ROTATIONS[rotation] });
            }
        }
    });
    const unique = candidates.filter((candidate, index, all) =>
        all.findIndex(other => other.type === candidate.type && other.rotation === candidate.rotation &&
            other.x === candidate.x && other.y === candidate.y) === index);
    if (!unique.length) return null;
    // O has identical geometry in all rotations. Prefer spawn in that case.
    return unique.sort((a, b) => rotationIndex(a.rotation) - rotationIndex(b.rotation))[0];
}

function replayStateAtPage(caseData, playerId, pageIndex) {
    const initial = caseData.initial[playerId] || {};
    const sequence = String(initial.sequence || '').split('').filter(piece => PIECE_TYPES.includes(piece));
    const state = { current: sequence.shift() || '', queue: sequence, hold: normalizePieceType(initial.hold) };
    const pages = caseData.pages || [];
    for (let index = 0; index <= pageIndex; index++) {
        const player = pages[index]?.[playerId] || {};
        const operation = operationForPage(player);
        if (Object.prototype.hasOwnProperty.call(player, 'active')) {
            state.current = normalizePieceType(player.active);
            state.queue = String(player.next || '').toUpperCase().split('')
                .filter(piece => PIECE_TYPES.includes(piece));
            state.hold = normalizePieceType(player.hold);
            if (index === pageIndex) return state;
            if (operation) state.current = state.queue.shift() || '';
            continue;
        }
        if (operation?.holdUsed) {
            if (state.hold) {
                const previousCurrent = state.current;
                state.current = state.hold;
                state.hold = previousCurrent;
            } else {
                state.hold = state.current;
                state.current = state.queue.shift() || '';
            }
        }
        if (index === pageIndex) return state;
        if (operation) state.current = state.queue.shift() || '';
    }
    return state;
}

function normalizeReplayCase(caseData) {
    if (!caseData || caseData.kind !== 'replay') return;
    ['p1', 'p2'].forEach(playerId => {
        let state;
        const initial = caseData.initial[playerId] || {};
        const sequence = String(initial.sequence || '').split('').filter(piece => PIECE_TYPES.includes(piece));
        state = { current: sequence.shift() || '', queue: sequence, hold: normalizePieceType(initial.hold) };
        caseData.pages.forEach(page => {
            const player = page[playerId];
            const operation = operationForPage(player);
            if (Object.prototype.hasOwnProperty.call(player, 'active')) {
                // Native video exports provide the already-resolved state for
                // every page.  Use it as the authority: reconstructing HOLD
                // from the global sequence loses a swapped-in HOLD piece and
                // causes the two players' merged timelines to drift.
                state.current = normalizePieceType(player.active);
                state.queue = String(player.next || '').toUpperCase().split('')
                    .filter(piece => PIECE_TYPES.includes(piece));
                state.hold = normalizePieceType(player.hold);
                player.hold = state.hold;
                player.next = state.queue.join('');
                if (operation) state.current = state.queue.shift() || '';
                return;
            }
            if (operation?.holdUsed) {
                if (state.hold) {
                    const previousCurrent = state.current;
                    state.current = state.hold;
                    state.hold = previousCurrent;
                } else {
                    state.hold = state.current;
                    state.current = state.queue.shift() || '';
                }
            }
            player.hold = state.hold;
            player.next = state.queue.join('');
            if (operation) state.current = state.queue.shift() || '';
        });
    });
}

function derivedNextForPage(playerId, pageIndex, pages = fumenPages) {
    const active = currentCase();
    if (!active || active.kind !== 'replay') return String(pages[pageIndex]?.[playerId]?.next || '');
    normalizeReplayCase(active);
    return String(pages[pageIndex]?.[playerId]?.next || '');
}

function displayNextForPage(playerId, pageIndex = currentPageIndex) {
    return currentCaseIsReplay()
        ? derivedNextForPage(playerId, pageIndex)
        : String(fumenPages[pageIndex]?.[playerId]?.next || '');
}

function createPageAfterOperation(page) {
    const nextPage = clonePage(page);
    ['p1', 'p2'].forEach(playerId => {
        const player = nextPage[playerId];
        const operation = operationForPage(page[playerId]);
        if (operation) player.board = advanceBoardByOperation(page[playerId].board, operation);
        player.operation = null;
        player.placementDraft = [];
        player.placementMode = false;
        player.nextInsertionIndex = -1;
    });
    return nextPage;
}

function setReplaySequence(playerId, sequence) {
    const active = currentCase();
    if (!active || active.kind !== 'replay') return;
    active.initial[playerId].sequence = String(sequence || '').toUpperCase()
        .split('').filter(piece => PIECE_TYPES.includes(piece)).join('');
    normalizeReplayCase(active);
    loadPage(currentPageIndex);
}

function collectionData() {
    saveCurrentCase();
    return {
        v: 3,
        m: gameMode,
        currentCase: currentCaseIndex,
        cases: fumenCases.map(caseData => normalizeCase(caseData))
    };
}

function applyCollectionData(data) {
    if (!data || data.v !== 3 || !Array.isArray(data.cases) || !data.cases.length) return false;
    fumenCases = data.cases.map(normalizeCase);
    currentCaseIndex = Math.max(0, Math.min(Number(data.currentCase) || 0, fumenCases.length - 1));
    fumenPages = fumenCases[currentCaseIndex].pages;
    gameMode = fumenCases[currentCaseIndex].gameMode || data.m || '1P';
    currentPageIndex = 0;
    loadPage(0);
    if (typeof updateCaseControls === 'function') updateCaseControls();
    return true;
}

// The native video recovery tool also writes a human-readable analysis JSON.
// Its timeline is intentionally different from the editor's v3 collection,
// but each timeline entry still contains the exact board/queue state and the
// placement that produced the following state.  Convert that file here so a
// user can import either the generated URL or the adjacent .json file.
function recoveryBoardToEditorBoard(value) {
    if (Array.isArray(value) && value.length && Array.isArray(value[0])) {
        const board = createEmptyBoard();
        const sourceRows = value.length === BOARD_VISIBLE_HEIGHT
            ? value.map(row => row)
            : value.slice(0, BOARD_HEIGHT);
        const targetOffset = value.length === BOARD_VISIBLE_HEIGHT
            ? BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT
            : 0;
        sourceRows.forEach((row, sourceY) => {
            const targetY = targetOffset + sourceY;
            if (targetY < 0 || targetY >= BOARD_HEIGHT) return;
            for (let x = 0; x < BOARD_WIDTH; x++) {
                const cell = row?.[x];
                board[targetY][x] = cell && cell !== '_' && cell !== 'E' && cell !== '0'
                    ? String(cell).toUpperCase()
                    : null;
            }
        });
        return board;
    }
    if (Array.isArray(value)) return stringToBoard(value.join(''));
    if (typeof value === 'string') return stringToBoard(value);
    return createEmptyBoard();
}

function recoveryQueueForEntry(entry) {
    const queue = entry?.simulatorNext || `${entry?.active || ''}${entry?.next || ''}`;
    return String(queue).toUpperCase().split('').filter(piece => PIECE_TYPES.includes(piece)).join('');
}

function recoveryOperationForEntry(entry) {
    const placement = entry?.placement || entry?.operation;
    if (!placement || !normalizePieceType(placement.piece || placement.type)) return null;
    return normalizeOperation({
        ...placement,
        type: placement.type || placement.piece,
        holdUsed: Boolean(placement.holdUsed || /hold/i.test(String(entry?.action || '')))
    });
}

function recoverySequence(entries) {
    let sequence = '';
    let previousVisibleQueue = '';
    entries.forEach(entry => {
        const queue = recoveryQueueForEntry(entry);
        if (!queue) return;
        const active = normalizePieceType(entry?.active);
        const visible = active && queue.startsWith(active) ? queue.slice(1) : queue;
        if (!sequence) {
            sequence = active + visible;
            previousVisibleQueue = visible;
            return;
        }
        let overlap = 0;
        const maxOverlap = Math.min(previousVisibleQueue.length, visible.length);
        for (let length = maxOverlap; length > 0; length--) {
            if (previousVisibleQueue.slice(-length) === visible.slice(0, length)) {
                overlap = length;
                break;
            }
        }
        sequence += visible.slice(overlap);
        previousVisibleQueue = visible;
    });
    return sequence;
}

function recoveryPlayerHasContent(entries) {
    return Array.isArray(entries) && entries.some(entry =>
        normalizePieceType(entry?.active) || recoveryOperationForEntry(entry) ||
        recoveryBoardToEditorBoard(entry?.board).some(row => row.some(Boolean)));
}

function recoveryPlayerCaseData(entries) {
    const source = Array.isArray(entries) ? entries : [];
    const first = source[0] || {};
    const pages = source.length ? source.map((entry, index) => {
        const page = createBlankPage();
        const player = page.p1;
        player.board = recoveryBoardToEditorBoard(entry.board || entry.fullBoard);
        player.active = normalizePieceType(entry.active);
        player.hold = normalizePieceType(entry.hold);
        const recoveryQueue = recoveryQueueForEntry(entry);
        player.next = player.active && recoveryQueue.startsWith(player.active)
            ? recoveryQueue.slice(1)
            : recoveryQueue;
        // In the native timeline, placement[i] is the lock that produces
        // board[i]. The page exported by the simulator therefore shows the
        // placement stored on the next timeline state.
        player.operation = index + 1 < source.length
            ? recoveryOperationForEntry(source[index + 1])
            : null;
        return page;
    }) : [createBlankPage()];
    return {
        board: recoveryBoardToEditorBoard(first.board || first.fullBoard),
        hold: normalizePieceType(first.hold),
        sequence: recoverySequence(source),
        pages
    };
}

function applyVideoRecoveryData(data) {
    if (!data || typeof data !== 'object') return false;

    // New native exports embed the exact v3 collection used by the generated
    // simulator URL. Prefer it so operation coordinates and player timing are
    // preserved byte-for-byte.
    const embedded = data.simulatorData || data.collectionData || data.collection;
    if (embedded?.v === 3 && typeof applyCollectionData === 'function') {
        return applyCollectionData(embedded);
    }
    if (data.v === 3 && typeof applyCollectionData === 'function') return applyCollectionData(data);
    if (data.pageFormat !== 'operation-pages/v1' && data.version !== 5) return false;

    const p1 = recoveryPlayerCaseData(data.p1);
    const p2 = recoveryPlayerCaseData(data.p2);
    const isTwoPlayer = data.urls?.combined && data.urls?.p1
        ? data.urls.combined !== data.urls.p1
        : recoveryPlayerHasContent(data.p2);
    const imported = {
        v: 3,
        m: isTwoPlayer ? '2P' : '1P',
        currentCase: 0,
        cases: [{
            id: 'video-recovery-import',
            name: 'Video recovery',
            kind: 'replay',
            gameMode: isTwoPlayer ? '2P' : '1P',
            initial: {
                p1: { board: p1.board, hold: p1.hold, sequence: p1.sequence },
                p2: { board: p2.board, hold: p2.hold, sequence: p2.sequence }
            },
            pages: p1.pages.map((page, index) => {
                if (isTwoPlayer) page.p2 = p2.pages[index]?.p1 || createBlankPage().p1;
                return page;
            })
        }]
    };
    return typeof applyCollectionData === 'function' ? applyCollectionData(imported) : false;
}
