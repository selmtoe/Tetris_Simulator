/* Player lifecycle, movement, rendering, AI execution, garbage, and custom-rule integration. */

class Player {
    constructor(id, offsetX, keyBindings, padIndex, isAi = false) {
        this.id = id;
        this.offsetX = offsetX; this.keyBindings = keyBindings; this.padIndex = padIndex;
        this.isAi = isAi;
        this.isAiThinking = false;
        this.aiWorker = null;
        this.aiRequestId = 0;
        this.aiSearchInitialized = false;
        
        if (this.isAi) {
            // AIタイプによって読み込むスクリプトを変える
            // Use the reference Cold Clear Standard core compiled to raw WASM.
            // The legacy JS port remains available for compatibility tests.
            this.aiWorker = new Worker('./simulator/workers/cold-clear-wasm-worker.js');

            this.aiWorker.onmessage = (e) => {
                if (e.data && e.data.type === 'debug') {
                    const debugDisplay = document.getElementById('ai-tree-debug-display');
                    if (debugDisplay && gameSettings.debugEnabled) {
                        debugDisplay.dataset.status = e.data.message;
                        const count = debugDisplay.dataset.count || 0;
                        debugDisplay.style.display = 'block';
                        debugDisplay.innerHTML = `Status: <span style="color: ${e.data.message.includes('RESET') ? '#ff4444' : '#44ff44'}">${e.data.message}</span><br>Nodes: ${count}`;
                    }
                } else if (e.data && e.data.type === 'nodeCount') {
                    const debugDisplay = document.getElementById('ai-tree-debug-display');
                    if (debugDisplay && gameSettings.debugEnabled) {
                        debugDisplay.dataset.count = e.data.count;
                        const status = debugDisplay.dataset.status || 'Waiting...';
                        const color = status.includes('RESET') ? '#ff4444' : (status.includes('REUSED') ? '#44ff44' : 'white');
                        debugDisplay.style.display = 'block';
                        debugDisplay.innerHTML = `Status: <span style="color: ${color}">${status}</span><br>Nodes: ${e.data.count}`;
                    }
                } else if (e.data && e.data.type === 'error') {
                    console.error('Cold Clear worker error:', e.data.message);
                    this.aiSearchInitialized = false;
                    this.isAiThinking = false;
                } else if (e.data && e.data.type === 'move') {
                    const isCurrentRequest = e.data.requestId === null || e.data.requestId === undefined || e.data.requestId === this.aiRequestId;
                    if (gameState === 'PLAYING' && this.isAiThinking && isCurrentRequest) {
                        this.executeAiMove(e.data.piece ? e.data : null);
                    }
                } else if (e.data && e.data.piece && gameState === 'PLAYING' && this.isAiThinking) {
                    // Compatibility with the former Worker message shape.
                    this.executeAiMove(e.data);
                }
            };
        }
        
        this.isDrawingOnBoard = false;
        this.drawnBlocks = new Map();
        this.customGhosts = [];
        this.pcGuide = null;

        this.keys = {};
        const pData = editorData[`p${id}`];


        this.initialHold = pData.hold;
        this.board = pData.board.map(row => [...row]);
        this.minoGenerator = createMinoGenerator(pData.nextQueue);
        this.opponent = null;
        this.holdDisabled = false;
        this.ruleWorker = null;
        this.activeRuleHooks = {};
        this.reset();
    }

reset() {
        this.isAiThinking = false;
        this.aiRequestId++;
        this.aiSearchInitialized = false;
        if (this.aiWorker) this.aiWorker.postMessage({ type: 'reset' });
        if (this.id === '1') analysisData = []; // P1リセット時に分析データも初期化
        this.pieceCount = 0;
        this.linesClearedLastLock = 0;
        this.lockUsedHold = false;
        this.player = { x: 0, y: 0, pieceType: null, rotation: 0 };
this.nextQueue = [];
        this.fullMinoSequence = [];
        for (let i = 0; i < gameSettings.maxNext; i++) {
            const newMino = this.minoGenerator.next().value;
            this.nextQueue.push(newMino);
            this.fullMinoSequence.push(newMino);
}
        this.isExecutingSequence = false;
        this.drawnBlocks.clear();
        this.isDrawingOnBoard = false;
        this.customGhosts = [];
        this.pcGuide = null;
this.holdPiece = this.initialHold;this.canHold = true;
this.gravityTimer = gameSettings.gravity; this.lockTimer = 0;

        this.dasTimer = 0; this.arrTimer = 0; this.sdfTimer = 0;
        this.dasDirection = 0;
        this.isGrounded = false; this.gameOver = false; this.isClearingLine = false; this.lineClearDelayTimer = 0;
        this.isSpawning = false; this.spawnDelayTimer = 0;
        this.gameClear = false;


        this.stats = {
            tSpinSingle: 0, tSpinDouble: 0, tSpinTriple: 0,
            miniTSpinSingle: 0, miniTSpinDouble: 0,
            perfectClear: 0, tetris: 0, ren: -1
        };

        this.viewY = BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT;
        this.pendingGarbage = 0;this.garbageQueue = []; this.ren = -1; this.isB2B = false;
        this.lastMoveWasRotation = false;
        this.lastSrsKickIndex = -1;
        this.specialMoveText = [];
        this.specialMoveTextTimer = 0;
        this.customUIText = null;
        this.elapsedTimeText = '0:00.000';
        this.lastGarbageHoleX = -1;
        this.spawnNewPiece();
    }

    
        
    spawnNewPiece() {
        this.player.pieceType = this.nextQueue.shift();
        const newMino = this.minoGenerator.next().value;
        this.nextQueue.push(newMino);
        this.fullMinoSequence.push(newMino);

        // Initial previews are part of the first snapshot.  Every later
        // rolling preview is a real Cold Clear add_next_piece event.
        if (this.isAi && this.aiSearchInitialized && this.aiWorker && ['I', 'O', 'T', 'L', 'J', 'S', 'Z'].includes(newMino)) {
            this.aiWorker.postMessage({ type: 'addNextPiece', piece: newMino });
        }

        
        this.player.rotation = 0;
        this.canHold = true;
        this.lastMoveWasRotation = false;
        this.player.x = Math.floor(BOARD_WIDTH / 2) - Math.floor(TETROMINOS[this.player.pieceType].center[0]) -1;

        const shape = this.getShape(this.player.pieceType, this.player.rotation);
        this.player.y = 20;
        if (this.checkCollision(this.player.x, this.player.y, shape)) {
            this.player.y = 19;
            if (this.checkCollision(this.player.x, this.player.y, shape)) {

                 this.gameOver = true;
            }
        }

        if (window.PCFinder && typeof window.PCFinder.onSpawn === 'function') {
            window.PCFinder.onSpawn(this);
        }
    }

