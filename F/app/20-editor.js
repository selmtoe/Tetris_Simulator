/* Board editor and operation editor. */

function setupEditors() {
    ['p1', 'p2'].forEach(setupPlayerEditor);
}

function setupPlayerEditor(playerId) {
    const palette = document.getElementById(`${playerId}-palette`);
    palette.innerHTML = '';
    Object.keys(EDITOR_COLORS).forEach(key => {
        const swatch = document.createElement('div');
        swatch.className = 'color-swatch';
        swatch.dataset.color = key;
        swatch.style.backgroundColor = key === 'EMPTY' ? '#333' : EDITOR_COLORS[key];
        if (key === 'EMPTY') {
            swatch.style.border = '1px dashed #fff';
            swatch.title = 'Eraser';
        }
        swatch.addEventListener('click', () => {
            fumenPages[currentPageIndex][playerId].activeColor = key;
            palette.querySelector('.active')?.classList.remove('active');
            swatch.classList.add('active');
        });
        palette.appendChild(swatch);
    });

    const canvas = document.getElementById(`field-editor-canvas-${playerId}`);
    canvas.width = BOARD_WIDTH * EDITOR_BLOCK_SIZE;
    canvas.height = BOARD_VISIBLE_HEIGHT * EDITOR_BLOCK_SIZE;
    let isDrawing = false;
    let isEraserMode = false;

    const getCoordsFromEvent = event => {
        const rect = canvas.getBoundingClientRect();
        const clientX = event.touches ? event.touches[0].clientX : event.clientX;
        const clientY = event.touches ? event.touches[0].clientY : event.clientY;
        return {
            x: Math.floor(((clientX - rect.left) / rect.width) * BOARD_WIDTH),
            y: Math.floor(((clientY - rect.top) / rect.height) * BOARD_VISIBLE_HEIGHT)
        };
    };

    const applyDraw = (x, y) => {
        if (x < 0 || x >= BOARD_WIDTH || y < 0 || y >= BOARD_VISIBLE_HEIGHT) return;
        const pageData = fumenPages[currentPageIndex][playerId];
        const boardY = y + pageData.viewY;
        const newColor = isEraserMode ? null : pageData.activeColor;
        if (pageData.board[boardY][x] !== newColor) {
            pageData.board[boardY][x] = newColor;
            drawEditorField(playerId);
        }
    };

    const placeOperationAt = (x, y) => {
        const data = fumenPages[currentPageIndex][playerId];
        const previous = operationForPage(data) || {
            type: normalizePieceType(data.activeColor, 'I'), rotation: 'spawn', x: 4, y: 20
        };
        data.operation = { ...previous, x, y };
        data.activeColor = data.operation.type;
        data.placementMode = false;
        normalizeAllPages();
        updateOperationControls(playerId);
        drawEditorField(playerId);
        updateNextQueueDisplay(playerId);
        pushHistory();
    };

    const handleDrawStart = event => {
        event.preventDefault();
        const { x, y } = getCoordsFromEvent(event);
        const pageData = fumenPages[currentPageIndex][playerId];
        if (pageData.placementMode) {
            placeOperationAt(x, y + pageData.viewY);
            return;
        }
        isDrawing = true;
        const boardY = y + pageData.viewY;
        const currentCellColor = (x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_VISIBLE_HEIGHT)
            ? pageData.board[boardY][x] : null;
        isEraserMode = pageData.activeColor === 'EMPTY' || currentCellColor === pageData.activeColor;
        applyDraw(x, y);
    };

    const handleDrawMove = event => {
        if (!isDrawing) return;
        event.preventDefault();
        const { x, y } = getCoordsFromEvent(event);
        applyDraw(x, y);
    };

    const handleDrawEnd = () => {
        if (!isDrawing) return;
        isDrawing = false;
        pushHistory();
    };

    canvas.addEventListener('mousedown', handleDrawStart);
    canvas.addEventListener('mouseup', handleDrawEnd);
    canvas.addEventListener('mouseleave', handleDrawEnd);
    canvas.addEventListener('mousemove', handleDrawMove);
    canvas.addEventListener('touchstart', handleDrawStart, { passive: false });
    canvas.addEventListener('touchend', handleDrawEnd, { passive: false });
    canvas.addEventListener('touchcancel', handleDrawEnd, { passive: false });
    canvas.addEventListener('touchmove', handleDrawMove, { passive: false });

    document.getElementById(`${playerId}-field-shift-up`).addEventListener('click', () => { shiftField(playerId, 'up'); pushHistory(); });
    document.getElementById(`${playerId}-field-shift-down`).addEventListener('click', () => { shiftField(playerId, 'down'); pushHistory(); });
    document.getElementById(`${playerId}-field-shift-left`).addEventListener('click', () => { shiftField(playerId, 'left'); pushHistory(); });
    document.getElementById(`${playerId}-field-shift-right`).addEventListener('click', () => { shiftField(playerId, 'right'); pushHistory(); });
    document.getElementById(`${playerId}-field-clear`).addEventListener('click', () => {
        fumenPages[currentPageIndex][playerId].board.forEach(row => row.fill(null));
        drawEditorField(playerId);
        pushHistory();
    });

    const nextIcons = document.getElementById(`${playerId}-next-icons`);
    nextIcons.innerHTML = '';
    PIECE_TYPES.forEach(type => {
        const icon = document.createElement('div');
        icon.className = 'mino-icon operation-piece-picker';
        icon.dataset.piece = type;
        icon.style.backgroundColor = EDITOR_COLORS[type];
        icon.title = `${type}をこの手の配置ミノにする`;
        icon.addEventListener('click', () => {
            const data = fumenPages[currentPageIndex][playerId];
            const current = operationForPage(data) || { rotation: 'spawn', x: 4, y: 20 };
            data.operation = { ...current, type };
            data.activeColor = type;
            normalizeAllPages();
            updateOperationControls(playerId);
            drawEditorField(playerId);
            updateNextQueueDisplay(playerId);
            pushHistory();
        });
        nextIcons.appendChild(icon);
    });

    document.getElementById(`${playerId}-operation-rotate-left`).addEventListener('click', () => rotateOperation(playerId, -1));
    document.getElementById(`${playerId}-operation-rotate-right`).addEventListener('click', () => rotateOperation(playerId, 1));
    ['x', 'y'].forEach(axis => {
        document.getElementById(`${playerId}-operation-${axis}`).addEventListener('change', event => {
            const data = fumenPages[currentPageIndex][playerId];
            const current = operationForPage(data) || { type: normalizePieceType(data.activeColor, 'I'), rotation: 'spawn', x: 4, y: 20 };
            const value = Number(event.target.value);
            data.operation = { ...current, [axis]: Number.isFinite(value) ? Math.round(value) : current[axis] };
            normalizeAllPages();
            updateOperationControls(playerId);
            drawEditorField(playerId);
            updateNextQueueDisplay(playerId);
            pushHistory();
        });
    });
    document.getElementById(`${playerId}-operation-hold`).addEventListener('change', event => {
        const data = fumenPages[currentPageIndex][playerId];
        const current = operationForPage(data) || { type: normalizePieceType(data.activeColor, 'I'), rotation: 'spawn', x: 4, y: 20 };
        data.operation = { ...current, hold: event.target.checked };
        normalizeAllPages();
        updateOperationControls(playerId);
        pushHistory();
    });
    document.getElementById(`${playerId}-operation-place-mode`).addEventListener('click', () => {
        const data = fumenPages[currentPageIndex][playerId];
        data.placementMode = !data.placementMode;
        updateOperationControls(playerId);
    });
    document.getElementById(`${playerId}-next-delete-left`).addEventListener('click', () => {
        fumenPages[currentPageIndex][playerId].operation = null;
        fumenPages[currentPageIndex][playerId].placementMode = false;
        normalizeAllPages();
        updateOperationControls(playerId);
        drawEditorField(playerId);
        updateNextQueueDisplay(playerId);
        pushHistory();
    });
    document.getElementById(`${playerId}-next-clear`).addEventListener('click', () => {
        fumenPages[currentPageIndex][playerId].operation = null;
        fumenPages[currentPageIndex][playerId].next = '';
        fumenPages[currentPageIndex][playerId].hold = '';
        normalizeAllPages();
        updateOperationControls(playerId);
        drawEditorField(playerId);
        updateNextQueueDisplay(playerId);
        pushHistory();
    });
}

