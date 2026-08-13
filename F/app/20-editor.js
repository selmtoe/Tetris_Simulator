/* Field editor plus the operation/lock editor. */

let caseControlsReady = false;

function setupEditors() {
    ['p1', 'p2'].forEach(setupPlayerEditor);
    setupCaseControls();
}

function setupCaseControls() {
    if (caseControlsReady) return;
    caseControlsReady = true;
    document.getElementById('case-selector')?.addEventListener('change', event => {
        pushHistory();
        switchCase(Number(event.target.value));
    });
    document.getElementById('new-snapshot-case')?.addEventListener('click', () => addCase('snapshot'));
    document.getElementById('new-replay-case')?.addEventListener('click', () => addCase('replay'));
    document.getElementById('delete-case')?.addEventListener('click', deleteCurrentCase);
    document.getElementById('apply-start-sequence')?.addEventListener('click', () => {
        const p1Sequence = document.getElementById('p1-start-sequence')?.value || '';
        const p2Sequence = document.getElementById('p2-start-sequence')?.value || '';
        const active = currentCase();
        if (!active || active.kind !== 'replay') return;
        setReplaySequence('p1', p1Sequence);
        if (gameMode === '2P') setReplaySequence('p2', p2Sequence);
        updateNextQueueDisplay('p1');
        if (gameMode === '2P') updateNextQueueDisplay('p2');
    });
}

function updateCaseControls() {
    const active = fumenCases[currentCaseIndex];
    if (!active) return;
    const selector = document.getElementById('case-selector');
    if (selector) {
        selector.replaceChildren();
        fumenCases.forEach((caseData, index) => {
            const option = document.createElement('option');
            option.value = index;
            option.textContent = `${index + 1}. ${caseData.name}`;
            option.selected = index === currentCaseIndex;
            selector.appendChild(option);
        });
    }
    const label = document.getElementById('case-mode-label');
    if (label) label.textContent = active.kind === 'replay'
        ? 'Replay: NEXT is derived from the start sequence and locked turns'
        : 'Snapshot: every page is independent';
    const replay = active.kind === 'replay';
    const caseMeta = document.getElementById('case-meta');
    if (caseMeta) caseMeta.style.display = replay ? 'flex' : 'none';
    ['p1-start-sequence', 'p2-start-sequence'].forEach(id => {
        const element = document.getElementById(id);
        if (element) element.parentElement?.classList.toggle('replay-only', !replay);
    });
    const applySequence = document.getElementById('apply-start-sequence');
    if (applySequence) applySequence.style.display = replay ? '' : 'none';
    const p2Sequence = document.getElementById('p2-start-sequence-wrap');
    if (p2Sequence) p2Sequence.style.display = gameMode === '2P' && replay ? 'inline-flex' : 'none';
    const p1Sequence = document.getElementById('p1-start-sequence');
    const p2SequenceInput = document.getElementById('p2-start-sequence');
    if (p1Sequence) p1Sequence.value = active.initial?.p1?.sequence || '';
    if (p2SequenceInput) p2SequenceInput.value = active.initial?.p2?.sequence || '';
}