    isActionPressed(action) {
        const binding = this.keyBindings[action];
        if (binding?.type === 'key' && this.keys[binding.value]) return true;
        
        if (this.padIndex !== null && gamepads[this.padIndex]) {
            if (binding?.type === 'pad_button' && gamepads[this.padIndex].buttons[binding.value]) return true;
            if (binding?.type === 'pad_axis') {
                const [axis, dir] = [parseInt(binding.value[0]), binding.value[1]];
                const axisValue = gamepads[this.padIndex].axes[axis];
                if ((dir === '+' && axisValue > AXIS_THRESHOLD) || (dir === '-' && axisValue < -AXIS_THRESHOLD)) return true;
            }
        }
        

        if (this.id === '1' && gameSettings.touchControlsEnabled && gameSettings.touchControlType === 'button') {
            if (virtualController.isButtonPressed(action)) return true;
        }

        return false;
    }
    handlePress(action) {
        if (action === 'retry') { document.getElementById('retryBtn').click(); return;
        }
        if (action === 'exit') { document.getElementById('backToEditorBtn').click(); return;
        }
        if (action === 'pcSearch') {
            if (this.id === '1' && window.PCFinder && typeof window.PCFinder.search === 'function') {
                window.PCFinder.search();
            }
            return;
        }

        if (this.gameOver || this.isClearingLine || this.isSpawning || this.isExecutingSequence) return;
        switch (action) {

            case 'rotateCCW': this.rotate(-1); break;
case 'rotateCW':  this.rotate(1); break;

            case 'hardDrop':  this.hardDrop(); break;
            case 'hold':      this.hold(); break;
}
}
    
update(dt) {
        if (gameStartTime > 0) {
            const totalMilliseconds = performance.now() - gameStartTime;
const minutes = Math.floor(totalMilliseconds / 60000);
            const seconds = Math.floor((totalMilliseconds % 60000) / 1000);
            const milliseconds = Math.floor(totalMilliseconds % 1000);
this.elapsedTimeText = `${minutes}:${String(seconds).padStart(2, '0')}.${String(milliseconds).padStart(3, '0')}`;
        }

        if (this.gameOver || this.gameClear || this.isExecutingSequence) return;
if (this.specialMoveTextTimer > 0) {
            this.specialMoveTextTimer -= dt;
}
    
this.processGarbageQueue();
        const left = this.isActionPressed('left');
        const right = this.isActionPressed('right');

        const horizDir = left ?
-1 : (right ? 1 : 0);
        if (horizDir !== 0) {
            if (this.dasDirection !== horizDir) { 
                this.dasTimer = 0;
this.arrTimer = 0;
                if (!this.isClearingLine && !this.isSpawning) this.move(horizDir, 0);
            } else { 
                this.dasTimer += dt;
if (this.dasTimer >= gameSettings.das) {
                    if (gameSettings.arr === 0) {
                        if (!this.isClearingLine && !this.isSpawning) {
                            let successfulMoves = 0;
while (!this.checkCollision(this.player.x + horizDir, this.player.y, this.getShape(this.player.pieceType, this.player.rotation))) {
                                this.player.x += horizDir;
successfulMoves++;
                            }
                            if (successfulMoves > 0) {
                               this.lockTimer = 0;
this.lastMoveWasRotation = false;
                            }
                        }
                    } else {
                        this.arrTimer += dt;
if (this.arrTimer >= gameSettings.arr) {
                            if (!this.isClearingLine && !this.isSpawning) this.move(horizDir, 0);
this.arrTimer -= gameSettings.arr;
                        }
                    }
                }
            }
        }
        this.dasDirection = horizDir;
if (this.isClearingLine || this.isSpawning) {
            if (this.isClearingLine) {
                this.lineClearDelayTimer -= dt;
            } else {
                this.spawnDelayTimer -= dt;
            }
if ((this.isClearingLine && this.lineClearDelayTimer <= 0) || (this.isSpawning && this.spawnDelayTimer <= 0)) { 
                this.isClearingLine = false;
                this.isSpawning = false;
this.riseGarbage(); 
                if (this.nextQueue[0] === 'E') {
                    if (this.holdPiece) {
                        this.player.pieceType = this.holdPiece;
this.holdPiece = null;
                        this.canHold = false;
                        
                        this.player.rotation = 0;
                        this.lockTimer = 0;
                        this.lastMoveWasRotation = false;
this.player.x = Math.floor(BOARD_WIDTH / 2) - Math.floor(TETROMINOS[this.player.pieceType].center[0]) -1;
                        const shape = this.getShape(this.player.pieceType, this.player.rotation);
                        this.player.y = 20;
if (this.checkCollision(this.player.x, this.player.y, shape)) {
                            this.player.y = 19;
if (this.checkCollision(this.player.x, this.player.y, shape)) {
                                 this.gameOver = true;
}
                        }
                        return;
} else {
                        this.gameOver = true;
return;
                    }
                }
                this.spawnNewPiece();
}
            return;
}

        if (this.isAi) {
            if (!this.isAiThinking) {
                if (gameSettings.pieceForPieceMode && this.opponent && this.pieceCount >= this.opponent.pieceCount) {
                    return;
                }
                this.requestAiMove();
            }
            return;
        }

        if (this.activeRuleHooks.onUpdate) {
            const playerStateProxy = {
                id: this.id,
                stats: this.stats,
            };
this.ruleWorker.postMessage({
                command: 'update',
                data: {
                    playerStateProxy: {
                        id: this.id,
                    
    stats: this.stats,
                        board: this.board,
                        holdPiece: this.holdPiece,
                        currentPiece: this.player.pieceType,
                        nextQueue: this.nextQueue,
                    
    fullMinoSequence: this.fullMinoSequence
                    },
                    is2P: gameMode === '2P'
                }
      });
}
        if (this.gameOver || this.gameClear) return;
if (this.isActionPressed('softDrop')) {
             if (gameSettings.sdf > 0) {
                this.sdfTimer += dt;
                if (this.sdfTimer >= gameSettings.sdf) {
                    this.move(0, 1);
                    this.sdfTimer -= gameSettings.sdf;
                }
             } else { 
                const ghostY = this.getGhostY();
                if (this.player.y < ghostY) {
                    this.player.y = ghostY;
                    this.lockTimer = 0;
                }
             }
        } else {
            this.sdfTimer = 0;
        }
        
        this.isGrounded = this.checkCollision(this.player.x, this.player.y + 1, this.getShape(this.player.pieceType, this.player.rotation));
        if (this.isGrounded) {
            this.lockTimer += dt;
            if (this.lockTimer >= gameSettings.lockDelay) {
                this.lockPiece();
            }
        } else {
            this.lockTimer = 0;
            if (!this.isActionPressed('softDrop')) {
                 if (gameSettings.gravity > 0) {
                    this.gravityTimer -= dt;
                    if (this.gravityTimer <= 0) {
                        this.move(0, 1, false);
                        this.gravityTimer += gameSettings.gravity;
                    }
                 } else { 
                    this.player.y = this.getGhostY();
 }
            }
        }
    }
    draw() {
        const layout = gameSettings.layout;
        const pLayout = (this.id === '1') ? layout.p1 : layout.p2;
        const bSize = layout.blockSize;
        const uiBSize = layout.uiBlockSize || bSize;
        const useCustomBG = !!layout.backgroundImage;
        // UI描画 (Hold, Next)
        this.drawUI(pLayout, uiBSize, useCustomBG);
        // 盤面の描画位置へ移動
        // 従来は translate(this.offsetX + PLAYFIELD_X_OFFSET, 0.5 * BLOCK_SIZE) だったものを
        ctx.save();
        ctx.translate(pLayout.board.x, pLayout.board.y);

        // 背景描画: グローバル背景画像がある場合は、盤面の背景(黒半透明)を描画しないか、あるいは薄くするなどの制御が可能
        // ここでは「背景画像設定時はデフォルトのUI描画(枠線や背景)をスキップする」という要望に沿う形にするが、
        // 盤面が見えなくなるのを防ぐため、グリッド等は残すか、完全にユーザー任せにする。
        // リクエスト通り「背景画像を設定...HoldとかNextなどの描画は全てしなくなる(枠など)」とするため、
        // boardの背景や枠線も useCustomBG が true ならスキップする
        
        if (!useCustomBG) {
            if (activeSkin['BG'] && activeSkin['BG'].src) {
                ctx.fillStyle = 'rgba(0,0,0,0.5)';
                ctx.fillRect(0, 0, BOARD_WIDTH * bSize, BOARD_VISIBLE_HEIGHT * bSize);
                
                for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y++) {
                    for (let x = 0; x < BOARD_WIDTH; x++) {
                        ctx.drawImage(activeSkin['BG'], x * bSize, y * bSize, bSize, bSize);
                    }
                }
            } else {
                ctx.fillStyle = 'rgba(0,0,0,0.5)';
                ctx.fillRect(0, 0, BOARD_WIDTH * bSize, BOARD_VISIBLE_HEIGHT * bSize);
            }
            
            // Grid
            ctx.strokeStyle = '#444';
            ctx.lineWidth = 0.5;
            for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y++) {
                for (let x = 0; x < BOARD_WIDTH; x++) {
                    ctx.strokeRect(x * bSize, y * bSize, bSize, bSize);
                }
            }
            // Border
            ctx.strokeStyle = '#4b4b7c';
            ctx.lineWidth = 2;
            ctx.strokeRect(0, 0, BOARD_WIDTH * bSize, BOARD_VISIBLE_HEIGHT * bSize);
        }

        // Blocks
        for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y++) {
            for (let x = 0; x < BOARD_WIDTH; x++) {
                const boardY = y + this.viewY;
                if (this.board[boardY]?.[x]) {
                    this.drawBlock(this.board[boardY][x], x * bSize, y * bSize, 1.0, bSize);
                }
            }
        }

        const occupiedCells = new Set();
        let shape, ghostY;
        if (!this.gameOver && !this.gameClear && !this.isClearingLine && !this.isSpawning && this.player.pieceType) { 
            shape = this.getShape(this.player.pieceType, this.player.rotation);
            ghostY = this.getGhostY(); 

            if (ghostY > this.player.y) {
                // Ghost
                this.drawPiece(shape, this.player.x, ghostY, this.player.pieceType, 0.4, bSize);
                shape.forEach(b => {
                    const bx = Math.floor(this.player.x + b[0]);
                    const by = Math.floor(ghostY + b[1]);
                    occupiedCells.add(`${bx},${by}`);
                });
            }
            
            // Active Piece
            this.drawPiece(shape, this.player.x, this.player.y, this.player.pieceType, 1.0, bSize);
            shape.forEach(b => {
                const bx = Math.floor(this.player.x + b[0]);
                const by = Math.floor(this.player.y + b[1]);
                occupiedCells.add(`${bx},${by}`);
            });
        }

        // Custom Ghosts
        if (this.customGhosts.length > 0) {
            const ghostMap = new Map();
            this.customGhosts.forEach(ghost => {
                const key = `${ghost.x},${ghost.y}`;
                ghostMap.set(key, ghost.pieceType);
            });
            ghostMap.forEach((pieceType, key) => {
                const [x, y] = key.split(',').map(Number);
                if (occupiedCells.has(key) || (y >= 0 && y < BOARD_HEIGHT && this.board[y][x])) {
                    return;
                }
                const screenY = y - this.viewY;
                if (screenY >= 0 && screenY < BOARD_VISIBLE_HEIGHT) {
                    this.drawBlock(pieceType, x * bSize, screenY * bSize, 0.2, bSize);
                }
            });
        }

        this.drawPcGuide(bSize);

        // Drawn Blocks
        if (this.drawnBlocks.size > 0) {
            ctx.fillStyle = '#FFFFFF';
            ctx.globalAlpha = 0.8;
            for (const key of this.drawnBlocks.keys()) {
                const [x, y] = key.split(',').map(Number);
                const screenY = y - this.viewY;
                if (screenY >= 0 && screenY < BOARD_VISIBLE_HEIGHT) {
                    ctx.fillRect(x * bSize, screenY * bSize, bSize, bSize);
                }
            }
            ctx.globalAlpha = 1.0;
        }
        
        this.drawSpecialMoveText(bSize);
        if (this.gameOver) this.drawMessage("GAME OVER", bSize);
        else if (this.gameClear) this.drawMessage("CLEAR!", bSize);
        ctx.restore();
}
    
    getShape(p, r) { if(!p) return []; const o=TETROMINOS[p].shape; if(r===0||p==='O')return o; const c=TETROMINOS[p].center;return o.map(b=>{let [x,y]=[b[0]-c[0],b[1]-c[1]]; for(let i=0;i<r;i++){[x,y]=[-y,x];} return [x+c[0]+(p==='O'?0.5:0),y+c[1]+(p==='O'?0.5:0)];});}
    checkCollision(x,y,s) { for(const b of s) { const bx=Math.floor(x+b[0]), by=Math.floor(y+b[1]); if(bx<0||bx>=BOARD_WIDTH||by>=BOARD_HEIGHT||(by>=0&&this.board[by]?.[bx])) return true; } return false; }
    rotate(d) { if(this.gameOver||!this.player.pieceType||this.player.pieceType==='O')return; const oR=this.player.rotation, nR=(oR+d+4)%4; const oD=(this.player.pieceType==='I'?SRS_OFFSETS.I:SRS_OFFSETS.JLSTZ)[`${oR}_${nR}`]; const nS=this.getShape(this.player.pieceType,nR); for(let i=0;i<oD.length;i++){const t=oD[i];const oX=t[0],oY=-t[1]; if(!this.checkCollision(this.player.x+oX,this.player.y+oY,nS)){this.player.x+=oX;this.player.y+=oY;this.player.rotation=nR;this.lockTimer=0;this.lastMoveWasRotation=true;this.lastSrsKickIndex=i;return;}}}
    move(dx,dy,isPlayer=true) { if(this.gameOver||!this.player.pieceType)return; const s=this.getShape(this.player.pieceType,this.player.rotation); if(!this.checkCollision(this.player.x+dx,this.player.y+dy,s)){this.player.x+=dx;this.player.y+=dy;if(isPlayer){this.lockTimer=0;this.lastMoveWasRotation=false;}}}
    getGhostY() {if(!this.player.pieceType) return this.player.y; const s=this.getShape(this.player.pieceType,this.player.rotation); let y=this.player.y; while(!this.checkCollision(this.player.x,y+1,s))y++; return y;}
    
    hardDrop() {
        if (this.gameOver) return;
        const originalY = this.player.y;
        this.player.y = this.getGhostY();
        if (this.player.y > originalY) {
            this.lastMoveWasRotation = false;
        }
        this.lockPiece();
    }
    
