function getShape(p, r) { 
    if(!p) return []; 
    if (!TETROMINOS[p]) {
        console.error('CRITICAL ERROR in getShape: Invalid piece type detected.', 'Value:', p, 'Type:', typeof p);
        throw new Error('Invalid piece type: ' + p);
    }
    const o=TETROMINOS[p].shape; 
    if(r===0||p==='O')return o; 
    const c=TETROMINOS[p].center;
    return o.map(b=>{
        let [x,y]=[b[0]-c[0],b[1]-c[1]]; 
        for(let i=0;i<r;i++){[x,y]=[-y,x];} 
        return [x+c[0]+(p==='O'?0.5:0),y+c[1]+(p==='O'?0.5:0)];
    });
}

function drawViewerBlock(ctx, p, x, y, alpha = 1.0) {
    if(y < -BLOCK_SIZE) return;
    ctx.globalAlpha = alpha;
    ctx.fillStyle = COLORS[p] || '#FFF';
    ctx.fillRect(x, y, BLOCK_SIZE, BLOCK_SIZE);
    ctx.strokeStyle = '#333333';
    ctx.lineWidth = 0.5;
    ctx.strokeRect(x, y, BLOCK_SIZE, BLOCK_SIZE);
    ctx.globalAlpha = 1.0;
}

function drawViewerPiece(ctx, s, x, y, p, alpha) {
    s.forEach(b => {
        const px = Math.floor(x + b[0]) * BLOCK_SIZE;
        const py = (Math.floor(y + b[1]) - (BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT)) * BLOCK_SIZE; 
        drawViewerBlock(ctx, p, px, py, alpha);
    });
}

function drawViewerUI(ctx, playerPageData, offsetX) {
    ctx.save();
    ctx.translate(offsetX, 0);
    
    ctx.textAlign = 'center';
    ctx.fillStyle = '#FFF'; 
    ctx.font = `bold ${BLOCK_SIZE * 0.8}px "Orbitron"`;

    ctx.fillText('HOLD', HOLD_AREA_WIDTH / 2, 40);
    const holdPiece = playerPageData.hold;
    if (holdPiece) { 
        const s = getShape(holdPiece, 0); 
        s.forEach(b => { 
            const px = (HOLD_AREA_WIDTH / 2) - (TETROMINOS[holdPiece].center[0] * BLOCK_SIZE) + (b[0] * BLOCK_SIZE);
            const py = 70 - (TETROMINOS[holdPiece].center[1] * BLOCK_SIZE) + (b[1] * BLOCK_SIZE); 
            drawViewerBlock(ctx, holdPiece, px, py); 
        });
    }

    const rX = PLAYFIELD_X_OFFSET + PLAYFIELD_WIDTH + PADDING + NEXT_AREA_WIDTH / 2;
    ctx.fillStyle = '#FFF';
ctx.fillText('NEXT', rX, 40);
    const nextQueue = (typeof displayNextForPage === 'function'
        ? displayNextForPage(offsetX === 0 ? 'p1' : 'p2')
        : String(playerPageData.next || '')).split('');
    for (let i = 0; i < nextQueue.length; i++) {
        const pT = nextQueue[i];
        if (!pT) continue;
        const s = getShape(pT, 0);
        s.forEach(b => {
            const px = rX - (TETROMINOS[pT].center[0] * BLOCK_SIZE) + (b[0] * BLOCK_SIZE);
            const py = 70 + (i * BLOCK_SIZE * 2.5) - (TETROMINOS[pT].center[1] * BLOCK_SIZE) + (b[1] * BLOCK_SIZE);
            drawViewerBlock(ctx, pT, px, py);
        });
    }
    
    ctx.restore();
}