function setupPlayerEditor(playerId) {
    const palette = document.getElementById(`${playerId}-palette`);
    palette.replaceChildren();
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

    const drawTerrain = (x, y) => {
        if (x < 0 || x >= BOARD_WIDTH || y < 0 || y >= BOARD_VISIBLE_HEIGHT) return;
        const pageData = fumenPages[currentPageIndex][playerId];
        const boardY = y + pageData.viewY;
        const newColor = isEraserMode ? null : pageData.activeColor;
        if (pageData.board[boardY][x] !== newColor) {
            pageData.board[boardY][x] = newColor;
            drawEditorField(playerId);
        }
    };

    const toggleDraftCell = (x, y) => {
        if (x < 0 || x >= BOARD_WIDTH || y < 0 || y >= BOARD_VISIBLE_HEIGHT) return;
        const data = fumenPages[currentPageIndex][playerId];
        const boardY = y + data.viewY;
        const index = data.placementDraft.findIndex(cell => cell[0] === x && cell[1] === boardY);
        if (index >= 0) data.placementDraft.splice(index, 1);
        else if (data.placementDraft.length < 4 && !data.board[boardY][x]) data.placementDraft.push([x, boardY]);
        updatePlacementStatus(playerId);
        drawEditorField(playerId);
    };

    const handleDrawStart = event => {
        event.preventDefault();
        const { x, y } = getCoordsFromEvent(event);
        const pageData = fumenPages[currentPageIndex][playerId];
        if (pageData.placementMode) {
            toggleDraftCell(x, y);
            return;
        }
        isDrawing = true;
        const boardY = y + pageData.viewY;
        const currentCellColor = x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_VISIBLE_HEIGHT
            ? pageData.board[boardY][x] : null;
        isEraserMode = pageData.activeColor === 'EMPTY' || currentCellColor === pageData.activeColor;
        drawTerrain(x, y);
    };

    const handleDrawMove = event => {
        if (!isDrawing || fumenPages[currentPageIndex][playerId].placementMode) return;
        event.preventDefault();
        const { x, y } = getCoordsFromEvent(event);
        drawTerrain(x, y);
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

    document.getElementById(`${playerId}-placement-mode`).addEventListener('click', () => {
        const data = fumenPages[currentPageIndex][playerId];
        data.placementMode = !data.placementMode;
        updatePlacementStatus(playerId);
        drawEditorField(playerId);
    });
    document.getElementById(`${playerId}-placement-clear`).addEventListener('click', () => {
        const data = fumenPages[currentPageIndex][playerId];
        data.placementDraft = [];
        updatePlacementStatus(playerId);
        drawEditorField(playerId);
    });
    document.getElementById(`${playerId}-placement-auto`).addEventListener('click', () => autoLockOperation(playerId));

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
    nextIcons.replaceChildren();
    PIECE_TYPES.forEach(type => {
        const icon = document.createElement('div');
        icon.className = 'mino-icon';
        icon.dataset.piece = type;
        icon.style.backgroundColor = EDITOR_COLORS[type];
        icon.title = `Add ${type} to this snapshot NEXT`;
        icon.addEventListener('click', () => {
            if (currentCaseIsReplay()) return;
            const data = fumenPages[currentPageIndex][playerId];
            const queue = String(data.next || '').split('');
            if (data.nextInsertionIndex < 0 || data.nextInsertionIndex >= queue.length) queue.push(type);
            else queue.splice(data.nextInsertionIndex, 0, type);
            data.next = queue.join('');
            data.nextInsertionIndex = queue.length;
            updateNextQueueDisplay(playerId);
            pushHistory();
        });
        nextIcons.appendChild(icon);
    });
    document.getElementById(`${playerId}-next-delete-left`).addEventListener('click', () => {
        if (currentCaseIsReplay()) return;
        const data = fumenPages[currentPageIndex][playerId];
        const queue = String(data.next || '').split('');
        queue.pop();
        data.next = queue.join('');
        data.nextInsertionIndex = queue.length;
        updateNextQueueDisplay(playerId);
        pushHistory();
    });
    document.getElementById(`${playerId}-next-clear`).addEventListener('click', () => {
        if (currentCaseIsReplay()) return;
        fumenPages[currentPageIndex][playerId].next = '';
        fumenPages[currentPageIndex][playerId].hold = '';
        updateNextQueueDisplay(playerId);
        pushHistory();
    });
}

function ensureReplayCaseForAuto() {
    const active = currentCase();
    if (active.kind === 'replay') return active;
    if (fumenPages.length > 1 && !window.confirm('This is an independent snapshot set. Convert it to a replay case for AUTO lock?')) return null;
    active.kind = 'replay';
    ['p1', 'p2'].forEach(playerId => {
        const first = fumenPages[0][playerId];
        active.initial[playerId].board = cloneBoard(first.board);
        active.initial[playerId].hold = first.hold || '';
        active.initial[playerId].sequence = first.next || '';
    });
    updateCaseControls();
    return active;
}

function autoLockOperation(playerId) {
    const active = ensureReplayCaseForAuto();
    if (!active) return;
    const data = fumenPages[currentPageIndex][playerId];
    if (data.placementDraft.length !== 4) {
        setPlacementStatus(playerId, 'Draw exactly four empty cells first.');
        return;
    }
    const detected = detectOperationFromDraft(data.board, data.placementDraft);
    if (!detected) {
        setPlacementStatus(playerId, 'No legal grounded mino matched these four cells.');
        return;
    }
    if (!active.initial[playerId].sequence) {
        active.initial[playerId].sequence = detected.type + String(data.next || '');
    }
    data.operation = detected;
    data.placementDraft = [];
    data.placementMode = false;
    normalizeReplayCase(active);
    updateCaseControls();
    setPlacementStatus(playerId, `Locked ${detected.type} ${detected.rotation} at (${detected.x}, ${detected.y}).`);
    drawEditorField(playerId);
    updateNextQueueDisplay(playerId);
    pushHistory();
}

function setPlacementStatus(playerId, message = '') {
    const status = document.getElementById(`${playerId}-placement-status`);
    if (status) status.textContent = message;
}

function updatePlacementStatus(playerId) {
    const data = fumenPages[currentPageIndex]?.[playerId];
    if (!data) return;
    const button = document.getElementById(`${playerId}-placement-mode`);
    button.classList.toggle('active', Boolean(data.placementMode));
    button.textContent = data.placementMode ? `接着ミノを指定 (${data.placementDraft.length}/4)` : '接着ミノを4マス指定';
    if (data.placementMode) {
        setPlacementStatus(playerId, `${data.placementDraft.length}/4 cells selected`);
    } else if (!data.operation) {
        setPlacementStatus(playerId, '');
    }
}

// Keep the original icon-based NEXT strip, but let the strip scroll instead
// of wrapping when a replay contains a long recorded queue.
function updateNextQueueDisplay(playerId) {
    const qd = document.getElementById(`${playerId}-next-queue`);
    if (!qd) return;
    qd.replaceChildren();
    const data = fumenPages[currentPageIndex]?.[playerId];
    if (!data) return;
    const nextArr = String(displayNextForPage(playerId) || '').split('').filter(type => EDITOR_COLORS[type]);
    if (data.nextInsertionIndex === undefined) data.nextInsertionIndex = -1;
    const createGap = index => {
        const gap = document.createElement('div');
        gap.style.width = '8px';
        gap.style.height = '38px';
        gap.style.flex = '0 0 8px';
        gap.style.cursor = 'pointer';
        gap.style.backgroundColor = data.nextInsertionIndex === index ||
            (index === -1 && data.nextInsertionIndex === nextArr.length) ? '#fff' : 'rgba(255,255,255,0.1)';
        gap.addEventListener('click', event => {
            event.stopPropagation();
            data.nextInsertionIndex = index;
            updateNextQueueDisplay(playerId);
        });
        return gap;
    };
    const holdContainer = document.createElement('div');
    holdContainer.style.cssText = 'display:flex;align-items:center;gap:3px;flex:0 0 auto;padding-right:8px;border-right:1px solid #555;margin-right:5px;';
    const holdLabel = document.createElement('span');
    holdLabel.textContent = 'H';
    holdLabel.style.fontFamily = 'var(--font-display)';
    const holdSlot = document.createElement('div');
    holdSlot.className = 'mino-icon';
    holdSlot.style.width = '38px';
    holdSlot.style.height = '38px';
    holdSlot.style.cursor = 'pointer';
    holdSlot.style.backgroundColor = data.hold ? (EDITOR_COLORS[data.hold] || '#333') : 'transparent';
    holdSlot.style.border = data.hold ? '2px solid transparent' : '2px dashed #555';
    if (data.nextInsertionIndex === 'hold') holdSlot.style.borderColor = '#fff';
    holdSlot.addEventListener('click', event => {
        event.stopPropagation();
        data.nextInsertionIndex = 'hold';
        updateNextQueueDisplay(playerId);
    });
    holdContainer.append(holdLabel, holdSlot);
    qd.appendChild(holdContainer);
    const track = document.createElement('div');
    track.className = 'next-queue-track';
    track.appendChild(createGap(0));
    nextArr.forEach((type, index) => {
        const icon = document.createElement('div');
        icon.className = 'mino-icon';
        icon.style.width = '38px';
        icon.style.height = '38px';
        icon.style.flex = '0 0 38px';
        icon.style.backgroundColor = EDITOR_COLORS[type] || '#333';
        icon.title = `NEXT ${index + 1}: ${type}`;
        icon.addEventListener('click', event => {
            event.stopPropagation();
            data.nextInsertionIndex = index + 1;
            updateNextQueueDisplay(playerId);
        });
        track.append(icon, createGap(index + 1));
    });
    track.addEventListener('click', event => {
        if (event.target !== track) return;
        data.nextInsertionIndex = nextArr.length;
        updateNextQueueDisplay(playerId);
    });
    qd.appendChild(track);
    updatePlacementStatus(playerId);
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
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y++) {
        const boardY = y + data.viewY;
        const row = data.board[boardY] || [];
        for (let x = 0; x < BOARD_WIDTH; x++) {
            const pieceType = row[x];
            if (pieceType) {
                ctx.fillStyle = COLORS[pieceType] || '#FFF';
                ctx.fillRect(x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
            }
            ctx.strokeStyle = '#333333';
            ctx.lineWidth = 1;
            ctx.strokeRect(x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
        }
    }
    const drawCell = (x, boardY, color, inner = false) => {
        const visibleY = boardY - data.viewY;
        if (x < 0 || x >= BOARD_WIDTH || visibleY < 0 || visibleY >= BOARD_VISIBLE_HEIGHT) return;
        const px = x * EDITOR_BLOCK_SIZE;
        const py = visibleY * EDITOR_BLOCK_SIZE;
        ctx.fillStyle = color;
        ctx.fillRect(px, py, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
        if (inner) {
            ctx.fillStyle = 'rgba(255,255,255,.16)';
            ctx.fillRect(px, py, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
        }
        ctx.strokeStyle = '#333333';
        ctx.lineWidth = 1;
        ctx.strokeRect(px, py, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
    };
    data.placementDraft.forEach(([x, y]) => drawCell(x, y, '#fff', false));
    const operation = operationForPage(data);
    if (operation) operationCells(operation).forEach(([x, y]) => drawCell(x, y, COLORS[operation.type] || '#fff', true));
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
        if (rect.width > window.innerWidth * 0.95) {
            viewerControls.style.transform = `translateX(-50%) scale(${(window.innerWidth * 0.95) / rect.width})`;
        }
    }
}

// Keep the shared resize hook reachable from the legacy helper scripts too.
window.updateScale = updateScale;