hold() {
        if (this.gameOver || !this.canHold || this.holdDisabled) return;

        if (!this.holdPiece && this.nextQueue[0] === 'E') {
            return;
        }

        this.canHold = false;
        this.lastMoveWasRotation = false;
        this.lockUsedHold = true;
        if (this.holdPiece) {
            [this.player.pieceType, this.holdPiece] = [this.holdPiece, this.player.pieceType];
            this.player.rotation = 0;
            this.lockTimer = 0;
            
            this.player.x = Math.floor(BOARD_WIDTH / 2) - Math.floor(TETROMINOS[this.player.pieceType].center[0]) -1;
            const shape = this.getShape(this.player.pieceType, this.player.rotation);
            this.player.y = 20;
            if (this.checkCollision(this.player.x, this.player.y, shape)) {
                this.player.y = 19;
                if (this.checkCollision(this.player.x, this.player.y, shape)) {
                     this.gameOver = true;
                }
            }
        } else {
            this.holdPiece = this.player.pieceType;
            // The current piece was stored and the next queue piece was
            // spawned; that new active piece did not come from HOLD.
            this.lockUsedHold = false;
            this.spawnNewPiece();
        }

        if (window.PCFinder && typeof window.PCFinder.onHold === 'function') {
            window.PCFinder.onHold(this);
        }
    }

    lockPiece() {
        if (!this.player.pieceType) return;

        const replayOperation = {
            type: this.player.pieceType,
            rotation: this.player.rotation,
            x: this.player.x,
            y: this.player.y,
            holdBefore: this.holdPiece || '',
            holdUsed: this.lockUsedHold === true,
            boardBefore: this.board.map(row => [...row])
        };
        if (gameState === 'PLAYING' && typeof window.recordReplayLock === 'function') {
            window.recordReplayLock(this, replayOperation);
        }

        if (window.PCFinder && typeof window.PCFinder.onBeforeLock === 'function') {
            window.PCFinder.onBeforeLock(this);
        } else if (window.PCFinder && typeof window.PCFinder.clearForPlayer === 'function') {
            window.PCFinder.clearForPlayer(this);
        } else {
            this.clearPcGuide();
        }
        
        let tspinResult = this.checkForTSpin();

        const shape = this.getShape(this.player.pieceType, this.player.rotation);
        this.specialMoveText = [];
        let moveText = '';

        for (const b of shape) { const bx = Math.floor(this.player.x + b[0]), by = Math.floor(this.player.y + b[1]);
if (by >= 0) this.board[by][bx] = this.player.pieceType; }
        
        if (gameState === 'PLAYING') {
            logCurrentGameState(this);
}

        this.linesClearedLastLock = this.clearLines();
        if (window.PCFinder && typeof window.PCFinder.onAfterLock === 'function') {
            window.PCFinder.onAfterLock(this);
        }
if (gameState === 'PLAYING') {
            if (this.linesClearedLastLock > 0) {
                logCurrentGameState(this);
}
}

const lines = this.linesClearedLastLock;
const isPC = lines > 0 && this.board.every(row => row.every(cell => cell === null));
        
        let attack = 0;
        let isAction = false;
        let moveInfo = { name: '', isPC: false, clearedLines: lines, isB2B: false, ren: this.ren, TSpinType: tspinResult };
if (isPC) {
            attack = 10;
            isAction = true;
this.stats.perfectClear++;
            if (lines === 4) {
                this.stats.tetris++;
}
            moveInfo.isPC = true;
            this.ren++;
            this.isB2B = true;
this.specialMoveText.push("PERFECT CLEAR");
        } else if (lines > 0) {
            isAction = true; this.ren++;
            
            if (tspinResult === 'TSPIN') {
                attack = [0, 2, 4, 6][lines];
                moveText = ['', 'T-Spin Single', 'T-Spin Double', 'T-Spin Triple'][lines];
                moveInfo.name = moveText;
                if (lines === 1) this.stats.tSpinSingle++;
                if (lines === 2) this.stats.tSpinDouble++;
                if (lines === 3) this.stats.tSpinTriple++;
            } else if (tspinResult === 'MINI_TSPIN') {
                attack = [0, 0, 1, 2, 4][lines];
                moveText = ['', 'T-Spin Mini Single', 'T-Spin Mini Double'][lines] || 'T-Spin Mini';
                moveInfo.name = moveText;
                if (lines === 1) this.stats.miniTSpinSingle++;
                if (lines === 2) this.stats.miniTSpinDouble++;
            } else {
                attack = [0, 0, 1, 2, 4][lines];
                moveText = ['', 'Single', 'Double', 'Triple', 'Tetris'][lines];
                moveInfo.name = moveText;
                if (lines === 4) this.stats.tetris++;
            }
            
            const isB2BEligible = (lines === 4) || (tspinResult && lines > 0);
            if (isB2BEligible) {
                if (this.isB2B) {
                    attack++;
                    this.specialMoveText.push("Back-to-Back");
                    moveInfo.isB2B = true;
                }
                this.isB2B = true;
            } else {
                this.isB2B = false;
            }
            const renBonus = [0,0,1,1,2,2,3,3,4,4,4,4,4,5][Math.min(this.ren, 13)];
            if(renBonus > 0) {
                attack += renBonus;
            }
            if (this.ren >= 1) {
                 this.specialMoveText.push(`${this.ren} REN`);
            }
            this.stats.ren = this.ren;
        } else if (tspinResult) {
            isAction = false; 
            this.ren = -1;
            this.isB2B = false; 
            moveText = (tspinResult === 'MINI_TSPIN') ? 'T-Spin Mini' : 'T-Spin';
        }
        
        if (!isAction) {
            this.ren = -1;
            this.stats.ren = -1;
        }
        if (moveText) this.specialMoveText.push(moveText);

        if (gameSettings.debugEnabled && gameStartTime > 0) {
            analysisData.push({
                time: performance.now() - gameStartTime,
                type: 'action',
                playerId: this.id,
                attack: attack,
                lines: lines
            });
        }

        if (this.opponent && attack > 0) {
            let remainingAttack = attack;
            // 相殺量の計算と記録
            let offsetEvent = 0;
            if (this.pendingGarbage > 0) { const offset = Math.min(this.pendingGarbage, remainingAttack);
                offsetEvent += offset;
this.pendingGarbage -= offset; remainingAttack -= offset;
            }
                if (remainingAttack > 0) { for (let i=0; i < this.garbageQueue.length && remainingAttack > 0; i++) { const offset = Math.min(this.garbageQueue[i].lines, remainingAttack);
                offsetEvent += offset;
                this.garbageQueue[i].lines -= offset; remainingAttack -= offset; } this.garbageQueue = this.garbageQueue.filter(g => g.lines > 0);
            }
            // 理論検証用データの記録（相殺が発生した瞬間）
            if (gameSettings.debugEnabled && offsetEvent > 0 && gameStartTime > 0) {
                analysisData.push({
                    time: performance.now() - gameStartTime,
                    type: 'offset',
                    amount: offsetEvent // ライン数 (理論上のリソース減少量は amount * 20)
                });
            }

                if (remainingAttack > 0) { this.opponent.addGarbage(remainingAttack);
            }
}
        
        if (this.activeRuleHooks.onPieceLock) {
            const playerStateProxy = { id: this.id, stats: this.stats };
this.ruleWorker.postMessage({
                command: 'pieceLock',
                data: {
                    playerStateProxy: {
                        id: this.id,
                    
    stats: this.stats,
                        board: this.board,
                        holdPiece: this.holdPiece,
                        currentPiece: this.player.pieceType,
                        nextQueue: this.nextQueue,
                    
    fullMinoSequence: this.fullMinoSequence
                    },
                    moveInfo: moveInfo,
                    is2P: gameMode === '2P'
         }
            });
            return;
}
        
        this.finishLockPiece();
    }
    
        finishLockPiece() {
        this.pieceCount++;
        if (this.gameOver || this.gameClear) return;
        if (this.specialMoveText.length > 0) {
            this.specialMoveTextTimer = 1500;
}

        if (this.nextQueue[0] === 'E') {    
            if (this.holdPiece) {
                this.player.pieceType = this.holdPiece;
this.holdPiece = null;
                this.canHold = false;
                
                this.player.rotation = 0;
                this.lockTimer = 0;
                this.lastMoveWasRotation = false;
this.player.x = Math.floor(BOARD_WIDTH / 2) - Math.floor(TETROMINOS[this.player.pieceType].center[0]) -1;
                const shape = this.getShape(this.player.pieceType, this.player.rotation);
                this.player.y = 20;
if (this.checkCollision(this.player.x, this.player.y, shape)) {
                    this.player.y = 19;
if (this.checkCollision(this.player.x, this.player.y, shape)) {
                         this.gameOver = true;
}
                }
                return;
} else {
                this.gameOver = true;
return;
            }
        }

        this.lockTimer = 0;
if (this.linesClearedLastLock > 0) { this.isClearingLine = true; this.lineClearDelayTimer = gameSettings.lineClearDelay;
} 
        else if (gameSettings.spawnDelay > 0) { this.isSpawning = true; this.spawnDelayTimer = gameSettings.spawnDelay;
}
        else { this.riseGarbage(); this.lockUsedHold = false; this.spawnNewPiece();
}
    }
    
    checkForTSpin() {
        if (this.player.pieceType !== 'T' || !this.lastMoveWasRotation) return null;
        const centerX = this.player.x + TETROMINOS['T'].center[0];
        const centerY = this.player.y + TETROMINOS['T'].center[1];
        const corners = [[-1, -1], [1, -1], [-1, 1], [1, 1]];
        let occupiedCorners = 0;
        corners.forEach(([cx, cy]) => {
            const boardX = Math.round(centerX + cx);
            const boardY = Math.round(centerY + cy);
            if (boardX < 0 || boardX >= BOARD_WIDTH || boardY < 0 || boardY >= BOARD_HEIGHT || this.board[boardY]?.[boardX]) occupiedCorners++;
        });
        if (occupiedCorners < 3) return null;
        const frontCornerDefs = { 0: [[-1, -1], [1, -1]], 1: [[1, -1], [1, 1]], 2: [[-1, 1], [1, 1]], 3: [[-1, -1], [-1, 1]] };
        const frontCorners = frontCornerDefs[this.player.rotation];
        let occupiedFrontCorners = 0;
        frontCorners.forEach(([cx, cy]) => {
            const boardX = Math.round(centerX + cx);
            const boardY = Math.round(centerY + cy);
            if (boardX < 0 || boardX >= BOARD_WIDTH || boardY < 0 || boardY >= BOARD_HEIGHT || this.board[boardY]?.[boardX]) occupiedFrontCorners++;
        });
                if (occupiedFrontCorners === 2 || this.lastSrsKickIndex === 4) return 'TSPIN';
        return 'MINI_TSPIN';
    }

    clearLines() {
        const clearedLinesY = [];
        for (let y = 0; y < this.board.length; y++) {
            if (this.board[y].every(c => c !== null)) {
                clearedLinesY.push(y);
            }
        }

        if (clearedLinesY.length === 0) {
            return 0;
        }

        this.customGhosts = this.customGhosts.filter(ghost => !clearedLinesY.includes(ghost.y));

        this.customGhosts.forEach(ghost => {
            const linesClearedBelow = clearedLinesY.filter(clearedY => clearedY > ghost.y).length;
            if (linesClearedBelow > 0) {
                ghost.y += linesClearedBelow;
            }
        });

        const newBoard = this.board.filter((row, y) => !clearedLinesY.includes(y));
        const clearedCount = clearedLinesY.length;

        for (let i = 0; i < clearedCount; i++) {
            newBoard.unshift(Array(BOARD_WIDTH).fill(null));
        }
        this.board = newBoard;

        return clearedCount;
    }
    addGarbage(lines) { if (this.opponent) { this.garbageQueue.push({ lines, receivedTime: performance.now() });} }
    processGarbageQueue() { const now = performance.now(); for (let i = this.garbageQueue.length - 1; i >= 0; i--) { if (now - this.garbageQueue[i].receivedTime > gameSettings.garbageGrace) { this.pendingGarbage += this.garbageQueue[i].lines; this.garbageQueue.splice(i, 1); } } }
    
    riseGarbage() {
        if (this.pendingGarbage <= 0) return;
        
        if (this.lastGarbageHoleX === -1) {
            this.lastGarbageHoleX = Math.floor(Math.random() * BOARD_WIDTH);
        }
        
        for (let i = 0; i < this.pendingGarbage; i++) {
            if (this.board[0].some(cell => cell !== null)) {
                this.gameOver = true;
                return;
            }
            if (Math.random() < gameSettings.garbageRandomness) {
                this.lastGarbageHoleX = Math.floor(Math.random() * BOARD_WIDTH);
            }
            
            this.board.shift();
            const newRow = Array(BOARD_WIDTH).fill('G');
            newRow[this.lastGarbageHoleX] = null;
            this.board.push(newRow);
        }
        this.pendingGarbage = 0;

}

    drawBlock(p, x, y, alpha = 1.0, bSize = BLOCK_SIZE) {
        if (y < -bSize) return;
        const skinImage = activeSkin[p];
        ctx.globalAlpha = alpha;
        if (skinImage && skinImage.src) {
            ctx.drawImage(skinImage, x, y, bSize, bSize);
        } else {
            ctx.fillStyle = activeSkinColors[p] || '#FFF';
            ctx.fillRect(x, y, bSize, bSize);
        }
        ctx.strokeStyle = '#444';
        ctx.lineWidth = 0.5;
        ctx.strokeRect(x, y, bSize, bSize);
        ctx.globalAlpha = 1.0;
    }

    drawPiece(s, x, y, p, alpha, bSize) {
        s.forEach(b => {
            const px = Math.floor(x + b[0]) * bSize;
            const py = (Math.floor(y + b[1]) - this.viewY) * bSize;
            this.drawBlock(p, px, py, alpha, bSize);
        });
    }

    drawPcGuide(bSize) {
        const guide = this.pcGuide;
        if (!guide || !Array.isArray(guide.cells) || guide.cells.length === 0) return;

        const color = activeSkinColors[guide.pieceType] || '#ffffff';
        ctx.save();
        ctx.lineWidth = Math.max(2, Math.round(bSize * 0.09));
        ctx.setLineDash([Math.max(3, Math.round(bSize * 0.18)), Math.max(2, Math.round(bSize * 0.11))]);

        for (const cell of guide.cells) {
            const x = Number(cell.x);
            const y = Number(cell.y);
            const screenY = y - this.viewY;
            if (!Number.isInteger(x) || !Number.isInteger(y) || x < 0 || x >= BOARD_WIDTH ||
                screenY < 0 || screenY >= BOARD_VISIBLE_HEIGHT) {
                continue;
            }

            const px = x * bSize;
            const py = screenY * bSize;
            ctx.globalAlpha = 0.14;
            ctx.fillStyle = color;
            ctx.fillRect(px + 1, py + 1, bSize - 2, bSize - 2);
            ctx.globalAlpha = 0.95;
            ctx.strokeStyle = color;
            ctx.strokeRect(px + 2, py + 2, bSize - 4, bSize - 4);
        }

        ctx.restore();
    }

    drawUI(layout, bSize, useCustomBG) {
        ctx.textAlign = 'center';
        ctx.fillStyle = '#FFF'; 
        ctx.font = `bold ${bSize * 0.8}px "Orbitron"`;

        // HOLD
        if (!this.holdDisabled) {
            if (!useCustomBG) ctx.fillText('HOLD', layout.hold.x, layout.hold.y - bSize);
            if (this.holdPiece) {
                const s = this.getShape(this.holdPiece, 0);
                s.forEach(b => {
                    // 中心合わせ
                    const cx = layout.hold.x;
                    const cy = layout.hold.y;
                    const px = cx - (TETROMINOS[this.holdPiece].center[0] * bSize) + (b[0] * bSize);
                    const py = cy - (TETROMINOS[this.holdPiece].center[1] * bSize) + (b[1] * bSize);
                    this.drawBlock(this.holdPiece, px, py, 1.0, bSize);
                });
            }
        }

        // Garbage Meter (盤面の左横)
        // カスタム背景時は描画しない、または位置調整が必要だが、ここでは標準位置(盤面左)依存とする
        if (!useCustomBG) {
            const meterX = layout.board.x - 12;
            const meterWidth = 8; 
            const meterMaxHeight = BOARD_VISIBLE_HEIGHT * bSize;
            const pendingHeight = Math.min(this.pendingGarbage, BOARD_VISIBLE_HEIGHT) * bSize;
            
            if (pendingHeight > 0) { 
                ctx.fillStyle = 'red'; 
                ctx.fillRect(meterX, layout.board.y + meterMaxHeight - pendingHeight, meterWidth, pendingHeight);
            }
            const queuedLines = this.garbageQueue.reduce((sum, g) => sum + g.lines, 0);
            const queuedHeight = Math.min(queuedLines, BOARD_VISIBLE_HEIGHT - this.pendingGarbage) * bSize;
            if (queuedHeight > 0) { 
                ctx.fillStyle = 'yellow';
                ctx.fillRect(meterX, layout.board.y + meterMaxHeight - pendingHeight - queuedHeight, meterWidth, queuedHeight);
            }
        }

        // Custom UI Text
        if (this.customUIText !== null) {
            ctx.font = `bold ${bSize * 1.6}px "Orbitron"`;
            ctx.fillStyle = '#FFFFFF';
            ctx.fillText(this.customUIText, layout.board.x + (BOARD_WIDTH * bSize)/2, layout.board.y + 150);
        }

        // Timer
        if (gameSettings.showTimer && !useCustomBG) {
            ctx.fillStyle = '#FFFFFF';
            ctx.font = `bold ${bSize * 0.8}px "Orbitron"`;
            const charSpacing = bSize * 0.6;
            const totalWidth = this.elapsedTimeText.length * charSpacing;

            let startX = Math.max(5, layout.hold.x - (totalWidth / 2) + 30);
            const timerY = layout.hold.y + 490;

            for (let i = 0; i < this.elapsedTimeText.length - 1; i++) {
                const char = this.elapsedTimeText[i];
                                const charX = startX + (i * charSpacing);
                ctx.fillText(char, charX, timerY);
            }
        }

        // NEXT
        ctx.fillStyle = '#FFF';
        ctx.font = `bold ${bSize * 0.8}px "Orbitron"`;
        if (!useCustomBG) ctx.fillText('NEXT', layout.next[0].x, layout.next[0].y - bSize);
        
        for (let i = 0; i < Math.min(gameSettings.maxNext, layout.next.length); i++) {
            const pT = this.nextQueue[i];
            if (!pT) continue;
            if (pT === 'E') break;
            const s = this.getShape(pT, 0);
            const pos = layout.next[i];
            s.forEach(b => {
                const px = pos.x - (TETROMINOS[pT].center[0] * bSize) + (b[0] * bSize);
                const py = pos.y - (TETROMINOS[pT].center[1] * bSize) + (b[1] * bSize);
                this.drawBlock(pT, px, py, 1.0, bSize);
            });
        }
    }
    
        drawSpecialMoveText(bSize) {
        if (this.specialMoveTextTimer <= 0 || this.specialMoveText.length === 0 || !gameSettings.showEffects) return;
        
        // 盤面中央（ctx.translateで盤面左上が(0,0)になっているため、相対座標を使用）
        const centerX = (BOARD_WIDTH * bSize) / 2;
        const startY = (BOARD_VISIBLE_HEIGHT * bSize) / 2 - (this.specialMoveText.length * 18);
        
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        
        for (let i = 0; i < this.specialMoveText.length; i++) {
            const text = this.specialMoveText[i];
            const y = startY + (i * 35);
            
            ctx.font = `bold ${bSize}px "Orbitron"`;
            ctx.fillStyle = '#FFFFFF';
            ctx.strokeStyle = 'black';
            ctx.lineWidth = 4;

            ctx.strokeText(text, centerX, y);
            ctx.fillText(text, centerX, y);
        }
        ctx.textBaseline = 'alphabetic';
    }

        drawMessage(t, bSize) { 
        // 盤面中央（ctx.translateで盤面左上が(0,0)になっているため、相対座標を使用）
        const cX = (BOARD_WIDTH * bSize) / 2;
        const cY = (BOARD_VISIBLE_HEIGHT * bSize) / 2;
        
        ctx.fillStyle = 'rgba(0,0,0,0.75)';
        ctx.fillRect(0, 0, BOARD_WIDTH * bSize, BOARD_VISIBLE_HEIGHT * bSize); 
        ctx.fillStyle = '#FFF'; 
        ctx.textAlign = 'center'; 
        ctx.textBaseline = 'middle';
        ctx.font = `bold ${bSize * 1.5}px "Orbitron"`;
        ctx.fillText(t, cX, cY); 
        ctx.textBaseline = 'alphabetic';
    }

