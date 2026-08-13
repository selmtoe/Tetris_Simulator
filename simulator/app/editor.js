/* Board/NEXT editor interactions and canvas rendering. */

function setupEditors() { ['p1', 'p2'].forEach(setupPlayerEditor);

}


function setupPlayerEditor(playerId) {
    editorData[playerId].board = Array.from({ length: BOARD_HEIGHT }, () => Array(BOARD_WIDTH).fill(null));
    const nextIcons = document.getElementById(`${playerId}-next-icons`);
    nextIcons.innerHTML = '';
    Object.keys(TETROMINOS).forEach(key => {
        const icon = document.createElement('div');
icon.className = 'mino-icon';
        if (activeSkin[key] && activeSkin[key].src) {
            icon.style.backgroundImage = `url(${activeSkin[key].src})`;
            icon.style.backgroundSize = 'cover';
        } else {
            icon.style.backgroundColor = activeSkinColors[key];
        }
        icon.addEventListener('click', () => {
            const data = editorData[playerId];
            if (data.nextInsertionIndex === 'hold') {
                data.hold = key;
           
     data.nextInsertionIndex = 0;
            } else if (data.nextInsertionIndex === -1) {
                data.nextQueue.push(key);
                data.nextInsertionIndex = data.nextQueue.length;
            } else {
                data.nextQueue.splice(data.nextInsertionIndex, 0, key);
                data.nextInsertionIndex++;
            }
            updateNextQueueDisplay(playerId);
        });
        nextIcons.appendChild(icon);
    });
    
    const endIcon = document.createElement('div');
    endIcon.className = 'mino-icon';
    if (activeSkin['E'] && activeSkin['E'].src) {
        endIcon.style.backgroundImage = `url(${activeSkin['E'].src})`;
        endIcon.style.backgroundSize = 'cover';
    } else {
        endIcon.style.backgroundColor = activeSkinColors['E'];
    }
    endIcon.style.color = '#FFFFFF';
    endIcon.style.display = 'flex';
    endIcon.style.alignItems = 'center';
endIcon.style.justifyContent = 'center';
    endIcon.style.fontFamily = 'var(--font-display)';
    endIcon.style.fontSize = '14px';
    endIcon.style.fontWeight = 'bold';
endIcon.textContent = 'END';
endIcon.addEventListener('click', () => {
        const data = editorData[playerId];
        if (data.nextInsertionIndex === 'hold' || data.nextQueue.length === 0) {
            return;
        }
        const key = 'E';
        if (data.nextInsertionIndex === -1) {
            data.nextQueue.push(key);
            data.nextInsertionIndex = data.nextQueue.length;
        } else {
            data.nextQueue.splice(data.nextInsertionIndex, 0, key);
            data.nextInsertionIndex++;
        }
        updateNextQueueDisplay(playerId);
    });
    nextIcons.appendChild(endIcon);
    
     document.getElementById(`${playerId}-next-delete-left`).addEventListener('click', () => {
        const data = editorData[playerId];
        const index = data.nextInsertionIndex;
        if (index === 'hold' || (index === -1 && data.nextQueue.length === 0)) {
            return;
        }
        if (index === 0) {
            data.hold = null;
        } else if (index > 0) {
            data.nextQueue.splice(index - 1, 1);
            data.nextInsertionIndex--;
        } else { // index is -1 (end of queue)
            data.nextQueue.pop();
            data.nextInsertionIndex = data.nextQueue.length;
        }
        updateNextQueueDisplay(playerId);
    });
    document.getElementById(`${playerId}-next-clear`).addEventListener('click', () => {
        editorData[playerId].nextQueue = [];
        editorData[playerId].hold = null;
        editorData[playerId].nextInsertionIndex = 0;
        updateNextQueueDisplay(playerId);
    });
    const palette = document.getElementById(`${playerId}-palette`);
    palette.innerHTML = '';

    const autoSwatch = document.createElement('div');
    autoSwatch.className = 'color-swatch';
    autoSwatch.style.display = 'flex';
    autoSwatch.style.alignItems = 'center';
    autoSwatch.style.justifyContent = 'center';
    autoSwatch.style.fontFamily = 'var(--font-display)';
    autoSwatch.style.backgroundColor = '#FFFFFF';
    autoSwatch.style.color = '#000000';
    autoSwatch.style.fontSize = '12px';
    autoSwatch.style.fontWeight = 'bold';
    autoSwatch.textContent = 'AUTO';
    autoSwatch.addEventListener('click', () => {
        editorData[playerId].activeColor = 'AUTO';
        palette.querySelector('.active')?.classList.remove('active');
        autoSwatch.classList.add('active');
    });
palette.appendChild(autoSwatch);

    Object.keys(EDITOR_COLORS).filter(key => key !== 'E').forEach(key => { const swatch = document.createElement('div'); swatch.className = 'color-swatch';
    if (key === 'EMPTY') {
        swatch.style.backgroundColor = '#333';
        swatch.style.border = '1px dashed #fff';
        swatch.title = 'Eraser';
    } else if (activeSkin[key] && activeSkin[key].src) {
        swatch.style.backgroundImage = `url(${activeSkin[key].src})`;
        swatch.style.backgroundSize = 'cover';
    } else {
        swatch.style.backgroundColor = EDITOR_COLORS[key];
    }
    if (key === editorData[playerId].activeColor) swatch.classList.add('active'); swatch.addEventListener('click', () => { editorData[playerId].activeColor = key; palette.querySelector('.active')?.classList.remove('active'); swatch.classList.add('active'); }); palette.appendChild(swatch); });
const canvas = document.getElementById(`field-editor-canvas-${playerId}`);
    
    canvas.width = BOARD_WIDTH * EDITOR_BLOCK_SIZE; canvas.height = BOARD_VISIBLE_HEIGHT * EDITOR_BLOCK_SIZE;
    let isDrawing = false;
let isEraserMode = false;

    function checkForAndReplaceTetromino(playerId) {
        const board = editorData[playerId].board;
        const whiteBlocks = [];
        for (let r = 0; r < BOARD_HEIGHT; r++) {
            for (let c = 0; c < BOARD_WIDTH; c++) {
                if (board[r][c] === 'W') {
                    whiteBlocks.push({ y: r, x: c });
                }
            }
        }

        if (whiteBlocks.length !== 4) return;

        whiteBlocks.sort((a, b) => a.y !== b.y ? a.y - b.y : a.x - b.x);
        const anchor = whiteBlocks[0];

        const relativeCoords = whiteBlocks.slice(1).map(block => `${block.x - anchor.x},${block.y - anchor.y}`);
        const key = relativeCoords.sort().join(';');
        
        const createKey = (coords) => coords.map(c => `${c[0]},${c[1]}`).sort().join(';');

        const shapeMap = {
            [createKey([[1,0],[2,0],[3,0]])]: 'I', [createKey([[0,1],[0,2],[0,3]])]: 'I',
            [createKey([[0,1],[1,0],[1,1]])]: 'O',
            [createKey([[0,1],[1,1],[0,2]])]: 'T', [createKey([[0,1],[0,2],[-1,1]])]: 'T',
            [createKey([[-1,1],[0,1],[1,1]])]: 'T', [createKey([[1,0],[2,0],[1,1]])]: 'T',
            [createKey([[0,1],[1,2],[0,2]])]: 'L', [createKey([[1,0],[1,1],[1,2]])]: 'L',
            [createKey([[-1,1],[0,1],[-2,1]])]: 'L', [createKey([[1,0],[2,0],[0,1]])]: 'L',
            [createKey([[0,1],[-1,2],[0,2]])]: 'J', [createKey([[0,1],[0,2],[1,0]])]: 'J',
            [createKey([[2,1],[0,1],[1,1]])]: 'J', [createKey([[1,0],[2,0],[2,1]])]: 'J',
            [createKey([[0,1],[1,1],[1,2]])]: 'S', [createKey([[1,0],[0,1],[-1,1]])]: 'S',
            [createKey([[-1,1],[0,1],[-1,2]])]: 'Z', [createKey([[1,0],[2,1],[1,1]])]: 'Z',
        };

        const minoType = shapeMap[key];

        if (minoType) {
            whiteBlocks.forEach(block => {
                board[block.y][block.x] = minoType;
            });
        }
    }

    const getCoordsFromEvent = e => {
        const rect = canvas.getBoundingClientRect();
const clientX = e.touches ? e.touches[0].clientX : e.clientX;
        const clientY = e.touches ? e.touches[0].clientY : e.clientY;
const relativeX = (clientX - rect.left) / rect.width;
        const relativeY = (clientY - rect.top) / rect.height;
const x = Math.floor(relativeX * BOARD_WIDTH);
        const y = Math.floor(relativeY * BOARD_VISIBLE_HEIGHT);
        return { x, y };
    };
const applyDraw = (x, y) => {
        if (x < 0 || x >= BOARD_WIDTH || y < 0 || y >= BOARD_VISIBLE_HEIGHT) return;

        const selectedColor = editorData[playerId].activeColor;
        const boardY = y + editorData[playerId].viewY;
        const board = editorData[playerId].board;

        if (selectedColor === 'AUTO') {
            const currentCell = board[boardY][x];
            if (isEraserMode) {
                if (currentCell === 'W') {
                    board[boardY][x] = null;
                }
            } else {
                if (currentCell === null) {
                    const whiteBlockCount = board.flat().filter(cell => cell === 'W').length;
                    if (whiteBlockCount < 4) {
                        board[boardY][x] = 'W';
                        if (whiteBlockCount + 1 === 4) {
                            checkForAndReplaceTetromino(playerId);
                        }
                    }
                }
            }
        } else {
            if (isEraserMode) {
                board[boardY][x] = null;
            } else {
                board[boardY][x] = (selectedColor === 'EMPTY') ? null : selectedColor;
            }
        }
        drawEditorField(playerId);
    };

    const handleDrawStart = e => {
        e.preventDefault();
isDrawing = true;
        const { x, y } = getCoordsFromEvent(e);
        const selectedColor = editorData[playerId].activeColor;

        if (selectedColor === 'AUTO') {
            if (x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_VISIBLE_HEIGHT) {
                const boardY = y + editorData[playerId].viewY;
                const currentCellColor = editorData[playerId].board[boardY][x];
                isEraserMode = (currentCellColor === 'W');
            }
        } else {
            if (x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_VISIBLE_HEIGHT) {
                const boardY = y + editorData[playerId].viewY;
                const currentCellColor = editorData[playerId].board[boardY][x];
                isEraserMode = (selectedColor === 'EMPTY' || currentCellColor === selectedColor);
            }
        }
        applyDraw(x, y);
    };
const handleDrawMove = e => {
        if (!isDrawing) return;
        e.preventDefault();
const { x, y } = getCoordsFromEvent(e);
        applyDraw(x, y);
    };
const handleDrawEnd = () => {
        if (!isDrawing) return;
        isDrawing = false;
isEraserMode = false;
    };

    canvas.addEventListener('mousedown', handleDrawStart);
    canvas.addEventListener('mouseup', handleDrawEnd);
    canvas.addEventListener('mouseleave', handleDrawEnd);
    canvas.addEventListener('mousemove', handleDrawMove);
    canvas.addEventListener('touchstart', handleDrawStart, { passive: false });
    canvas.addEventListener('touchend', handleDrawEnd, { passive: false });
    canvas.addEventListener('touchcancel', handleDrawEnd, { passive: false });
    canvas.addEventListener('touchmove', handleDrawMove, { passive: false });

    
    document.getElementById(`${playerId}-field-shift-up`).addEventListener('click', () => shiftField(playerId, 'up'));
    document.getElementById(`${playerId}-field-shift-down`).addEventListener('click', () => shiftField(playerId, 'down'));
    document.getElementById(`${playerId}-field-clear`).addEventListener('click', () => { editorData[playerId].board.forEach(row => row.fill(null)); drawEditorField(playerId); });
    document.getElementById(`${playerId}-field-line-clear`).addEventListener('click', () => lineClearField(playerId));
    drawEditorField(playerId);
}