function drawViewer() {
    if (!viewerCtx) return;
    
    viewerCtx.clearRect(0, 0, viewerCanvas.width, viewerCanvas.height);
    const page = fumenPages[currentPageIndex];
    const playersToDraw = (gameMode === '1P') ? ['p1'] : ['p1', 'p2'];
    
    playersToDraw.forEach((pid, index) => {
        const offsetX = index * PLAYER_CANVAS_WIDTH;
        const playerData = page[pid];
        
        viewerCtx.save();
        viewerCtx.translate(offsetX, 0);

        drawViewerUI(viewerCtx, playerData, index === 0 ? 0 : 1);

        viewerCtx.save();
        viewerCtx.translate(PLAYFIELD_X_OFFSET, 0.5 * BLOCK_SIZE);


        viewerCtx.fillStyle = '#000000';
        viewerCtx.fillRect(0, 0, PLAYFIELD_WIDTH, BOARD_VISIBLE_HEIGHT * BLOCK_SIZE);
        
        viewerCtx.strokeStyle = '#333333';
        viewerCtx.lineWidth = 0.5;
        for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y++) {
            for (let x = 0; x < BOARD_WIDTH; x++) {
      
          viewerCtx.strokeRect(x * BLOCK_SIZE, y * BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE);
            }
        }

        viewerCtx.strokeStyle = '#333333';
        viewerCtx.lineWidth = 1;
        viewerCtx.strokeRect(1, 1, PLAYFIELD_WIDTH - 2, (BOARD_VISIBLE_HEIGHT * BLOCK_SIZE) - 2);

        const viewY = BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT;
        for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y++) {
            const boardY = y + viewY;
            
            let isLineClear = true;
            if (playerData.board[boardY]) {
                for (let xCheck = 0; xCheck < BOARD_WIDTH; xCheck++) {
                    if (!playerData.board[boardY][xCheck]) {
                        isLineClear = false;
                        break;
                    }
                }
            } else {
                isLineClear = false;
            }

            for (let x = 0; x < BOARD_WIDTH; x++) { 
if (playerData.board[boardY]?.[x]) { 
                    const pieceType = playerData.board[boardY][x];
                    const drawX = x * BLOCK_SIZE;
                    const drawY = y * BLOCK_SIZE;
                    
                    viewerCtx.globalAlpha = 1.0;
                    viewerCtx.fillStyle = COLORS[pieceType] || '#FFF';
                    viewerCtx.fillRect(drawX, drawY, BLOCK_SIZE, BLOCK_SIZE);
                    
                    if (isLineClear) {
                        viewerCtx.fillStyle = 'rgba(255, 255, 255, 0.3)';
                        viewerCtx.fillRect(drawX, drawY, BLOCK_SIZE, BLOCK_SIZE);
                    }
                    
                    viewerCtx.strokeStyle = '#333333';
                    viewerCtx.lineWidth = 0.5;
                    viewerCtx.strokeRect(drawX, drawY, BLOCK_SIZE, BLOCK_SIZE);
                    viewerCtx.globalAlpha = 1.0;
} 
            } 
        }

        const operation = typeof operationForPage === 'function' ? operationForPage(playerData) : null;
        if (operation && typeof operationCells === 'function') {
            operationCells(operation).forEach(([x, boardY]) => {
                const visibleY = boardY - viewY;
                if (x < 0 || x >= BOARD_WIDTH || visibleY < 0 || visibleY >= BOARD_VISIBLE_HEIGHT) return;
                const drawX = x * BLOCK_SIZE;
                const drawY = visibleY * BLOCK_SIZE;
                viewerCtx.fillStyle = COLORS[operation.type] || '#fff';
                viewerCtx.fillRect(drawX, drawY, BLOCK_SIZE, BLOCK_SIZE);
                viewerCtx.fillStyle = 'rgba(255,255,255,.16)';
                viewerCtx.fillRect(drawX, drawY, BLOCK_SIZE, BLOCK_SIZE);
                viewerCtx.strokeStyle = '#333333';
                viewerCtx.lineWidth = 0.5;
                viewerCtx.strokeRect(drawX, drawY, BLOCK_SIZE, BLOCK_SIZE);
            });
        }
        
        viewerCtx.restore();
        viewerCtx.restore();
    });
}

function viewerLoop() {
    drawViewer();
    viewerLoopHandle = requestAnimationFrame(viewerLoop);
}

function sendToSimulator() {
    const currentPage = fumenPages[currentPageIndex];

    // A replay case owns the complete starting sequence. Keep that v3
    // collection intact so the simulator receives every remaining NEXT,
    // instead of only the currently visible page's short preview.
    const replayCollection = typeof collectionData === 'function' &&
        typeof currentCaseIsReplay === 'function' && currentCaseIsReplay()
        ? collectionData()
        : null;
    
    const sanitize = (str) => str.replace(/[^IOTLSJZ]/gi, '');
    const p1Hold = sanitize(currentPage.p1.hold || '');
    const p1Next = sanitize(typeof displayNextForPage === 'function' ? displayNextForPage('p1') : currentPage.p1.next || '');
    const p1Operation = typeof operationForPage === 'function' ? operationForPage(currentPage.p1) : null;

    const stateData = replayCollection || {
        v: 2,
        m: gameMode,
        p1: {
            b: boardToString(currentPage.p1.board),
            n: (p1Operation ? p1Operation.type : '') + p1Next,
            h: p1Hold
        },
    };

    if (gameMode === '2P') {
        const p2Hold = sanitize(currentPage.p2.hold || '');
        const p2Next = sanitize(typeof displayNextForPage === 'function' ? displayNextForPage('p2') : currentPage.p2.next || '');
        const p2Operation = typeof operationForPage === 'function' ? operationForPage(currentPage.p2) : null;
        stateData.p2 = {
            b: boardToString(currentPage.p2.board),
            n: (p2Operation ? p2Operation.type : '') + p2Next,
            h: p2Hold
        };
    }

    const jsonString = JSON.stringify(stateData);
    const uint8Array = new TextEncoder().encode(jsonString);
    const base64Data = btoa(String.fromCharCode.apply(null, uint8Array));
    
    let simulatorURL = '../index.html';
    try {
        const path = window.location.pathname;
const parentPath = path.substring(0, path.lastIndexOf('/') + 1);
        simulatorURL = parentPath + '../index.html';
} catch(e) { /* fallback to '../index.html' */ }

    // Hub検知: 親ウィンドウが存在し、自分自身でない場合
    if (window.parent !== window) {
        window.parent.postMessage({
            target: 'sim',
            type: 'loadState',
            data: stateData // JSONオブジェクトをそのまま送信
        }, '*');
    } else {
        location.href = `${simulatorURL}#${base64Data}`;
    }
}


// --- 圧縮ロジック (RLE & 差分) ---

/**
 * 1次元配列をランレングス圧縮する
 * @param {Array<string>} data - 1次元配列 ('_' を含む)
 * @returns {Array<[string, number]>} 圧縮データ [[value, count], ...]
 */