requestAiMove() {
        const requestId = ++this.aiRequestId;
        this.isAiThinking = true; 
        const debugPayload = {
            playerId: this.id,
            board: this.board,
            minoSequence: [this.player.pieceType, ...this.nextQueue],
            holdPiece: this.holdPiece,
            canHold: this.canHold,
            isB2B: this.isB2B,
            ren: this.ren
        };
        updateAiDebugDisplay(debugPayload);

        const currentWeights = { ...gameSettings.aiWeights };
        if (gameSettings.banPC) {
            currentWeights.perfect_clear = -999;
        }

        // Keep the real state separate: current is not the hold piece when
        // hold is empty.  Cold Clear can then preserve/re-root its DAG after
        // the move instead of rebuilding a wrong hold branch every turn.
        const aiWorkerStartPayload = {
            type: 'analyze',
            board: this.board,
            currentPiece: this.player.pieceType,
            nextQueue: this.nextQueue,
            holdPiece: this.holdPiece || null,
            canHold: this.canHold,
            isB2B: this.isB2B,
            ren: this.ren,
            incoming: this.pendingGarbage + this.garbageQueue.reduce((total, item) => total + item.lines, 0),
            thinkTimeMs: gameSettings.aiThinkTime,
            nodeLimit: Number.isFinite(gameSettings.aiNodeLimit) ? gameSettings.aiNodeLimit : 120000,
            background: true,
            requestId,
            weights: currentWeights
        };
        if (this.aiWorker) {
            this.aiSearchInitialized = true;
            this.aiWorker.postMessage(aiWorkerStartPayload);
        }
    }


    getMinoShape_forAI(type, rotation) {
        rotation = rotation % 4;
        switch (type) {
            case 'I':
                if (rotation == 0) return [[0,0], [-1,0], [1,0], [2,0]];
                if (rotation == 1) return [[1,0], [1,-1], [1,1], [1,2]];
                if (rotation == 2) return [[0,1], [-1,1], [1,1], [2,1]];
                if (rotation == 3) return [[0,0], [0,-1], [0,1], [0,2]];
                break;
            case 'O':
                return [[0,0], [1,0], [0,-1], [1,-1]];
            case 'T':
                if (rotation == 0) return [[0,0], [-1,0], [1,0], [0,-1]];
                if (rotation == 1) return [[0,0], [0,-1], [1,0], [0,1]];
                if (rotation == 2) return [[0,0], [1,0], [-1,0], [0,1]];
                if (rotation == 3) return [[0,0], [0,1], [-1,0], [0,-1]];
                break;
            case 'S':
                if (rotation == 0) return [[0,0], [-1,0], [0,-1], [1,-1]];
                if (rotation == 1) return [[0,0], [0,-1], [1,0], [1,1]];
                if (rotation == 2) return [[0,0], [1,0], [0,1], [-1,1]];
                if (rotation == 3) return [[0,0], [0,1], [-1,0], [-1,-1]];
                break;
            case 'Z':
                if (rotation == 0) return [[0,0], [1,0], [0,-1], [-1,-1]];
                if (rotation == 1) return [[0,0], [0,1], [1,0], [1,-1]];
                if (rotation == 2) return [[0,0], [-1,0], [0,1], [1,1]];
                if (rotation == 3) return [[0,0], [0,-1], [-1,0], [-1,1]];
                break;
            case 'J':
                if (rotation == 0) return [[0,0], [-1,0], [1,0], [-1,-1]];
                if (rotation == 1) return [[0,0], [0,-1], [0,1], [1,-1]];
                if (rotation == 2) return [[0,0], [1,0], [-1,0], [1,1]];
                if (rotation == 3) return [[0,0], [0,1], [0,-1], [-1,1]];
                break;
            case 'L':
                if (rotation == 0) return [[0,0], [1,0], [-1,0], [1,-1]];
                if (rotation == 1) return [[0,0], [0,1], [0,-1], [1,1]];
                if (rotation == 2) return [[0,0], [-1,0], [1,0], [-1,1]];
                if (rotation == 3) return [[0,0], [0,-1], [0,1], [-1,-1]];
                break;
        }
        return [[0,0]];
    }

    srsOffsets_forAI = {
        common: {
            '0->1': [[0,0], [-1,0], [-1,1], [0,-2], [-1,-2]],
            '1->0': [[0,0], [1,0], [1,-1], [0,2], [1,2]],
            '1->2': [[0,0], [1,0], [1,-1], [0,2], [1,2]],
            '2->1': [[0,0], [-1,0], [-1,1], [0,-2], [-1,-2]],
            '2->3': [[0,0], [1,0], [1,1], [0,-2], [1,-2]],
            '3->2': [[0,0], [-1,0], [-1,-1], [0,2], [-1,2]],
            '3->0': [[0,0], [-1,0], [-1,-1], [0,2], [-1,2]],
            '0->3': [[0,0], [1,0], [1,1], [0,-2], [1,-2]],
        },
        I: {
            '0->1': [[0,0], [-2,0], [1,0], [-2,-1], [1,2]],
            '1->0': [[0,0], [2,0], [-1,0], [2,1], [-1,-2]],
            '1->2': [[0,0], [-1,0], [2,0], [-1,2], [2,-1]],
            '2->1': [[0,0], [1,0], [-2,0], [1,-2], [-2,1]],
            '2->3': [[0,0], [2,0], [-1,0], [2,1], [-1,-2]],
            '3->2': [[0,0], [-2,0], [1,0], [-2,-1], [1,2]],
            '3->0': [[0,0], [1,0], [-2,0], [1,-2], [-2,1]],
            '0->3': [[0,0], [-1,0], [2,0], [-1,2], [2,-1]],
        }
    };

    checkCollision_forAI(type, x, y, r, currentBoard) {
        const shape = this.getMinoShape_forAI(type, r);
        for (const [dx, dy] of shape) {
            const nx = x + dx;
            const ny = y + dy;
            if (nx < 0 || nx >= BOARD_WIDTH || ny < 0 || ny >= BOARD_HEIGHT || (ny >= 0 && currentBoard[ny][nx])) {
                return true;
            }
        }
        return false;
    }

    tryRotate_forAI(type, x, y, r, direction, currentBoard) {
        const newR = (r + direction + 4) % 4;
        const offsetTable = (type === 'I' ? this.srsOffsets_forAI.I : this.srsOffsets_forAI.common);
        const offsetData = offsetTable[`${r}->${newR}`];

        for (const [ox, oy] of offsetData) {
            const newX = x + ox;
            const newY = y - oy;
            if (!this.checkCollision_forAI(type, newX, newY, newR, currentBoard)) {
                return { x: newX, y: newY, r: newR };
            }
        }
        return null;
    }

    checkTSpinCondition_forAI(target, minoType, currentBoard) {
        if (minoType !== 'T') {
            return false;
        }
        const { x, y } = target;
        const offsets = [[-1, -1], [1, -1], [-1, 1], [1, 1]];
        let filledCorners = 0;
        for (const [dx, dy] of offsets) {
            const nx = x + dx;
            const ny = y + dy;
            if (nx < 0 || nx >= BOARD_WIDTH || ny < 0 || ny >= BOARD_HEIGHT || (ny >= 0 && currentBoard[ny][nx])) {
                filledCorners++;
            }
        }
        return filledCorners >= 3;
    }

    findShortestPath_forAI(start, target, minoType, currentBoard) {
        const queue = [[start.x, start.y, start.r, []]];
        const visited = new Set();
        visited.add(`${start.x},${start.y},${start.r}`);

        let shortestPath = null;
        let preferredPath = null;

        const isTSpinPriority = this.checkTSpinCondition_forAI(target, minoType, currentBoard);

        while (queue.length > 0) {
            const [x, y, r, path] = queue.shift();

            if (preferredPath && path.length >= preferredPath.length) {
                continue;
            }
            if (!isTSpinPriority && shortestPath && path.length >= shortestPath.length) {
                continue;
            }

            if (x === target.x && r === target.r) {
                let finalY = y;
                while (!this.checkCollision_forAI(minoType, x, finalY + 1, r, currentBoard)) {
                    finalY++;
                }
                if (finalY === target.y) {
                    const finalPath = [...path, '↑'];
                    if (!shortestPath) {
                        shortestPath = finalPath;
                    }

                    if (isTSpinPriority && !preferredPath) {
                        const isGrounded = this.checkCollision_forAI(minoType, x, y + 1, r, currentBoard);
                        if (path.length > 0) {
                            const lastMove = path[path.length - 1];
                            if ((lastMove === 'R' || lastMove === 'L') && isGrounded) {
                                const tempBoard = currentBoard.map(row => [...row]);
                                const shape = this.getMinoShape_forAI(minoType, r);
                                for (const [dx, dy] of shape) {
                                    const nx = x + dx;
                                    const ny = finalY + dy;
                                    if (ny >= 0 && ny < BOARD_HEIGHT && nx >= 0 && nx < BOARD_WIDTH) {
                                        tempBoard[ny][nx] = 1;
                                    }
                                }
                                let linesCleared = 0;
                                const checkedRows = new Set();
                                for (const [, dy] of shape) {
                                    const ny = finalY + dy;
                                    if (ny >= 0 && ny < BOARD_HEIGHT && !checkedRows.has(ny)) {
                                        checkedRows.add(ny);
                                        if (tempBoard[ny].every(cell => cell === 1)) {
                                            linesCleared++;
                                        }
                                    }
                                }
                                if (linesCleared > 0) {
                                    preferredPath = finalPath;
                                }
                            }
                        }
                    }
                    continue;
                }
            }


            const actions = ['R', 'L', '←', '→', '↓'];
            for (const action of actions) {
                let nextState = null;
                let newPath = [...path, action];

                switch (action) {
                    case '←':
                        nextState = { x: x - 1, y: y, r: r };
                        break;
                    case '→':
                        nextState = { x: x + 1, y: y, r: r };
                        break;
                    case '↓':
                        nextState = { x: x, y: y + 1, r: r };
                        break;
                    case 'R':
                        nextState = this.tryRotate_forAI(minoType, x, y, r, 1, currentBoard);
                        break;
                    case 'L':
                        nextState = this.tryRotate_forAI(minoType, x, y, r, -1, currentBoard);
                        break;
                }

                if (nextState) {
                    const stateKey = `${nextState.x},${nextState.y},${nextState.r}`;
                    if (!visited.has(stateKey) && !this.checkCollision_forAI(minoType, nextState.x, nextState.y, nextState.r, currentBoard)) {
                        visited.add(stateKey);
                        queue.push([nextState.x, nextState.y, nextState.r, newPath]);
                    }
                }
            }
        }

        const foundPath = preferredPath || shortestPath;
        if (foundPath) {
            return foundPath;
        }
        
        let alternativeTarget = null;
        if (minoType === 'I' || minoType === 'S' || minoType === 'Z') {
            const { x: tx, y: ty, r: tr } = target;
            if (minoType === 'I') {
                if (tr === 0) alternativeTarget = { x: tx, y: ty - 1, r: 2 };
                else if (tr === 2) alternativeTarget = { x: tx, y: ty + 1, r: 0 };
                else if (tr === 1) alternativeTarget = { x: tx + 1, y: ty, r: 3 };
                else if (tr === 3) alternativeTarget = { x: tx - 1, y: ty, r: 1 };
            } else if (minoType === 'S') {
                if (tr === 0) alternativeTarget = { x: tx, y: ty - 1, r: 2 };
                else if (tr === 2) alternativeTarget = { x: tx, y: ty + 1, r: 0 };

                else if (tr === 1) alternativeTarget = { x: tx + 1, y: ty, r: 3 };

                else if (tr === 3) alternativeTarget = { x: tx - 1, y: ty, r: 1 };

            } else if (minoType === 'Z') {
                if (tr === 0) alternativeTarget = { x: tx, y: ty - 1, r: 2 };

                else if (tr === 2) alternativeTarget = { x: tx, y: ty + 1, r: 0 };

                else if (tr === 1) alternativeTarget = { x: tx + 1, y: ty, r: 3 };

                else if (tr === 3) alternativeTarget = { x: tx - 1, y: ty, r: 1 };
 
            }
        }

        if (alternativeTarget) {
            const queue2 = [[start.x, start.y, start.r, []]];
            const visited2 = new Set([`${start.x},${start.y},${start.r}`]);
            while (queue2.length > 0) {
                 const [x, y, r, path] = queue2.shift();

                 if (x === alternativeTarget.x && r === alternativeTarget.r) {
                     let finalY = y;

                     while (!this.checkCollision_forAI(minoType, x, finalY + 1, r, currentBoard)) {
                         finalY++;

                     }
                     if (finalY === alternativeTarget.y) {
                         return [...path, '↑'];

                     }
                 }
                 const actions = ['R', 'L', '←', '→', '↓'];

                 for (const action of actions) {
                     let nextState = null;

                     let newPath = [...path, action];
                     switch (action) {
                         case '←': nextState = { x: x - 1, y: y, r: r };

                         break;
                         case '→': nextState = { x: x + 1, y: y, r: r }; break;

                         case '↓': nextState = { x: x, y: y + 1, r: r }; break;

                         case 'R': nextState = this.tryRotate_forAI(minoType, x, y, r, 1, currentBoard); break;

                         case 'L': nextState = this.tryRotate_forAI(minoType, x, y, r, -1, currentBoard); break;

                     }
                     if (nextState) {
                         const stateKey = `${nextState.x},${nextState.y},${nextState.r}`;

                         if (!visited2.has(stateKey) && !this.checkCollision_forAI(minoType, nextState.x, nextState.y, nextState.r, currentBoard)) {
                             visited2.add(stateKey);

                             queue2.push([nextState.x, nextState.y, nextState.r, newPath]);
                         }
                     }
                 }
            }
        }

        return null;

    }