function lineClearField(playerId) {
    const data = editorData[playerId];
    const oldBoard = data.board;
    
    // 完全に空(nullのみ)でもなく、完全に埋まって(nullなし)もいない行だけを残す
    const keptRows = oldBoard.filter(row => {
        const isFull = row.every(cell => cell !== null);
        const isEmpty = row.every(cell => cell === null);
        return !isFull && !isEmpty;
    });

    // 新しいボードを作成（上部をnullで埋め、下部に残った行を詰める）
    const newBoard = Array.from({ length: BOARD_HEIGHT }, () => Array(BOARD_WIDTH).fill(null));
    const fillStartIndex = BOARD_HEIGHT - keptRows.length;
    
    for (let i = 0; i < keptRows.length; i++) {
        newBoard[fillStartIndex + i] = keptRows[i];
    }
    
    data.board = newBoard;
    drawEditorField(playerId);
}

function shiftField(playerId, direction) {
    const board = editorData[playerId].board;
    if (direction === 'up') {
        board.shift();
        board.push(Array(BOARD_WIDTH).fill(null));
    } else {
        board.pop();
        board.unshift(Array(BOARD_WIDTH).fill(null));
    }
    drawEditorField(playerId);
}

function updateNextQueueDisplay(playerId) {
    const qd = document.getElementById(`${playerId}-next-queue`);
    qd.innerHTML = '';
    const data = editorData[playerId];

    const createGap = (index) => {
        const gap = document.createElement('div');
        gap.style.width = '8px';
        gap.style.height = '38px';
        gap.style.cursor = 'pointer';
        gap.style.display = 'flex';
        gap.style.alignItems = 'center';
        gap.style.justifyContent = 'center';
        gap.style.userSelect = 'none';
        gap.addEventListener('click', (e) => {
            e.stopPropagation();
            data.nextInsertionIndex = index;
            updateNextQueueDisplay(playerId);
        });
        if (data.nextInsertionIndex === index) {
            gap.style.backgroundColor = 'rgba(255, 255, 255, 0.3)';
            gap.style.borderRadius = '2px';
            gap.innerHTML = '<span style="color: white; font-weight: bold; line-height: 1;">|</span>';
        }
        return gap;
    };

    const holdContainer = document.createElement('div');
    holdContainer.style.display = 'flex';
    holdContainer.style.alignItems = 'center';
    holdContainer.style.gap = '5px';
    holdContainer.style.padding = '0 8px';
    holdContainer.style.borderRight = '2px solid var(--primary-color)';

    const holdSlot = document.createElement('div');
    holdSlot.className = 'mino-icon';
    holdSlot.style.width = '38px';
    holdSlot.style.height = '38px';
    holdSlot.style.cursor = 'pointer';
    holdSlot.style.boxSizing = 'border-box';
    if (data.hold) {
        if (activeSkin[data.hold] && activeSkin[data.hold].src) {
            holdSlot.style.backgroundImage = `url(${activeSkin[data.hold].src})`;
            holdSlot.style.backgroundSize = 'cover';
            holdSlot.style.border = 'none';
        } else {
            holdSlot.style.backgroundColor = activeSkinColors[data.hold];
        }
} else {
        holdSlot.style.backgroundColor = 'transparent';
        holdSlot.style.border = '2px dashed #555';
}
    if (data.nextInsertionIndex === 'hold') {
        holdSlot.style.borderColor = '#FFF';
        holdSlot.style.boxShadow = '0 0 8px #FFF';
    }
    holdSlot.addEventListener('click', (e) => {
        e.stopPropagation();
        data.nextInsertionIndex = 'hold';
        updateNextQueueDisplay(playerId);
    });
    const holdLabel = document.createElement('span');
    holdLabel.textContent = "H";
    holdLabel.style.fontFamily = 'var(--font-display)';
    holdContainer.appendChild(holdLabel);
    holdContainer.appendChild(holdSlot);
    qd.appendChild(holdContainer);
    
    const nextContainer = document.createElement('div');
    nextContainer.style.display = 'flex';
    nextContainer.style.alignItems = 'center';
    nextContainer.style.gap = '5px';
    nextContainer.style.paddingLeft = '8px';
    nextContainer.style.flexWrap = 'nowrap';
    nextContainer.style.minWidth = '0';
    nextContainer.style.overflowX = 'auto';
    nextContainer.style.overflowY = 'hidden';
    nextContainer.style.maxWidth = '100%';

   nextContainer.appendChild(createGap(0));
    data.nextQueue.forEach((key, i) => {
        const icon = document.createElement('div');
        icon.className = 'mino-icon';
        if (activeSkin[key] && activeSkin[key].src) {
            icon.style.backgroundImage = `url(${activeSkin[key].src})`;
            icon.style.backgroundSize = 'cover';
        } else {
            icon.style.backgroundColor = activeSkinColors[key];
        }
        icon.style.width = '38px';
        icon.style.height = '38px';
        if (key === 'E') {
            icon.textContent = 'END';
            icon.style.color = 'white';
      
      icon.style.display = 'flex';
            icon.style.alignItems = 'center';
            icon.style.justifyContent = 'center';
            icon.style.fontFamily = 'var(--font-display)';
            icon.style.fontSize = '12px';
            icon.style.fontWeight = 'bold';
        }
        nextContainer.appendChild(icon);
        nextContainer.appendChild(createGap(i + 1));
    });
    qd.appendChild(nextContainer);

    qd.onclick = (e) => {
        if (e.target === qd || e.target === nextContainer) {
            data.nextInsertionIndex = data.nextQueue.length;
            updateNextQueueDisplay(playerId);
        }
    };
}

