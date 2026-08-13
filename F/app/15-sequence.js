/* Operation-first page model shared by the editor, viewer and exporters. */

const PIECE_TYPES = Object.freeze(['I', 'O', 'T', 'L', 'J', 'S', 'Z']);
const OPERATION_ROTATIONS = Object.freeze(['spawn', 'right', 'reverse', 'left']);

function normalizePieceType(value, fallback = '') {
    const piece = String(value || '').toUpperCase();
    return PIECE_TYPES.includes(piece) ? piece : fallback;
}

function normalizeRotation(value) {
    if (typeof value === 'number' && Number.isFinite(value)) return OPERATION_ROTATIONS[((value % 4) + 4) % 4];
    const rotation = String(value || '').toLowerCase();
    return OPERATION_ROTATIONS.includes(rotation) ? rotation : 'spawn';
}

function normalizeOperation(value) {
    if (!value || typeof value !== 'object') return null;
    const type = normalizePieceType(value.type || value.piece);
    if (!type) return null;
    const x = Number(value.x);
    const y = Number(value.y);
    return {
        type,
        rotation: normalizeRotation(value.rotation),
        x: Number.isFinite(x) ? Math.round(x) : 4,
        y: Number.isFinite(y) ? Math.round(y) : 20,
        hold: Boolean(value.hold || value.holdUsed || value.useHold),
        clearedLines: Number.isFinite(Number(value.clearedLines)) ? Math.max(0, Math.round(Number(value.clearedLines))) : undefined
    };
}

function operationRotationIndex(operation) {
    return OPERATION_ROTATIONS.indexOf(normalizeRotation(operation?.rotation));
}

function operationForPage(playerData) {
    return normalizeOperation(playerData?.operation || playerData?.placement);
}

function derivedNextForPage(playerId, pageIndex, pages = fumenPages) {
    const later = [];
    for (let index = pageIndex + 1; index < pages.length; index++) {
        const operation = operationForPage(pages[index]?.[playerId]);
        if (operation) later.push(operation.type);
    }
    return later.join('');
}

function displayNextForPage(playerId, pageIndex = currentPageIndex, pages = fumenPages) {
    const player = pages[pageIndex]?.[playerId];
    if (!player) return '';
    const hasOperations = pages.some(page => operationForPage(page?.[playerId]));
    return hasOperations
        ? derivedNextForPage(playerId, pageIndex, pages)
        : String(player.next || '').replace(/[^IOTLSJZ]/gi, '').toUpperCase();
}

function normalizePlayerData(playerData) {
    if (!playerData) return;
    if (!Array.isArray(playerData.board)) playerData.board = createEmptyBoard();
    playerData.board = Array.from({ length: BOARD_HEIGHT }, (_, y) =>
        Array.from({ length: BOARD_WIDTH }, (_, x) => playerData.board[y]?.[x] || null)
    );
    if (!Number.isFinite(playerData.viewY)) playerData.viewY = BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT;
    if (!PIECE_TYPES.includes(playerData.activeColor)) playerData.activeColor = 'I';
    playerData.hold = normalizePieceType(playerData.hold);
    playerData.operation = operationForPage(playerData);
    if (playerData.nextInsertionIndex === undefined) playerData.nextInsertionIndex = -1;
}

function normalizeAllPages() {
    for (const page of fumenPages) {
        normalizePlayerData(page?.p1);
        normalizePlayerData(page?.p2);
    }
    for (const playerId of ['p1', 'p2']) {
        if (!fumenPages.some(page => operationForPage(page?.[playerId]))) continue;
        for (let index = 0; index < fumenPages.length; index++) {
            const player = fumenPages[index]?.[playerId];
            if (player) player.next = derivedNextForPage(playerId, index);
        }
    }
}

function operationCells(operation) {
    const normalized = normalizeOperation(operation);
    if (!normalized || typeof getShape !== 'function') return [];
    return getShape(normalized.type, operationRotationIndex(normalized))
        .map(([x, y]) => [Math.round(normalized.x + x), Math.round(normalized.y + y)]);
}

function advanceBoardByOperation(board, operation) {
    const result = Array.from({ length: BOARD_HEIGHT }, (_, y) =>
        Array.from({ length: BOARD_WIDTH }, (_, x) => board?.[y]?.[x] || null)
    );
    const normalized = normalizeOperation(operation);
    if (!normalized) return result;
    for (const [x, y] of operationCells(normalized)) {
        if (x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT) result[y][x] = normalized.type;
    }
    const remaining = result.filter(row => !row.every(Boolean));
    while (remaining.length < BOARD_HEIGHT) remaining.unshift(Array(BOARD_WIDTH).fill(null));
    return remaining;
}

function createPageAfterOperation(page) {
    const nextPage = JSON.parse(JSON.stringify(page || createBlankPage()));
    for (const playerId of ['p1', 'p2']) {
        const source = page?.[playerId];
        const target = nextPage[playerId];
        const operation = operationForPage(source);
        if (operation) target.board = advanceBoardByOperation(source.board, operation);
        target.operation = null;
        target.next = '';
        target.nextInsertionIndex = -1;
        target.placementMode = false;
    }
    return nextPage;
}
