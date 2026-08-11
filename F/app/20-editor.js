function setupEditors() {
    ['p1', 'p2'].forEach(setupPlayerEditor);
}

function setupPlayerEditor(playerId) {
    const palette = document.getElementById(`${playerId}-palette`);


    Object.keys(EDITOR_COLORS).forEach(key => {
        const swatch = document.createElement('div');
        swatch.className = 'color-swatch';
        swatch.dataset.color = key; // データ属性として色を保持
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
        const pageData = fumenPages[currentPageIndex][playerId];
        const boardY = y + pageData.viewY;
        
        const newColor = isEraserMode ? null : pageData.activeColor;
        if (pageData.board[boardY][x] !== newColor) {
            pageData.board[boardY][x] = newColor;
            drawEditorField(playerId);
        }
    };

    const handleDrawStart = e => {
        e.preventDefault();
        isDrawing = true;


        const { x, y } = getCoordsFromEvent(e);
        const pageData = fumenPages[currentPageIndex][playerId];
        const selectedColor = pageData.activeColor;
        const boardY = y + pageData.viewY;
        const currentCellColor = (x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_VISIBLE_HEIGHT)
            ? pageData.board[boardY][x]
            : null;

        isEraserMode = (selectedColor === 'EMPTY' || currentCellColor === selectedColor);
        
        applyDraw(x, y);
    };

    const handleDrawMove = e => {
        if (!isDrawing) return;
        e.preventDefault();
        const { x, y } = getCoordsFromEvent(e);
        applyDraw(x, y);
    };
    
    const handleDrawEnd = () => {
        if (isDrawing) {
            isDrawing = false;
            pushHistory(); // Save state AFTER drawing
        }
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
// --- ここからNext/HoldエディタUIの構築 (シミュレータから移植・調整) ---
    const nextIcons = document.getElementById(`${playerId}-next-icons`);
    nextIcons.innerHTML = '';
    const minoTypes = ['I', 'O', 'T', 'L', 'J', 'S', 'Z'];
    // 各ミノアイコンの生成
    minoTypes.forEach(key => {
        const icon = document.createElement('div');
        icon.className = 'mino-icon';
        icon.style.backgroundColor = EDITOR_COLORS[key];
        // テキスト削除
        
                icon.addEventListener('click', () => {
            const data = fumenPages[currentPageIndex][playerId];

            let nextArr = data.next ? data.next.split('') : [];
            
            if (data.nextInsertionIndex === 'hold') {
                data.hold = key;
                data.nextInsertionIndex = 0; 
            } else {
                if (data.nextInsertionIndex === -1 || data.nextInsertionIndex >= nextArr.length) {
                    nextArr.push(key);
                    data.nextInsertionIndex = nextArr.length;
                } else {
                    nextArr.splice(data.nextInsertionIndex, 0, key);
                    data.nextInsertionIndex++;
                }
                data.next = nextArr.join('');
            }
            updateNextQueueDisplay(playerId);
            pushHistory(); // Moved to end
        });

        nextIcons.appendChild(icon);
    });

    document.getElementById(`${playerId}-next-delete-left`).addEventListener('click', () => {
        const data = fumenPages[currentPageIndex][playerId];
        let nextArr = data.next ? data.next.split('') : [];
        const index = data.nextInsertionIndex;

        if (index === 'hold') {
            data.hold = '';
        } else if (index === -1) {
            nextArr.pop();
            data.nextInsertionIndex = nextArr.length;
        } else if (index > 0) {
            nextArr.splice(index - 1, 1);
            data.nextInsertionIndex--;
        }
        data.next = nextArr.join('');
        updateNextQueueDisplay(playerId);
        pushHistory(); // Moved to end
    });
    document.getElementById(`${playerId}-next-clear`).addEventListener('click', () => {
        const data = fumenPages[currentPageIndex][playerId];
        data.next = '';
        data.hold = '';
        data.nextInsertionIndex = 0;
        updateNextQueueDisplay(playerId);
        pushHistory(); // Moved to end
    });

}

// 新規追加: Next/Hold表示の更新関数
function updateNextQueueDisplay(playerId) {
    const qd = document.getElementById(`${playerId}-next-queue`);
    if (!qd) return;
    qd.innerHTML = '';
    const data = fumenPages[currentPageIndex][playerId];
    const nextArr = data.next ? data.next.split('') : [];
    
    // カーソル位置の初期化確認
    if (data.nextInsertionIndex === undefined) data.nextInsertionIndex = -1;

// ギャップ(カーソル)生成ヘルパー
    const createGap = (index) => {
        const gap = document.createElement('div');
        gap.style.width = '8px';
        gap.style.height = '38px';
        gap.style.cursor = 'pointer';
        gap.style.display = 'flex';
        gap.style.alignItems = 'center';
        gap.style.justifyContent = 'center';
        
        gap.addEventListener('click', (e) => {
            e.stopPropagation();
            data.nextInsertionIndex = index;
            updateNextQueueDisplay(playerId);
        });

        // アクティブなカーソル表示
        if (data.nextInsertionIndex === index || (index === -1 && data.nextInsertionIndex === nextArr.length)) {
            gap.style.backgroundColor = '#fff';
            gap.style.boxShadow = '0 0 5px #fff';
        } else {
            gap.style.backgroundColor = 'rgba(255,255,255,0.1)';
        }
        return gap;
    };

    // Hold表示エリア
    const holdContainer = document.createElement('div');
    holdContainer.style.display = 'flex';
    holdContainer.style.alignItems = 'center';
    holdContainer.style.gap = '3px';
    holdContainer.style.paddingRight = '8px';
    holdContainer.style.borderRight = '1px solid #555';
    holdContainer.style.marginRight = '5px';

    const holdSlot = document.createElement('div');
    holdSlot.className = 'mino-icon';
    holdSlot.style.width = '38px';
    holdSlot.style.height = '38px';
    holdSlot.style.cursor = 'pointer';
    if (data.hold) {
        holdSlot.style.backgroundColor = EDITOR_COLORS[data.hold] || '#333';
        holdSlot.style.border = 'none';
    } else {
        holdSlot.style.backgroundColor = 'transparent';
        holdSlot.style.border = '2px dashed #555';
    }

    // Hold選択状態
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
    nextContainer.style.gap = '2px';
    nextContainer.style.flexWrap = 'wrap';
    nextContainer.style.flex = '1';

    // 先頭のカーソル (インデックス0)
    nextContainer.appendChild(createGap(0));

    nextArr.forEach((key, i) => {
        const icon = document.createElement('div');
        icon.className = 'mino-icon';
        icon.style.width = '38px';
        icon.style.height = '38px';
        icon.style.backgroundColor = EDITOR_COLORS[key] || '#333';
        
        // アイコンをクリックしたらその右側にカーソル移動
        icon.addEventListener('click', (e) => {

            e.stopPropagation();
            data.nextInsertionIndex = i + 1;
            updateNextQueueDisplay(playerId);
        });

        nextContainer.appendChild(icon);
        nextContainer.appendChild(createGap(i + 1));
    });

    // 末尾クリックで末尾選択
    nextContainer.addEventListener('click', (e) => {
        if (e.target === nextContainer) {
            data.nextInsertionIndex = nextArr.length; // -1と同義だが配列長で管理
            updateNextQueueDisplay(playerId);
        }
    });

    qd.appendChild(nextContainer);
}

function shiftField(playerId, direction) {

    const board = fumenPages[currentPageIndex][playerId].board;
    if (direction === 'up') {
        board.shift();
        board.push(Array(BOARD_WIDTH).fill(null));
    } else if (direction === 'down') {
        board.pop();
        board.unshift(Array(BOARD_WIDTH).fill(null));
    } else if (direction === 'left') {
        board.forEach(row => {
            row.shift();
            row.push(null);
        });
    } else if (direction === 'right') {
        board.forEach(row => {
            row.pop();
            row.unshift(null);
        });
    }
    drawEditorField(playerId);
}

function drawEditorField(playerId) {
    const canvas = document.getElementById(`field-editor-canvas-${playerId}`);
    const ctx = canvas.getContext('2d');
    const data = fumenPages[currentPageIndex][playerId];
    ctx.fillStyle = '#0f0f18';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.strokeStyle = '#444';
ctx.lineWidth = 1;

    for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y++) {
        const boardY = y + data.viewY;
        let isLineClear = true;
        if (data.board[boardY]) {
            for (let xCheck = 0; xCheck < BOARD_WIDTH; xCheck++) {
                if (!data.board[boardY][xCheck]) {
                    isLineClear = false;
                    break;
                }
            }
        } else {
            isLineClear = false;
        }

        for (let x = 0; x < BOARD_WIDTH; x++) {
            const pieceType = data.board[y + data.viewY]?.[x];
if (pieceType) {
                ctx.fillStyle = COLORS[pieceType] ||
'#FFF';
                ctx.fillRect(x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
                
                if (isLineClear) {
                    ctx.fillStyle = 'rgba(255, 255, 255, 0.3)';
                    ctx.fillRect(x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
                }
            }
            ctx.strokeRect(x * EDITOR_BLOCK_SIZE, y * EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE, EDITOR_BLOCK_SIZE);
        }
    }
}

function updateScale() {
    const mainContainer = document.querySelector('.main-container');
    if (mainContainer) {
        mainContainer.style.transform = 'none';
        const rect = mainContainer.getBoundingClientRect();
        if (rect.width !== 0 && rect.height !== 0) {
            const scale = Math.min(
                window.innerWidth / rect.width,
                window.innerHeight / rect.height
            ) * 0.98;
            mainContainer.style.transform = `scale(${scale})`;
        }
    }

    const viewerControls = document.getElementById('viewer-controls');
    if (viewerControls) {
        viewerControls.style.transform = 'translateX(-50%)';
        const vcRect = viewerControls.getBoundingClientRect();
        if (vcRect.width > 0 && vcRect.width > window.innerWidth * 0.95) {
            const vcScale = (window.innerWidth * 0.95) / vcRect.width;
            viewerControls.style.transform = `translateX(-50%) scale(${vcScale})`;
        }
    }
}


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