function rotateOperation(playerId, direction) {
    const data = fumenPages[currentPageIndex][playerId];
    const current = operationForPage(data) || { type: normalizePieceType(data.activeColor, 'I'), x: 4, y: 20, rotation: 'spawn' };
    const index = (operationRotationIndex(current) + direction + 4) % 4;
    data.operation = { ...current, rotation: OPERATION_ROTATIONS[index] };
    normalizeAllPages();
    updateOperationControls(playerId);
    drawEditorField(playerId);
    pushHistory();
}

function updateOperationControls(playerId) {
    const data = fumenPages[currentPageIndex]?.[playerId];
    if (!data) return;
    const operation = operationForPage(data);
    const summary = document.getElementById(`${playerId}-operation-summary`);
    const xInput = document.getElementById(`${playerId}-operation-x`);
    const yInput = document.getElementById(`${playerId}-operation-y`);
    const holdInput = document.getElementById(`${playerId}-operation-hold`);
    const placeButton = document.getElementById(`${playerId}-operation-place-mode`);
    summary.textContent = operation
        ? `${operation.type} / ${operation.rotation} / (${operation.x}, ${operation.y})${operation.hold ? ' / HOLD' : ''}`
        : '未配置（ミノを選択）';
    xInput.value = operation?.x ?? 4;
    yInput.value = operation?.y ?? 20;
    holdInput.checked = Boolean(operation?.hold);
    placeButton.classList.toggle('active', Boolean(data.placementMode));
    placeButton.textContent = data.placementMode ? '盤面をクリック（選択中）' : '盤面クリック配置';
    document.getElementById(`${playerId}-next-icons`).querySelectorAll('[data-piece]').forEach(icon => {
        icon.classList.toggle('active', icon.dataset.piece === operation?.type);
    });
}