function drawEditorField(playerId) {
    const canvas = document.getElementById(`field-editor-canvas-${playerId}`),
        ctx = canvas.getContext('2d'),
        data = editorData[playerId];
if (activeSkin['BG'] && activeSkin['BG'].src) {
        ctx.fillStyle = '#0f0f18';
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        
        for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y++) {
            for (let x = 0; x < BOARD_WIDTH; x++) {
                ctx.drawImage(activeSkin['BG'], x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
            }
        }
} else {
        ctx.fillStyle = '#0f0f18';
        ctx.fillRect(0, 0, canvas.width, canvas.height);
    }
    ctx.strokeStyle = '#444';
ctx.lineWidth = 1;
    for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y++) {
        for (let x = 0; x < BOARD_WIDTH; x++) {
            const pieceType = data.board[y + data.viewY]?.[x];
if (pieceType) {
                if (pieceType === 'W') {
                    ctx.fillStyle = '#FFFFFF';
                    ctx.fillRect(x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
                } else if (activeSkin[pieceType] && activeSkin[pieceType].src) {
                    ctx.drawImage(activeSkin[pieceType], x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
                } else {
                    ctx.fillStyle = activeSkinColors[pieceType];
ctx.fillRect(x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
                }
}
            ctx.strokeRect(x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
}
    }
}