async executeAiMove(move) {


        
        if (this.gameOver || !move) {
            this.isAiThinking = false;
            return;
        }

        if (this.player.pieceType !== move.piece) {
            if (this.canHold) {
                this.hold();
                await new Promise(resolve => setTimeout(resolve, gameSettings.aiMoveDelay));
            } else {
                this.isAiThinking = false;
                return;
            }
        }

        const startState = { x: this.player.x, y: this.player.y, r: this.player.rotation };
const targetState = { x: move.x, y: move.y, r: move.rotation };
        const minoType = this.player.pieceType;
if (minoType === 'I') {
            targetState.x += 1;
startState.x += 1;
        }
        
                const debugDisplay = document.getElementById('ai-tree-debug-display');
if (debugDisplay && gameSettings.debugEnabled) {
            debugDisplay.style.display = 'block';
debugDisplay.innerHTML += `<br><span style="color:#ff88ff">AI Path: ${minoType} ${startState.x},${startState.y},${startState.r} -> ${targetState.x},${targetState.y},${targetState.r}</span>`;
        }

        const pathfinderBoard = this.board.map(row => row.map(cell => (cell === null ? 0 : 1)));
const path = this.findShortestPath_forAI(startState, targetState, minoType, pathfinderBoard);

        if (path) {
            for (const action of path) {
                                switch (action) {
                    case '←': this.player.x--;
break;
                    case '→': this.player.x++; break;
                    case '↓': this.player.y++; break;
                    case 'R': {
                        const offset = minoType === 'I' ? 1 : 0;
                        const newState = this.tryRotate_forAI(minoType, this.player.x + offset, this.player.y, this.player.rotation, 1, pathfinderBoard);
if (newState) {
                            this.player.x = newState.x - offset;
this.player.y = newState.y;
                            this.player.rotation = newState.r;
                        }
                        break;
}
                    case 'L': {
                        const offset = minoType === 'I' ? 1 : 0;
                        const newState = this.tryRotate_forAI(minoType, this.player.x + offset, this.player.y, this.player.rotation, -1, pathfinderBoard);
if (newState) {
                            this.player.x = newState.x - offset;
this.player.y = newState.y;
                            this.player.rotation = newState.r;
                        }
                        break;
}
                    case '↑':
                        this.player.y = this.getGhostY();
                        break;
                }
                let delay = gameSettings.aiMoveDelay;
                if (action === '↓') {
                    delay = gameSettings.aiSdfDelay;
                }
                await new Promise(resolve => setTimeout(resolve, delay));
            }
        }

        if (path && path.length > 0) {
            const lastMeaningfulAction = path.at(-1) === '↑' && path.length > 1 ? path.at(-2) : path.at(-1);
            this.lastMoveWasRotation = lastMeaningfulAction === 'R' || lastMeaningfulAction === 'L';
        } else {
            this.lastMoveWasRotation = false;
}

        this.player.rotation = move.rotation;
        this.player.x = move.x;
        this.player.y = move.y;
        
        // Match Cold Clear's event order: play_move/commit first, then the
        // synchronous lock/spawn emits add_next_piece for the new preview.
        if (this.aiWorker) this.aiWorker.postMessage({ type: 'commit' });
        this.lockPiece();
this.isAiThinking = false;
    }

    win() {
        this.gameClear = true;
        if (this.aiWorker) this.aiWorker.postMessage({ type: 'pause' });
        if (this.opponent) {
            this.opponent.gameOver = true;
        }
    }

    lose() {
        this.gameOver = true;
        if (this.aiWorker) this.aiWorker.postMessage({ type: 'pause' });
        if (this.opponent) {
            this.opponent.gameClear = true;
        }
    }

    setCustomUIText(text) {
        this.customUIText = String(text);
    }
    
    placeBlock(x, y, type) {
        if (x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT) {
            this.board[y][x] = type;
        }
    }

    displayGhostBlock(x, y, type) {
        if (x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT) {
            const pieceType = Object.keys(COLORS).includes(type) ? type : null;
            this.customGhosts.push({x, y, pieceType});
        }
    }

    clearAllGhostBlocks() {
        this.customGhosts = [];
    }

    setPcGuide(cells, pieceType) {
        if (!Array.isArray(cells) || !Object.keys(COLORS).includes(pieceType)) {
            this.pcGuide = null;
            return;
        }

        const uniqueCells = new Map();
        for (const cell of cells) {
            const x = Number(cell && cell.x);
            const y = Number(cell && cell.y);
            if (Number.isInteger(x) && Number.isInteger(y) && x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT) {
                uniqueCells.set(`${x},${y}`, { x, y });
            }
        }
        this.pcGuide = uniqueCells.size === 4 ? { pieceType, cells: [...uniqueCells.values()] } : null;
    }

    clearPcGuide() {
        this.pcGuide = null;
    }
    
    setPiece(type, piece, index = 0) {
        const isValidPiece = Object.keys(TETROMINOS).includes(piece) ||
            piece === null || piece === 'E';
        if (!isValidPiece) return;

        if (type === 'hold') {
            this.holdPiece = piece;
} else if (type === 'next') {
            if (index === 0) {
                this.player.pieceType = piece;
            } else if (index > 0 && index - 1 < this.nextQueue.length) {
                this.nextQueue[index - 1] = piece;
            }
}
    }

    async executeDrawMove(path) {
        if (this.isExecutingSequence) return;
this.isExecutingSequence = true;

        for (const action of path) {
            if (this.gameOver || this.gameClear) break;
                        switch (action) {
                case '←': this.move(-1, 0); break;
                case '→': this.move(1, 0); break;
                case '↓': this.move(0, 1); break;
                case 'R': this.rotate(1); break;
                case 'L': this.rotate(-1); break;
                case '↑': this.hardDrop(); break;
            }
            await new Promise(resolve => setTimeout(resolve, gameSettings.drawMoveDelay));
        }

        this.isExecutingSequence = false;
}
    
    async processDrawing() {
        if (this.drawnBlocks.size !== 4) {
            this.drawnBlocks.clear();
return;
        }

        const blocks = Array.from(this.drawnBlocks.keys()).map(k => {
            const [x, y] = k.split(',').map(Number);
            return { x, y };
        });
blocks.sort((a, b) => a.y - b.y || a.x - b.x);

        const anchor = blocks[0];
const relativeCoords = blocks.slice(1).map(b => `${b.x - anchor.x},${b.y - anchor.y}`);
        const key = relativeCoords.sort().join(';');

        const shapeInfo = DRAW_SHAPE_MAP[key];
        this.drawnBlocks.clear();
if (!shapeInfo) return;

        const targetPieceType = shapeInfo.type;
        const targetState = {
            x: anchor.x - shapeInfo.offset[0],
            y: anchor.y - shapeInfo.offset[1],
            r: shapeInfo.rot
        };

        let path = null;
        let startState = null;
        let pieceForPathfinding = null;
        let holdIsNeeded = false;

        if (this.player.pieceType === targetPieceType) {
            pieceForPathfinding = this.player.pieceType;
            startState = { x: this.player.x, y: this.player.y, r: this.player.rotation };
            holdIsNeeded = false;
        } else if (this.canHold && (this.holdPiece === targetPieceType || (!this.holdPiece && this.nextQueue[0] === targetPieceType))) {
            pieceForPathfinding = targetPieceType;
            holdIsNeeded = true;
            
            const spawnX = Math.floor(BOARD_WIDTH / 2) - Math.floor(TETROMINOS[pieceForPathfinding].center[0]) - 1;
            const tempShape = this.getShape(pieceForPathfinding, 0);
            let spawnY = 19;
            if (this.checkCollision(spawnX, spawnY, tempShape)) {
                spawnY = 18;
            }
            startState = { x: spawnX, y: spawnY, r: 0 };
        }
        
        if (startState) {
            if (pieceForPathfinding === 'I') {
                startState.x += 1;
targetState.x += 1;
            }

                        const debugDisplay = document.getElementById('ai-tree-debug-display');
if (debugDisplay) {
                debugDisplay.style.display = 'block';
debugDisplay.innerHTML = `<span style="color:#88ffff">Draw Path: ${pieceForPathfinding} ${startState.x},${startState.y},${startState.r} -> ${targetState.x},${targetState.y},${targetState.r}</span>`;
            }
            
            const pathfinderBoard = this.board.map(row => row.map(cell => (cell === null ? 0 : 1)));
path = this.findShortestPath_forAI(startState, targetState, pieceForPathfinding, pathfinderBoard);
        }
        
        if (path) {
            if (holdIsNeeded) {
                this.hold();
                await new Promise(resolve => setTimeout(resolve, gameSettings.drawMoveDelay));
            }
            await this.executeDrawMove(path);
        }
    }
}