function updateNextQueueDisplay(playerId) {
    const queue = document.getElementById(`${playerId}-next-queue`);
    if (!queue) return;
    queue.replaceChildren();
    const data = fumenPages[currentPageIndex]?.[playerId];
    if (!data) return;
    const derived = fumenPages.some(page => operationForPage(page?.[playerId]));
    const label = document.createElement('span');
    label.className = 'next-queue-label';
    label.textContent = derived ? 'NEXT · 配置記録から復元' : 'NEXT · 旧データ';
    queue.appendChild(label);

    const hold = document.createElement('div');
    hold.className = 'next-queue-hold';
    hold.textContent = `H ${data.hold || '—'}`;
    if (data.hold) hold.style.color = EDITOR_COLORS[data.hold];
    queue.appendChild(hold);

    const track = document.createElement('div');
    track.className = 'next-queue-track';
    const next = displayNextForPage(playerId);
    Array.from(next).forEach((type, index) => {
        const item = document.createElement('div');
        item.className = 'next-queue-item';
        item.style.backgroundColor = EDITOR_COLORS[type] || '#333';
        item.title = `NEXT ${index + 1}: ${type}`;
        item.textContent = type;
        track.appendChild(item);
    });
    if (!next.length) {
        const empty = document.createElement('span');
        empty.className = 'next-queue-empty';
        empty.textContent = '配置済みの後続ミノがありません';
        track.appendChild(empty);
    }
    queue.appendChild(track);
    updateOperationControls(playerId);
}

function shiftField(playerId, direction) {
    const board = fumenPages[currentPageIndex][playerId].board;
    if (direction === 'up') { board.shift(); board.push(Array(BOARD_WIDTH).fill(null)); }
    if (direction === 'down') { board.pop(); board.unshift(Array(BOARD_WIDTH).fill(null)); }
    if (direction === 'left') board.forEach(row => { row.shift(); row.push(null); });
    if (direction === 'right') board.forEach(row => { row.pop(); row.unshift(null); });
    drawEditorField(playerId);
}

function drawEditorField(playerId) {
    const canvas = document.getElementById(`field-editor-canvas-${playerId}`);
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const data = fumenPages[currentPageIndex][playerId];
    ctx.fillStyle = '#0f0f18';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.strokeStyle = '#444';
    ctx.lineWidth = 1;
    for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y++) {
        const boardY = y + data.viewY;
        const row = data.board[boardY] || [];
        const isLineClear = row.length === BOARD_WIDTH && row.every(Boolean);
        for (let x = 0; x < BOARD_WIDTH; x++) {
            const pieceType = row[x];
            if (pieceType) {
                ctx.fillStyle = COLORS[pieceType] || '#FFF';
                ctx.fillRect(x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
                if (isLineClear) {
                    ctx.fillStyle = 'rgba(255,255,255,.3)';
                    ctx.fillRect(x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
                }
            }
            ctx.strokeRect(x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
        }
    }

    const operation = operationForPage(data);
    if (operation) {
        for (const [x, y] of operationCells(operation)) {
            const visibleY = y - data.viewY;
            if (x < 0 || x >= BOARD_WIDTH || visibleY < 0 || visibleY >= BOARD_VISIBLE_HEIGHT) continue;
            const px = x * EDITOR_BLOCK_SIZE;
            const py = visibleY * EDITOR_BLOCK_SIZE;
            ctx.fillStyle = COLORS[operation.type] || '#FFF';
            ctx.fillRect(px, py, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
            ctx.fillStyle = 'rgba(255,255,255,.35)';
            ctx.fillRect(px + 3, py + 3, EDITOR_BLOCK_SIZE - 6, EDITOR_BLOCK_SIZE - 6);
            ctx.strokeStyle = '#fff';
            ctx.lineWidth = 3;
            ctx.strokeRect(px + 1, py + 1, EDITOR_BLOCK_SIZE - 2, EDITOR_BLOCK_SIZE - 2);
            ctx.strokeStyle = '#444';
            ctx.lineWidth = 1;
        }
    }
}

function updateScale() {
    const mainContainer = document.querySelector('.main-container');
    if (mainContainer) {
        mainContainer.style.transform = 'none';
        const rect = mainContainer.getBoundingClientRect();
        if (rect.width && rect.height) {
            const scale = Math.min(window.innerWidth / rect.width, window.innerHeight / rect.height) * 0.98;
            mainContainer.style.transform = `scale(${scale})`;
        }
    }
    const viewerControls = document.getElementById('viewer-controls');
    if (viewerControls) {
        viewerControls.style.transform = 'translateX(-50%)';
        const rect = viewerControls.getBoundingClientRect();
        if (rect.width > window.innerWidth * .95) {
            viewerControls.style.transform = `translateX(-50%) scale(${(window.innerWidth * .95) / rect.width})`;
        }
    }
}

function boardToString(board) {
    return board.map(row => row.map(cell => cell === null ? '_' : cell).join('')).join('');
}

function stringToBoard(str) {
    if (!str || str.length !== BOARD_WIDTH * BOARD_HEIGHT) return createEmptyBoard();
    return Array.from({ length: BOARD_HEIGHT }, (_, y) =>
        str.slice(y * BOARD_WIDTH, (y + 1) * BOARD_WIDTH).split('').map(cell => cell === '_' ? null : cell)
    );
}
