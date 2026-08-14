/* DOM event wiring, game startup/retry, PWA setup, analysis, and final bootstrap. */

document.addEventListener('DOMContentLoaded', () => {
    const modeTitle = document.querySelector('.mode-selection h2');
    let debugClickCount = 0;
    let debugClickTimer = null;
    let recordedReplayEvents = [];

    function ensureMemoryMonitor() {
        let memMon = document.getElementById('memory-monitor');
        if (memMon) return memMon;

        memMon = document.createElement('div');
        memMon.id = 'memory-monitor';
        memMon.style.position = 'fixed';
        memMon.style.top = '0';
        memMon.style.right = '0';
        memMon.style.background = 'rgba(0,0,0,0.8)';
        memMon.style.color = '#00ff00';
        memMon.style.padding = '5px 10px';
        memMon.style.fontFamily = 'monospace';
        memMon.style.fontSize = '12px';
        memMon.style.zIndex = '10000';
        memMon.style.pointerEvents = 'none';
        document.body.appendChild(memMon);
        return memMon;
    }

    function applyDebugModeUi() {
        const enabled = gameSettings.debugEnabled === true;
        const analyzeButton = document.getElementById('analyzeBtn');
        if (analyzeButton) {
            analyzeButton.hidden = !enabled;
            analyzeButton.setAttribute('aria-hidden', String(!enabled));
        }

        if (!enabled) {
            const analysisModal = document.getElementById('analysis-modal');
            if (analysisModal) analysisModal.style.display = 'none';
            // Analysis is debug-only; do not retain samples collected while it was enabled.
            analysisData = [];
        }

        const memMon = enabled ? ensureMemoryMonitor() : document.getElementById('memory-monitor');
        if (memMon) memMon.style.display = enabled ? 'block' : 'none';

        const debugIds = ['ai-tree-debug-display', 'ai-debug-display', 'ai-debug-controls'];
        debugIds.forEach(id => {
            const el = document.getElementById(id);
            if (el) el.style.display = enabled ? 'block' : 'none';
        });
    }

    function setDebugMode(enabled) {
        gameSettings.debugEnabled = Boolean(enabled);
        saveGameSettings();
        applyDebugModeUi();
    }

    if (modeTitle) {
        modeTitle.style.userSelect = 'none'; 
        
        modeTitle.addEventListener('click', () => {
            debugClickCount++;
            
            if (debugClickTimer) clearTimeout(debugClickTimer);
            debugClickTimer = setTimeout(() => { debugClickCount = 0; }, 400);

            if (debugClickCount >= 10) {
                setDebugMode(!gameSettings.debugEnabled);
                alert(`デバッグモード: ${gameSettings.debugEnabled ? "ON" : "OFF"}`);
                debugClickCount = 0;
            }
        });
    }


    mainCanvas = document.getElementById('mainCanvas'); ctx = mainCanvas.getContext('2d');
    
    virtualController.init();

    // The simulator used to export only board snapshots. Keep a separate
    // event stream so the editor can reconstruct the exact locked operation,
    // including the board before the lock and whether HOLD was used.
    window.resetRecordedReplay = () => { recordedReplayEvents = []; };
    window.recordReplayLock = (lockingPlayer, operation) => {
        if (!lockingPlayer || !operation || !players.length) return;
        const event = { time: performance.now(), pages: {} };
        players.forEach(player => {
            const playerId = `p${player.id}`;
            const isLockingPlayer = player === lockingPlayer;
            event.pages[playerId] = {
                b: isLockingPlayer ? boardToString(operation.boardBefore) : boardToString(player.board),
                h: isLockingPlayer ? (operation.holdBefore || '') : (player.holdPiece || ''),
                n: '',
                o: isLockingPlayer ? {
                    type: operation.type,
                    rotation: operation.rotation,
                    x: operation.x,
                    y: operation.y,
                    coordinateSpace: 'simulator',
                    lock: true,
                    ...(operation.holdUsed ? { holdUsed: true } : {})
                } : null
            };
        });
        recordedReplayEvents.push(event);
    };
    window.createRecordedReplayCollection = () => {
        if (!recordedReplayEvents.length || !gameHistoryLog.length) return null;
        const initialLog = gameHistoryLog[0];
        const playerIds = gameMode === '2P' ? ['p1', 'p2'] : ['p1'];
        const initial = {};
        playerIds.forEach(playerId => {
            const player = players.find(item => `p${item.id}` === playerId);
            const initialPage = initialLog[playerId] || { b: boardToString(Array.from({ length: BOARD_HEIGHT }, () => Array(BOARD_WIDTH).fill(null))), h: '', n: '' };
            const sequence = player?.fullMinoSequence?.filter(piece => ['I', 'O', 'T', 'L', 'J', 'S', 'Z'].includes(piece)).join('')
                || String(initialPage.n || '').replace(/[^IOTLSJZ]/gi, '');
            initial[playerId] = {
                board: stringToBoard(initialPage.b),
                hold: String(initialPage.h || '').replace(/[^IOTLSJZ]/gi, ''),
                sequence
            };
        });
        const pages = recordedReplayEvents.map(event => {
            const page = {};
            playerIds.forEach(playerId => {
                const entry = event.pages[playerId];
                page[playerId] = {
                    board: stringToBoard(entry.b),
                    hold: entry.h || '',
                    next: '',
                    operation: entry.o || null,
                    placementDraft: [],
                    placementMode: false,
                    nextInsertionIndex: -1,
                    viewY: BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT,
                    activeColor: 'I'
                };
            });
            return page;
        });
        return {
            v: 3,
            m: gameMode,
            currentCase: 0,
            cases: [{
                id: `simulator-replay-${Date.now()}`,
                name: 'Simulator recorded replay',
                kind: 'replay',
                gameMode,
                initial: {
                    p1: initial.p1,
                    p2: initial.p2 || { board: stringToBoard(''), hold: '', sequence: '' }
                },
                pages
            }]
        };
    };

    loadKeyBindings();
    loadGameSettings();
    applyDebugModeUi();

    if (loadSkinsFromLocalStorage()) {
        console.log('Custom skins loaded from localStorage.');
    }

    document.getElementById('shareBtn').addEventListener('click', openShareModal);
    document.getElementById('share-close').addEventListener('click', () => {
        document.getElementById('share-modal').style.display = 'none';
    });

    document.getElementById('ruleBtn').addEventListener('click', () => {
        document.getElementById('rule-description-input').value = editorData.rule.description;
        document.getElementById('rule-code-input').value = editorData.rule.code;
        document.getElementById('rule-modal').style.display = 'flex';
    });
    document.getElementById('rule-save-close').addEventListener('click', () => {
        editorData.rule.description = document.getElementById('rule-description-input').value;
editorData.rule.code = document.getElementById('rule-code-input').value;
        document.getElementById('rule-modal').style.display = 'none';
    });
    document.getElementById('rule-close').addEventListener('click', () => {
        document.getElementById('rule-modal').style.display = 'none';
    });

    document.getElementById('swapBtn').addEventListener('click', () => {
        if (gameMode !== '2P') return;
        
        const tempBoard = editorData.p1.board;
        const tempNext = editorData.p1.nextQueue;
        const tempHold = editorData.p1.hold;
        const tempNextInsertion = editorData.p1.nextInsertionIndex;

        editorData.p1.board = editorData.p2.board;
        editorData.p1.nextQueue = editorData.p2.nextQueue;
        editorData.p1.hold = editorData.p2.hold;
        editorData.p1.nextInsertionIndex = editorData.p2.nextInsertionIndex;

        editorData.p2.board = tempBoard;
        editorData.p2.nextQueue = tempNext;
        editorData.p2.hold = tempHold;
        editorData.p2.nextInsertionIndex = tempNextInsertion;

        drawEditorField('p1');
        updateNextQueueDisplay('p1');
        drawEditorField('p2');
        updateNextQueueDisplay('p2');
    });

document.getElementById('copy-link-btn').addEventListener('click', () => {
        const input = document.getElementById('share-link-input');
        input.select();
        navigator.clipboard.writeText(input.value)
            .then(() => alert('共有リンクをクリップボードにコピーしました！'))
            .catch(err => alert('コピーに失敗しました: ' + err));
    });
    


        document.getElementById('import-from-data-btn').addEventListener('click', async () => {
        try {
            const text = await navigator.clipboard.readText();
            if (!text) {
                alert('クリップボードが空です。');
                return;
            }

            let data;

                if (text.startsWith('http') && text.includes('#')) {
                const base64Data = text.substring(text.indexOf('#') + 1);
                const binaryString = atob(base64Data);
                const bytes = Uint8Array.from(binaryString, c => c.charCodeAt(0));
                const jsonString = new TextDecoder().decode(bytes);
                data = JSON.parse(jsonString);
            } else {
                data = JSON.parse(text);
            }
            
            if(applyGameState(data)) {
                 alert('クリップボードから盤面を読み込みました。');
                 document.getElementById('share-modal').style.display = 'none';
            }

        } catch (e) {
            alert('クリップボードのデータが無効か、読み込みに失敗しました。');
            console.error('Failed to import from clipboard:', e);
        }
    });
document.getElementById('mode-1p').addEventListener('click', () => { 
        gameMode = '1P'; 
        document.getElementById('mode-1p').classList.add('active'); 
        document.getElementById('mode-2p').classList.remove('active'); 
        document.getElementById('p2-editor-col').style.display = 'none'; 
        document.getElementById('swapBtn').style.display = 'none';
        setTimeout(updateScale, 0);
    });
document.getElementById('mode-2p').addEventListener('click', () => { 
        gameMode = '2P'; 
        document.getElementById('mode-2p').classList.add('active'); 
        document.getElementById('mode-1p').classList.remove('active'); 
        document.getElementById('p2-editor-col').style.display = 'flex';
        document.getElementById('swapBtn').style.display = 'inline-block';
        setTimeout(updateScale, 0);
    });
document.getElementById('advanced-link-btn').addEventListener('click', () => {
        document.getElementById('advanced-link-options').style.display = 'block';
    });
    
    const startSimCheckbox = document.getElementById('start-sim-checkbox');
    const hideBackCheckbox = document.getElementById('hide-back-btn-checkbox');
    startSimCheckbox.addEventListener('change', () => {
        if (startSimCheckbox.checked) {
            hideBackCheckbox.disabled = false;
        } else {
            hideBackCheckbox.disabled = true;
            hideBackCheckbox.checked = false;
        }
    });

    document.getElementById('generate-advanced-link-btn').addEventListener('click', () => {
        generateAndDisplayLink({
            startSim: document.getElementById('start-sim-checkbox').checked,
            noHold: document.getElementById('no-hold-checkbox').checked,
            hideBack: document.getElementById('hide-back-btn-checkbox').checked
        });
    });
document.getElementById('startGameBtn').addEventListener('click', () => {
        gameHistoryLog = [];
        window.resetRecordedReplay?.();
        let currentRunSettings = { ...gameSettings };
        gameStartTime = performance.now();
        let startTime = gameStartTime;
        
        players = [];
        const p1_isAi = document.getElementById('p1-ai-toggle').checked;
        const p2_isAi = document.getElementById('p2-ai-toggle').checked;
        
        if (gameMode === '1P') {
          
           const player = new Player('1', 0, keyBindings.p1, 0, p1_isAi);
            player.holdDisabled = autoStartParams.nh;
            players.push(player);
        } else {
            const p1 = new Player('1', 0, keyBindings.p1, 0, p1_isAi);
            p1.holdDisabled = autoStartParams.nh;
            const p2 = new Player('2', PLAYER_CANVAS_WIDTH, keyBindings.p2, 1, p2_isAi);
            p2.holdDisabled = autoStartParams.nh;
      
            p1.opponent = p2;
            p2.opponent = p1;
            players.push(p1, p2);
        }

        logCurrentGameState();

        if (editorData.rule.code) {
 try {
        const ruleWorkerScript = `
                    let userFunctions = {};
let startTime = 0;
                    let playerStates = {
                        '1': { board: [], holdPiece: null, currentPiece: null, nextQueue: [], fullMinoSequence: [], stats: {} },
                        '2': { board: [], holdPiece: null, currentPiece: null, nextQueue: [], fullMinoSequence: [], stats: {} }
                    };
const createBoardAPI = (playerId) => ({
                        hasBlock: (x, y) => {
                            const board = playerStates[playerId]?.board;
                            if (!board || y < 0 || y 
>= board.length || x < 0 || x >= (board[0] || []).length) {
                                return false;
                            }
                           
 return board[y][x] !== null;
                        },
                        placeBlock: (x, y, type) => {
                            postMessage({ command: 'api', func: 'placeBlock', args: [playerId, x, y, type] });
     
                   }
                    });
const api = {
                        win: (playerProxy) => postMessage({ command: 'api', func: 'win', args: [playerProxy.id] }),
                        lose: (playerProxy) => postMessage({ command: 'api', func: 'lose', args: [playerProxy.id] }),
                        setCustomUIText: 
(playerProxy, text) => postMessage({ 
command: 'api', func: 'setCustomUIText', args: [playerProxy.id, text] }),
                        getTime: () => performance.now() - startTime,
                        sendAttack: (receivingPlayerProxy, lines) => postMessage({ command: 'api', func: 'sendAttack', args: [receivingPlayerProxy.id, lines] }),
forceSetting: (key, value) => postMessage({ command: 'api', func: 'forceSetting', args: [key, value] }),
                
        displayGhostBlock: (playerProxy, x, y, type) => postMessage({ command: 'api', func: 'displayGhostBlock', args: [playerProxy.id, x, y, type] }),
                        clearAllGhostBlocks: (playerProxy) => postMessage({ command: 'api', func: 'clearAllGhostBlocks', args: [playerProxy.id] }),
         getPiece: (playerProxy, type, index = 0) => {
                          
  const state = playerStates[playerProxy.id];
                            if (!state) return null;
                            if (type === 'hold') return state.holdPiece;
if (type === 'next') {
                                if (index === 0) return state.currentPiece;
                                return state.nextQueue[index - 1] || null;
                            }
                            return null;
},
                        getFullSequence: (playerProxy) => {
                            const state = playerStates[playerProxy.id];
return state ? [...state.fullMinoSequence] : [];
},
                        setPiece: (playerProxy, type, piece, index) => postMessage({ command: 'api', func: 'setPiece', args: [playerProxy.id, type, piece, index] }),
                    };
self.onmessage = (e) => {
                        const { command, data } = e.data;
                        if (data && data.playerStateProxy) {
                            const { id, board, holdPiece, currentPiece, nextQueue, fullMinoSequence, stats } = data.playerStateProxy;
if (playerStates[id]) {
                                playerStates[id] = { id, board, holdPiece, currentPiece, nextQueue, fullMinoSequence, stats };
}
                        }

if (command === 'init') {
                            if (data.initialPlayerState) {
                                const { id, board, holdPiece, currentPiece, nextQueue, fullMinoSequence, stats } = data.initialPlayerState;
                                if (playerStates[id]) {
                                    playerStates[id] = { id, board, holdPiece, currentPiece, nextQueue, fullMinoSequence, stats };
                                }
                            }
                            startTime = data.startTime;
const p1Proxy = { id: '1', board: createBoardAPI('1') };
                            const p2Proxy = data.is2P ? { id: '2', board: createBoardAPI('2') } : null;
const gameApiProxy = { p1: p1Proxy, p2: p2Proxy, ...api };
const combinedCode = data.ruleCode + \`
                                return {
                                    onInit: typeof onInit === 'function' ?
onInit : undefined,
                                    onPieceLock: typeof onPieceLock === 'function' ?
onPieceLock : undefined,
                                    onUpdate: typeof onUpdate === 'function' ?
onUpdate : undefined
                                };\`;
const ruleScript = new Function('api', combinedCode);
                            userFunctions = ruleScript(gameApiProxy);
                            const availableHooks = {};
if (userFunctions.onInit) {
                                availableHooks.onInit = true;
userFunctions.onInit(gameApiProxy);
                            }
                            if (userFunctions.onUpdate) availableHooks.onUpdate = true;
if (userFunctions.onPieceLock) availableHooks.onPieceLock = true;
                            postMessage({ command: 'hooksAvailable', hooks: availableHooks });
} else if ((command === 'pieceLock' && userFunctions.onPieceLock) || (command === 'update' && userFunctions.onUpdate)) {
                            const p1Proxy = { id: '1', board: createBoardAPI('1'), stats: playerStates['1'].stats };
const p2Proxy = data.is2P ? { id: '2', board: createBoardAPI('2'), stats: playerStates['2'].stats } : null;
if (p1Proxy && p2Proxy) {
                                p1Proxy.opponent = p2Proxy;
                                p2Proxy.opponent = p1Proxy;
                            }
                            const gameApiProxy = { p1: p1Proxy, p2: p2Proxy, ...api };
                            const playerProxy = data.playerStateProxy.id === '1' ? p1Proxy : p2Proxy;

if (command === 'pieceLock') {
                                userFunctions.onPieceLock(gameApiProxy, playerProxy, data.moveInfo);
                                postMessage({ command: 'hookComplete', hook: 'pieceLock' });
} else {
                                userFunctions.onUpdate(gameApiProxy, playerProxy);
}
}
};
`;

                const ruleWorkerBlob = new Blob([ruleWorkerScript], { type: 'application/javascript' });

                players.forEach(p => {
                    p.ruleWorker = new Worker(URL.createObjectURL(ruleWorkerBlob));
                    p.ruleWorker.onmessage = (e) => {
                        const { command, func, args, hooks } = e.data;


                        if (command === 'api') {
                            const getPlayerById = (id) => players.find(player => player.id === id); // Define once

                     switch(func) {

                                case 'win': getPlayerById(args[0])?.win(); break;
                                case 'lose': getPlayerById(args[0])?.lose(); break;

                        case 'setCustomUIText': getPlayerById(args[0])?.setCustomUIText(args[1]); break;


                                case 'sendAttack':
                                    const targetPlayer = getPlayerById(args[0]);

                                   if (targetPlayer) {
                                        targetPlayer.garbageQueue.push({

                                     lines: args[1],
                                            receivedTime: performance.now()

                                });
                                    }
                                    break;
                        case 'forceSetting':
                                    if (args[0] in currentRunSettings) currentRunSettings[args[0]] = args[1];
                                break;
                                case 'displayGhostBlock': getPlayerById(args[0])?.displayGhostBlock(args[1], args[2], args[3]); break;
                                case 'clearAllGhostBlocks': getPlayerById(args[0])?.clearAllGhostBlocks(); break;
                                case 'setPiece': getPlayerById(args[0])?.setPiece(args[1], args[2], args[3]); break;
                        case 'placeBlock': getPlayerById(args[0])?.placeBlock(args[1], args[2], args[3]); break;
                            }
                        } else if (command === 'hooksAvailable') {
                            p.activeRuleHooks = hooks;
if(hooks.onInit){
                                Object.assign(gameSettings, currentRunSettings);
}
                        } else if (command === 'hookComplete' && e.data.hook === 'pieceLock') {
                            p.finishLockPiece();
                        }
                    };
                    const initialStateProxy = {
                        id: p.id,
                        board: p.board,
                        holdPiece: p.holdPiece,
                        currentPiece: p.player.pieceType,
                        nextQueue: p.nextQueue,
                        fullMinoSequence: p.fullMinoSequence,
                        stats: p.stats
                    };
p.ruleWorker.postMessage({
                        command: 'init',
                        data: {
                            ruleCode: editorData.rule.code,
                     
       startTime: startTime,
                            is2P: gameMode === '2P',
                            initialPlayerState: initialStateProxy
                        }
                    });
});
            } catch (e) {
                alert('カスタムルールWorkerの生成中にエラーが発生しました:\n' + e.message);
                console.error("Custom rule worker error:", e);
                return;
            }
        }
        
                const originalSettingsBeforeRun = { ...gameSettings };
        if(!editorData.rule.code) {
             Object.assign(gameSettings, currentRunSettings);
        }

        // --- 追加: 背景画像の設定 ---
        if (gameSettings.layout && gameSettings.layout.backgroundImage) {
            // bodyに背景画像を設定して全画面表示にする
            document.body.style.backgroundImage = `url(${gameSettings.layout.backgroundImage})`;
            document.body.style.backgroundSize = 'cover';
            document.body.style.backgroundPosition = 'center center';
            document.body.style.backgroundRepeat = 'no-repeat';
            
            // キャンバスの枠線と背景を消す
            mainCanvas.style.border = 'none';
            mainCanvas.style.backgroundColor = 'transparent';
            mainCanvas.style.boxShadow = 'none';
        } else {
            // 背景がない場合はデフォルトに戻す（念のため）
            document.body.style.backgroundImage = '';
            mainCanvas.style.border = ''; // CSSの定義に戻す
            mainCanvas.style.backgroundColor = ''; // CSSの定義に戻す
            mainCanvas.style.boxShadow = '';
        }
        // ---------------------------

        gameState = 'PLAYING'; 
        document.getElementById('editor-container').style.display = 'none'; 
        document.getElementById('game-container').style.display = 'block';
        document.getElementById('scan-controls').style.display = 'none';
        document.getElementById('game-controls').style.display = 'flex';
const ruleDescDisplay = document.getElementById('rule-description-display');
        if (editorData.rule.description) {
            ruleDescDisplay.innerText = editorData.rule.description;
ruleDescDisplay.style.display = 'block';
            const p1OffsetX = 0;
            const leftPos = p1OffsetX + PADDING;
            const topPos = 160; 
            ruleDescDisplay.style.left = `${leftPos}px`;
ruleDescDisplay.style.top = `${topPos}px`;
            ruleDescDisplay.style.maxWidth = `${HOLD_AREA_WIDTH - PADDING}px`;
        } else {
            ruleDescDisplay.style.display = 'none';
}
        
        if (autoStartParams.hb) {
            document.getElementById('backToEditorBtn').style.display = 'none';
} else {
            document.getElementById('backToEditorBtn').style.display = '';
}

        ctx.setTransform(1, 0, 0, 1, 0, 0);

        if (gameMode === '1P') {
            mainCanvas.width = PLAYER_CANVAS_WIDTH * RESOLUTION_SCALE;
mainCanvas.height = CANVAS_HEIGHT * RESOLUTION_SCALE;
            mainCanvas.style.width = PLAYER_CANVAS_WIDTH + 'px';
            mainCanvas.style.height = CANVAS_HEIGHT + 'px';
}
        else {
            const totalWidth = PLAYER_CANVAS_WIDTH * 2;
mainCanvas.width = totalWidth * RESOLUTION_SCALE; 
            mainCanvas.height = CANVAS_HEIGHT * RESOLUTION_SCALE;
            mainCanvas.style.width = totalWidth + 'px';
            mainCanvas.style.height = CANVAS_HEIGHT + 'px';
        }

        
        autoStartParams = { ss: false, nh: false, hb: false };
        ctx.scale(RESOLUTION_SCALE, RESOLUTION_SCALE);

if (gameSettings.touchControlsEnabled && gameSettings.touchControlType === 'button') {
            virtualController.show();
        }

        const aiDebugDisplay = document.getElementById('ai-debug-display');
        const aiDebugControls = document.getElementById('ai-debug-controls');
        if (players.some(p => p.isAi)) {
            if(aiDebugDisplay) aiDebugDisplay.style.display = 'none';
            if(aiDebugControls) aiDebugControls.style.display = 'none';
        } else {
            if(aiDebugDisplay) aiDebugDisplay.style.display = 'none';
            if(aiDebugControls) aiDebugControls.style.display = 'none';
        }

        setTimeout(updateScale, 0);
    });


document.getElementById('backToEditorBtn').addEventListener('click', () => {
        gameState = 'EDITING';
        gameHistoryLog = [];
        window.resetRecordedReplay?.();
        
        if (players.length > 0) {
            players.forEach(p => {
                if (p.aiWorker) {
                    p.aiWorker.terminate();
                }
                if (p.ruleWorker) {
                    p.ruleWorker.terminate();
                }
            });
        }
        
                players = [];
        loadGameSettings();

        // --- 追加: 背景とキャンバススタイルのリセット ---
        document.body.style.backgroundImage = '';
        mainCanvas.style.border = ''; // CSSの定義(枠線あり)に戻す
        mainCanvas.style.backgroundColor = ''; // CSSの定義(背景色あり)に戻す
        mainCanvas.style.boxShadow = '';
        // -----------------------------------------

        document.getElementById('game-container').style.display = 'none';
        document.getElementById('game-controls').style.display = 'none';
        document.getElementById('editor-container').style.display = 'flex';
        document.getElementById('rule-description-display').style.display = 'none';
        
        virtualController.hide();
        
        const aiDebugDisplay = document.getElementById('ai-debug-display');
        const aiDebugControls = document.getElementById('ai-debug-controls');
        if(aiDebugDisplay) aiDebugDisplay.style.display = 'none';
        if(aiDebugControls) aiDebugControls.style.display = 'none';

        setTimeout(updateScale, 0);
    });
    
    ['p1', 'p2'].forEach(pId => { 
        document.getElementById(`imageLoader-${pId}`).addEventListener('change', e => { 
            if (e.target.files && e.target.files[0]) {
                startScanProcess(e.target.files[0], e.target.dataset.player);
            }
        }); 
        document.getElementById(`pptLoader-${pId}`).addEventListener('change', e => { 
            if (e.target.files && e.target.files[0]) {
                processPptImage(e.target.files[0]);
            }
        });
    });
    document.getElementById('scanCancelBtn').addEventListener('click', endScanProcess);
    document.getElementById('scanConfirmBtn').addEventListener('click', () => { if (gameState === 'SCAN_BL' && scanState.bottomLeft) gameState = 'SCAN_TR'; else if (gameState === 'SCAN_TR' && scanState.topRight) processAndLoadBoard(); updateScanUI(); });
    mainCanvas.addEventListener('click', e => { if (!gameState.startsWith('SCAN')) return; const rect = mainCanvas.getBoundingClientRect();
    const scaleX = mainCanvas.width / rect.width;
    const scaleY = mainCanvas.height / rect.height;
    const pos = { x: (e.clientX - rect.left) * scaleX, y: (e.clientY - rect.top) * scaleY };
    if (gameState==='SCAN_BL') scanState.bottomLeft=pos; else if (gameState==='SCAN_TR') scanState.topRight=pos; updateScanUI(); });

    const getDrawCoordsFromEvent = (e) => {
        if (!players[0]) return null;
        const rect = mainCanvas.getBoundingClientRect();
        const clientX = e.touches ? e.touches[0].clientX : e.clientX;
        const clientY = e.touches ? e.touches[0].clientY : e.clientY;
        const x = Math.floor(((clientX - rect.left) / rect.width) * (mainCanvas.width / RESOLUTION_SCALE) / BLOCK_SIZE - (PLAYFIELD_X_OFFSET / BLOCK_SIZE));
        const y = Math.floor(((clientY - rect.top) / rect.height) * (mainCanvas.height / RESOLUTION_SCALE) / BLOCK_SIZE - 0.5) + players[0].viewY;
        return { x, y };
    };

    const handleDrawStartOnBoard = e => {
        if (gameState !== 'PLAYING' || !gameSettings.touchControlsEnabled || gameSettings.touchControlType !== 'draw' || !players[0] || players[0].isExecutingSequence) return;
e.preventDefault();
        players[0].isDrawingOnBoard = true;
        const coords = getDrawCoordsFromEvent(e);

        if (coords && coords.x >= 0 && coords.x < BOARD_WIDTH && coords.y >= 0 && coords.y < BOARD_HEIGHT && !players[0].board[coords.y][coords.x]) {
            players[0].drawnBlocks.set(`${coords.x},${coords.y}`, true);
        }
    };

    const handleDrawMoveOnBoard = e => {
        if (gameState !== 'PLAYING' || !gameSettings.touchControlsEnabled || gameSettings.touchControlType !== 'draw' || !players[0] || !players[0].isDrawingOnBoard) return;
        e.preventDefault();
        const coords = getDrawCoordsFromEvent(e);
        if (coords && coords.x >= 0 && coords.x < BOARD_WIDTH && coords.y >= 0 && coords.y < BOARD_HEIGHT && !players[0].board[coords.y][coords.x]) {
            if (players[0].drawnBlocks.size < 4) {
                 players[0].drawnBlocks.set(`${coords.x},${coords.y}`, true);
            }
        }
    };

    const handleDrawEndOnBoard = e => {
        if (gameState !== 'PLAYING' || !gameSettings.touchControlsEnabled || gameSettings.touchControlType !== 'draw' || !players[0] || !players[0].isDrawingOnBoard) return;
        e.preventDefault();
        players[0].isDrawingOnBoard = false;
        players[0].processDrawing();
    };

    mainCanvas.addEventListener('mousedown', handleDrawStartOnBoard);
    mainCanvas.addEventListener('mousemove', handleDrawMoveOnBoard);
    mainCanvas.addEventListener('mouseup', handleDrawEndOnBoard);
    mainCanvas.addEventListener('mouseleave', handleDrawEndOnBoard);
    mainCanvas.addEventListener('touchstart', handleDrawStartOnBoard, { passive: false });
    mainCanvas.addEventListener('touchmove', handleDrawMoveOnBoard, { passive: false });
    mainCanvas.addEventListener('touchend', handleDrawEndOnBoard, { passive: false });
    mainCanvas.addEventListener('touchcancel', handleDrawEndOnBoard, { passive: false });

    document.getElementById('settingsBtn').addEventListener('click', () => openUnifiedSettingsModal('general'));
    document.getElementById('p1-key-config-btn').addEventListener('click', () => openUnifiedSettingsModal('p1-keys'));
    document.getElementById('p2-key-config-btn').addEventListener('click', () => openUnifiedSettingsModal('p2-keys'));

    document.getElementById('settings-close').addEventListener('click', () => {
        saveGameSettings();
        try { localStorage.setItem('tetrisKeyBindings', JSON.stringify(keyBindings)); } 
        catch (e) { console.error("Failed to save key bindings to localStorage:", e); }

        document.getElementById('settings-modal').style.display = 'none';
        isBindingKey = false; bindingPlayer = null; bindingAction = null;
    });

    document.getElementById('vc-edit-layout-btn').addEventListener('click', () => {
        document.getElementById('settings-modal').style.display = 'none';
        virtualController.startEditMode();
    });

    const touchModeButtons = document.querySelectorAll('#p1-touch-mode-selection .button');
        touchModeButtons.forEach(button => {
        button.addEventListener('click', () => {
            const newMode = button.dataset.touchMode;
            gameSettings.touchControlType = newMode;
            touchModeButtons.forEach(btn => btn.classList.remove('active'));
            button.classList.add('active');
            document.getElementById('p1-touch-button-controls').style.display = newMode === 'button' ? 'flex' : 'none';
            document.getElementById('p1-touch-draw-controls').style.display = newMode === 'draw' ? 'flex' : 'none';
       
 });
    });

    document.getElementById('vc-save-and-close-btn').addEventListener('click', () => {

        virtualController.endEditMode();
    });
    document.getElementById('vc-copy-layout-btn').addEventListener('click', virtualController.copyLayoutsToClipboard);
    document.getElementById('vc-paste-layout-btn').addEventListener('click', virtualController.importLayoutsFromClipboard);

    document.addEventListener('keydown', e => {
        if (isBindingKey) {
            e.preventDefault();
            let keyLabel = e.key;
            if (keyLabel === ' ') keyLabel = 'Space';
            bindKey({ type: 'key', value: e.key.toLowerCase(), label: keyLabel });
        } 
        else if (gameState === 'PLAYING') {
            players.forEach(p => {
                p.keys[e.key.toLowerCase()] = true;
                Object.keys(p.keyBindings).forEach(action => {
                    const binding = p.keyBindings[action];
                    if (binding.type === 'key' && binding.value === e.key.toLowerCase()) {
                        p.handlePress(action);
                    }
                });
            });
        }
    });
    document.addEventListener('keyup', e => {
        if (gameState === 'PLAYING') {
            players.forEach(p => p.keys[e.key.toLowerCase()] = false );
        }
    });

    if ('serviceWorker' in navigator) {
        navigator.serviceWorker.register('./sw.js')
            .then(reg => console.log('Service Worker registered successfully.'))
            .catch(err => console.error('Service Worker registration failed:', err));
    }
    
document.getElementById('retryBtn').addEventListener('click', () => {
        if (gameState !== 'PLAYING' || !players.length) return;
        
            players.forEach(p => {
            if (p.aiWorker) {
                p.aiWorker.terminate();
            }
            if (p.ruleWorker) {
                p.ruleWorker.terminate();
            }
        });

        loadGameSettings();
        gameHistoryLog = [];
        window.resetRecordedReplay?.();
     
        let currentRunSettings = { ...gameSettings };
        gameStartTime = performance.now();
        let startTime = gameStartTime;
        const wasHoldDisabled = players[0].holdDisabled;

        players = [];
        const p1_isAi_retry 
= document.getElementById('p1-ai-toggle').checked;
        const p2_isAi_retry = document.getElementById('p2-ai-toggle').checked;

        if (gameMode === '1P') {
            const player = new Player('1', 0, keyBindings.p1, 0, p1_isAi_retry);
            player.holdDisabled = wasHoldDisabled;
            players.push(player);
        } else {
         
   const p1 = new Player('1', 0, keyBindings.p1, 0, p1_isAi_retry);
            p1.holdDisabled = wasHoldDisabled;
const p2 = new Player('2', PLAYER_CANVAS_WIDTH, keyBindings.p2, 1, p2_isAi_retry);
            p2.holdDisabled = wasHoldDisabled;
            p1.opponent = p2;
            p2.opponent = p1;
            players.push(p1, p2);
}
        
        logCurrentGameState();
        
        if (editorData.rule.code) {
             const ruleWorkerScript = `
                    let userFunctions = {};
let startTime = 0;
                    let playerStates = {
                        '1': { board: [], holdPiece: null, currentPiece: null, nextQueue: [], fullMinoSequence: [], stats: {} },
                        '2': { board: [], holdPiece: null, currentPiece: null, nextQueue: [], fullMinoSequence: [], stats: {} }
                 
   };
const createBoardAPI = (playerId) => ({
                        hasBlock: (x, y) => {
                            const board = playerStates[playerId]?.board;
                            if (!board || y 
< 0 || y 
>= board.length || x < 0 || x >= (board[0] || []).length) {
                                return false;
                            }
                       
    
 return board[y][x] !== null;
                        },
                        placeBlock: (x, y, type) => {
                            postMessage({ command: 'api', func: 'placeBlock', args: [playerId, x, y, type] });
 
    
                   }
                    });
const api = {
                        win: (playerProxy) => postMessage({ command: 'api', func: 'win', args: [playerProxy.id] }),
                        lose: (playerProxy) => postMessage({ command: 'api', func: 'lose', args: [playerProxy.id] }),
                        setCustomUIText: 
(playerProxy, text) => postMessage({ 

command: 'api', func: 'setCustomUIText', args: [playerProxy.id, text] }),
                        getTime: () => performance.now() - startTime,
                        sendAttack: (receivingPlayerProxy, lines) => postMessage({ command: 'api', func: 'sendAttack', args: [receivingPlayerProxy.id, lines] }),
forceSetting: (key, value) => postMessage({ command: 'api', func: 'forceSetting', args: [key, value] }),
                

        displayGhostBlock: (playerProxy, x, y, type) => postMessage({ command: 'api', func: 'displayGhostBlock', args: [playerProxy.id, x, y, type] }),
                        clearAllGhostBlocks: (playerProxy) => postMessage({ command: 'api', func: 'clearAllGhostBlocks', args: [playerProxy.id] }),
         getPiece: (playerProxy, type, index = 0) => {
                          

  const state = playerStates[playerProxy.id];
                            if (!state) return null;
                            if (type === 'hold') return state.holdPiece;
if (type === 'next') {
                                if (index === 0) return state.currentPiece;
return state.nextQueue[index - 1] || null;
                            }
                            return null;
},
                        getFullSequence: (playerProxy) => {
                            const state = playerStates[playerProxy.id];
return state ? [...state.fullMinoSequence] : [];
},
                        setPiece: (playerProxy, type, piece, index) => postMessage({ command: 'api', func: 'setPiece', args: [playerProxy.id, type, piece, index] }),
                    };
self.onmessage = (e) => {
                        const { command, data } = e.data;
if (data && data.playerStateProxy) {
                            const { id, board, holdPiece, currentPiece, nextQueue, fullMinoSequence, stats } = data.playerStateProxy;
if (playerStates[id]) {
                                playerStates[id] = { id, board, holdPiece, currentPiece, nextQueue, fullMinoSequence, stats };
}
                        }

if (command === 'init') {
                            if (data.initialPlayerState) {
                                const { id, board, holdPiece, currentPiece, nextQueue, fullMinoSequence, stats } = data.initialPlayerState;
                                if (playerStates[id]) {
                                    playerStates[id] = { id, board, holdPiece, currentPiece, nextQueue, fullMinoSequence, stats };
                                }
                            }
                            startTime = data.startTime;
const p1Proxy = { id: '1', board: createBoardAPI('1') };
                            const p2Proxy = data.is2P ?
{ id: '2', board: createBoardAPI('2') } : null;
const gameApiProxy = { p1: p1Proxy, p2: p2Proxy, ...api };
const combinedCode = data.ruleCode + \`
                                return {
                                    onInit: typeof onInit === 'function' ?
onInit : undefined,
                                    onPieceLock: typeof onPieceLock === 'function' ?
onPieceLock : undefined,
                                    onUpdate: typeof onUpdate === 'function' ?
onUpdate : undefined
                                };\`;
const ruleScript = new Function('api', combinedCode);
                            userFunctions = ruleScript(gameApiProxy);
                            const availableHooks = {};
if (userFunctions.onInit) {
                                availableHooks.onInit = true;
userFunctions.onInit(gameApiProxy);
                            }
                            if (userFunctions.onUpdate) availableHooks.onUpdate = true;
if (userFunctions.onPieceLock) availableHooks.onPieceLock = true;
                            postMessage({ command: 'hooksAvailable', hooks: availableHooks });
} else if ((command === 'pieceLock' && userFunctions.onPieceLock) || (command === 'update' && userFunctions.onUpdate)) {
                            const p1Proxy = { id: '1', board: createBoardAPI('1'), stats: playerStates['1'].stats };
const p2Proxy = data.is2P ? { id: '2', board: createBoardAPI('2'), stats: playerStates['2'].stats } : null;
if (p1Proxy && p2Proxy) {
                                p1Proxy.opponent = p2Proxy;
p2Proxy.opponent = p1Proxy;
                            }
                            const gameApiProxy = { p1: p1Proxy, p2: p2Proxy, ...api };
const playerProxy = data.playerStateProxy.id === '1' ? p1Proxy : p2Proxy;
if (command === 'pieceLock') {
                                userFunctions.onPieceLock(gameApiProxy, playerProxy, data.moveInfo);
                                postMessage({ command: 'hookComplete', hook: 'pieceLock' });
} else {
                                userFunctions.onUpdate(gameApiProxy, playerProxy);
}
}
};
`;
             const ruleWorkerBlob = new Blob([ruleWorkerScript], { type: 'application/javascript' });
players.forEach(p => {
                p.ruleWorker = new Worker(URL.createObjectURL(ruleWorkerBlob));
                p.ruleWorker.onmessage = (e) => {
                    const { command, func, args, hooks } = e.data;
                    
     if (command === 'api') {
                          const getPlayerById = (id) => players.find(player => player.id === id);
                            switch(func) {
                         
       case 'win': getPlayerById(args[0])?.win(); break;
 
                               case 'lose': getPlayerById(args[0])?.lose(); break;
                                case 'setCustomUIText': getPlayerById(args[0])?.setCustomUIText(args[1]); break;
                    
           
 case 'sendAttack':
                                    const targetPlayer = getPlayerById(args[0]);
                                    if (targetPlayer) {
                                        targetPlayer.garbageQueue.push({
                                            lines: args[1],
                                            receivedTime: performance.now()
                                        });
                                    }
                                    break;
                                case 'forceSetting': 
                                    if (args[0] in currentRunSettings) currentRunSettings[args[0]] = args[1];
         
           
                break;
case 'displayGhostBlock': getPlayerById(args[0])?.displayGhostBlock(args[1], args[2], args[3]); break;
                                case 'clearAllGhostBlocks': getPlayerById(args[0])?.clearAllGhostBlocks(); break;
                                case 'setPiece': getPlayerById(args[0])?.setPiece(args[1], args[2], args[3]); break;
                                case 'placeBlock': getPlayerById(args[0])?.placeBlock(args[1], args[2], args[3]); break;
}
                        } 
else if (command === 'hooksAvailable') {
                        p.activeRuleHooks = hooks;
if(hooks.onInit){
                             Object.assign(gameSettings, currentRunSettings);
}
                    } else if (command === 'hookComplete' && e.data.hook === 'pieceLock') {
                        p.finishLockPiece();
                    }
                };
                const initialStateProxy = {
                        id: p.id,
                        board: p.board,
                        holdPiece: p.holdPiece,
                        currentPiece: p.player.pieceType,
                        nextQueue: p.nextQueue,
                        fullMinoSequence: p.fullMinoSequence,
                        stats: p.stats
                    };
p.ruleWorker.postMessage({
                    command: 'init',
                    data: {
                        ruleCode: editorData.rule.code,
                        startTime: startTime,
        
                is2P: gameMode === '2P',
                        initialPlayerState: initialStateProxy
                    }
                });
});
        }
        
        if (!editorData.rule.code) {
            Object.assign(gameSettings, currentRunSettings);
        }
});
    document.getElementById('gameSettingsBtn').addEventListener('click', () => {
        openUnifiedSettingsModal('general');
    });
    setupEditors();

    loadStateFromURL();

    lastTime = performance.now(); 
    requestAnimationFrame(gameLoop);

    
    window.addEventListener('resize', updateScale);

    setTimeout(updateScale, 100);

    document.getElementById('ai-debug-execute').addEventListener('click', () => {
        const piece = document.getElementById('ai-debug-piece').value.toUpperCase();
        const x = parseInt(document.getElementById('ai-debug-x').value, 10);
        const y = parseInt(document.getElementById('ai-debug-y').value, 10);
        const rot = parseInt(document.getElementById('ai-debug-rot').value, 10);

        if (!piece || isNaN(x) || isNaN(y) || isNaN(rot)) {
            alert('Invalid debug input.');
            return;
        }

        const aiPlayer = players.find(p => p.isAi);
        if (!aiPlayer) {
            alert('No AI player found to execute the move.');
            return;
        }
        
        const move = {
             pieceType: piece,
            x: x,
            y: y,
            rotation: rot
        };

        aiPlayer.executeAiMove(move);
    });

    document.getElementById('exportFumenBtn').addEventListener('click', () => {
        const recordedCollection = window.createRecordedReplayCollection?.();
        if (!recordedCollection) {
            alert('接着操作がまだ記録されていません。まずゲームを開始して、1個以上ミノを接着してください。');
            return;
        }
        const recordedJson = JSON.stringify(recordedCollection);
        const recordedBytes = new TextEncoder().encode(recordedJson);
        let recordedBinary = '';
        for (let index = 0; index < recordedBytes.length; index += 8192) {
            recordedBinary += String.fromCharCode(...recordedBytes.subarray(index, index + 8192));
        }
        const recordedBase64 = btoa(recordedBinary);

        gameState = 'EDITING';
        players.forEach(player => {
            player.aiWorker?.terminate();
            player.ruleWorker?.terminate();
        });
        players = [];
        loadGameSettings();
        document.getElementById('game-container').style.display = 'none';
        document.getElementById('game-controls').style.display = 'none';
        document.getElementById('editor-container').style.display = 'flex';
        document.getElementById('rule-description-display').style.display = 'none';
        virtualController.hide();
        setTimeout(updateScale, 0);

        let recordedEditorUrl = './F/index.html';
        try {
            const path = window.location.pathname;
            recordedEditorUrl = path.substring(0, path.lastIndexOf('/') + 1) + 'F/index.html';
        } catch (error) {
            console.warn('Could not resolve editor URL:', error);
        }
        if (window.parent !== window) {
            window.parent.postMessage({ target: 'editor', type: 'loadFumen', data: recordedCollection }, '*');
        } else {
            // Opening a new tab is frequently blocked when the simulator is
            // hosted inside an app shell. Navigate in the current tab so the
            // recorded operation-aware replay is always visible.
            window.location.href = `${recordedEditorUrl}#${recordedBase64}`;
        }
        return;
    });

        /* Legacy snapshot exporter kept below for reference; the active path
           above always exports the operation-aware v3 replay collection.
        if (gameHistoryLog.length === 0) {
            alert('記録するデータがありません。');
            return;
        }
        
        const compressedPages = [];
        let prevP1Board1D = null;
        let prevP2Board1D = null;

        for (let i = 0; i < gameHistoryLog.length; i++) {
            const page = gameHistoryLog[i];
            const pageData = {};

            // P1
            const currentP1Board1D = page.p1.b.split('');
            let p1BoardCompressed;
            if (i === 0) {
                // 1ページ目: 生データをRLE
                p1BoardCompressed = encodeRLE(currentP1Board1D);
            } else {
                // 2ページ目以降: 差分をRLE
                const diff = getDifference(prevP1Board1D, currentP1Board1D);
                p1BoardCompressed = encodeRLE(diff);
            }
            pageData.p1 = {
                b: p1BoardCompressed, // 圧縮データを格納
                h: page.p1.h || '',
                n: page.p1.n || ''
            };
            prevP1Board1D = currentP1Board1D; // 次の差分のために現在地を保存

            // P2 (2Pモード時)
            if (gameMode === '2P' && page.p2) {
                const currentP2Board1D = page.p2.b.split('');
                let p2BoardCompressed;
                if (i === 0) {
                    p2BoardCompressed = encodeRLE(currentP2Board1D);
                } else {
                    const diff = getDifference(prevP2Board1D, currentP2Board1D);
                    p2BoardCompressed = encodeRLE(diff);
                }
                pageData.p2 = {
                    b: p2BoardCompressed,
                    h: page.p2.h || '',
                    n: page.p2.n || ''
                };
                prevP2Board1D = currentP2Board1D;
            }
            
            compressedPages.push(pageData);
        }

        const fumenData = {
            v: 'f2',
            m: gameMode,
            p: compressedPages
        };

        let jsonString, base64Data;

        try {
            jsonString = JSON.stringify(fumenData);
            const uint8Array = new TextEncoder().encode(jsonString);
            
            let binaryString = '';
            const CHUNK_SIZE = 8192; 
            for (let i = 0; i < uint8Array.length; i += CHUNK_SIZE) {
                const chunk = uint8Array.subarray(i, i + CHUNK_SIZE);
                binaryString += String.fromCharCode.apply(null, chunk);
            }
            base64Data = btoa(binaryString);

        } catch (e) {
            console.error('譜面データのエンコードに失敗しました:', e);
            if (confirm('譜面データのエンコードに失敗しました。データが大きすぎる可能性があります。\n\n生の譜面データ(JSON)をクリップボードにコピーしますか？ (Base64ではありません)')) {
                navigator.clipboard.writeText(jsonString)
                    .then(() => {
                        alert('生の譜面データ(JSON)をクリップボードにコピーしました。');
                    })
                    .catch(err => {
                        console.error('クリップボードへのコピーに失敗しました:', err);
                        alert('クリップボードへのコピーに失敗しました。');
                    });
}
                return;
}
            
            
            gameState = 'EDITING';
            
            if (players.length > 0) {
                players.forEach(p => {
                    if (p.aiWorker) {
                        p.aiWorker.terminate();
                    }
                    if (p.ruleWorker) {
                        p.ruleWorker.terminate();
                    }
                });
            }
            
            players = [];
            loadGameSettings();
            document.getElementById('game-container').style.display = 'none';
            document.getElementById('game-controls').style.display = 'none';
            document.getElementById('editor-container').style.display = 'flex';
            document.getElementById('rule-description-display').style.display = 'none';
            
            virtualController.hide();
            
            const aiDebugDisplay = document.getElementById('ai-debug-display');
            const aiDebugControls = document.getElementById('ai-debug-controls');
            if(aiDebugDisplay) aiDebugDisplay.style.display = 'none';
            if(aiDebugControls) aiDebugControls.style.display = 'none';

            setTimeout(updateScale, 0);

        
            try {
                let fumenEditorURL 
= './F/index.html';
try {
                const path = window.location.pathname;
const parentPath = path.substring(0, path.lastIndexOf('/') + 1);
fumenEditorURL = parentPath + 'F/index.html';
} catch (e) { legacy fallback }

                if (window.parent !== window) {
                    window.parent.postMessage({
                        target: 'editor',
                        type: 'loadFumen',
                        data: fumenData // JSONオブジェクトをそのまま送信
                    }, '*');
                } else {
                    window.open(`${fumenEditorURL}#${base64Data}`, '_blank');
                }
            } catch (e) {
                console.error('譜面データの生成に失敗しました:', e);

if (confirm('リンクの生成に失敗しました。データが長すぎる可能性があります。\n譜面データ（Base64）をクリップボードにコピーしますか？')) {
                navigator.clipboard.writeText(base64Data)
                    .then(() => {
                        alert('譜面データをクリップボードにコピーしました。\n譜面エディタで手動で読み込んでください。');
                    })
                

    .catch(err => {
                                            console.error('クリップボードへのコピーに失敗しました:', err);
                        alert('クリップボードへのコピーに失敗しました。');
                    });
            }
}
    });

        */
    // Analysis is intentionally available only while Debug Mode is enabled.
    document.getElementById('analyzeBtn').addEventListener('click', () => {
        if (!gameSettings.debugEnabled) return;

        const modal = document.getElementById('analysis-modal');
        const statsDiv = document.getElementById('analysis-stats');
        modal.style.display = 'flex';

        const container = document.getElementById('analysis-canvas-container');
        container.innerHTML = '';
        container.style.display = 'flex';
        container.style.flexDirection = 'column';
        container.style.gap = '10px';

        const canvas1 = document.createElement('canvas');
        canvas1.style.flex = '1'; canvas1.style.width = '100%'; canvas1.style.backgroundColor = '#0f0f18'; canvas1.style.border = '1px solid #4b4b7c';
        const canvas2 = document.createElement('canvas');
        canvas2.style.flex = '1'; canvas2.style.width = '100%'; canvas2.style.backgroundColor = '#0f0f18'; canvas2.style.border = '1px solid #4b4b7c';
        container.appendChild(canvas1);
        container.appendChild(canvas2);

        // Resize logic
        canvas1.width = container.clientWidth; canvas1.height = container.clientHeight / 2 - 10;
        canvas2.width = container.clientWidth; canvas2.height = container.clientHeight / 2 - 10;

        const ctx1 = canvas1.getContext('2d');
        const ctx2 = canvas2.getContext('2d');

        if (analysisData.length === 0) {
            statsDiv.innerHTML = 'データがありません';
            return;
        }

        const resources = analysisData.filter(d => d.type === 'resource');
        const actions = analysisData.filter(d => d.type === 'action');
        if (resources.length === 0) return;

        const maxTime = resources[resources.length - 1].time;
        
        // --- 共通描画関数 ---
        const drawGrid = (ctx, w, h, maxY, xLabelFunc, yLabelFunc, padding = 40) => {
            const drawW = w - padding * 2;
            const drawH = h - padding * 2;
            ctx.strokeStyle = '#4b4b7c'; ctx.lineWidth = 1; ctx.beginPath();
            ctx.moveTo(padding, h - padding); ctx.lineTo(w - padding, h - padding); // X
            ctx.moveTo(padding, h - padding); ctx.lineTo(padding, padding); // Y
            ctx.stroke();

            ctx.fillStyle = '#e0e0e0'; ctx.font = '10px "Noto Sans JP", sans-serif'; ctx.textAlign = 'right';
            const yStep = maxY / 5;
            for(let v = 0; v <= maxY; v += yStep) {
                const y = h - padding - (v / maxY) * drawH;
                ctx.fillText(yLabelFunc ? yLabelFunc(v) : v.toFixed(1), padding - 5, y + 3);
                ctx.strokeStyle = '#333'; ctx.beginPath(); ctx.moveTo(padding, y); ctx.lineTo(w - padding, y); ctx.stroke();
            }
            ctx.textAlign = 'center';
            for(let t = 0; t <= maxTime; t += 10000) {
                const x = padding + (t / maxTime) * drawW;
                ctx.fillText(Math.floor(t / 1000) + 's', x, h - padding + 15);
            }
            return { drawW, drawH, padding };
        };

        // --- Graph 1: Resources ---
        const maxR = 200;
        const layout1 = drawGrid(ctx1, canvas1.width, canvas1.height, maxR, null, (v) => Math.round(v));
        
        // Title
        ctx1.fillStyle = '#e0e0e0'; ctx1.font = '14px "Noto Sans JP", sans-serif'; ctx1.textAlign = 'left';
        ctx1.fillText('リソース推移 (ブロック数)', layout1.padding, 20);

        const plot = (ctx, data, valueKey, color, layout, maxY) => {
            ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.beginPath();
            let first = true;
            for (const d of data) {
                const x = layout.padding + (d.time / maxTime) * layout.drawW;
                const y = canvas1.height - layout.padding - (d[valueKey] / maxY) * layout.drawH;
                if (first) { ctx.moveTo(x, y); first = false; } else { ctx.lineTo(x, y); }
            }
            ctx.stroke();
        };

        plot(ctx1, resources, 'p1_R', '#ff4444', layout1, maxR);
        if (gameMode === '2P') {
            plot(ctx1, resources, 'p2_R', '#4444ff', layout1, maxR);
            ctx1.strokeStyle = '#44ff44'; ctx1.lineWidth = 1; ctx1.setLineDash([5, 5]); ctx1.beginPath();
            let first = true;
            for (const d of resources) {
                const x = layout1.padding + (d.time / maxTime) * layout1.drawW;
                const y = canvas1.height - layout1.padding - ((d.p1_R + d.p2_R) / maxR) * layout1.drawH;
                if (first) { ctx1.moveTo(x, y); first = false; } else { ctx1.lineTo(x, y); }
            }
            ctx1.stroke(); ctx1.setLineDash([]);
        }

        // --- Graph 2: APL (Moving Average) ---
        const layout2 = drawGrid(ctx2, canvas2.width, canvas2.height, 3.0, null, (v) => v.toFixed(1));
        ctx2.fillStyle = '#e0e0e0'; ctx2.font = '14px "Noto Sans JP", sans-serif'; ctx2.textAlign = 'left';
        ctx2.fillText('APL推移 (直近10回の移動平均)', layout2.padding, 20);

        // APL=1.0 line
        const y1 = canvas2.height - layout2.padding - (1.0 / 3.0) * layout2.drawH;
        ctx2.strokeStyle = '#ffff00'; ctx2.lineWidth = 1; ctx2.setLineDash([2, 2]); 
        ctx2.beginPath(); ctx2.moveTo(layout2.padding, y1); ctx2.lineTo(canvas2.width - layout2.padding, y1); ctx2.stroke(); ctx2.setLineDash([]);

        const calculateMovingAvgAPL = (pId) => {
            const pActions = actions.filter(a => a.playerId === pId && a.lines > 0);
            const points = [];
            const windowSize = 10;
            for(let i = 0; i < pActions.length; i++) {
                let sumAtk = 0; let sumLines = 0;
                for(let j = Math.max(0, i - windowSize + 1); j <= i; j++) {
                    sumAtk += pActions[j].attack;
                    sumLines += pActions[j].lines;
                }
                if (sumLines > 0) points.push({ time: pActions[i].time, apl: sumAtk / sumLines });
            }
            return points;
        };

        const p1APL = calculateMovingAvgAPL('1');
        ctx2.strokeStyle = '#ff4444'; ctx2.lineWidth = 2; ctx2.beginPath();
        let first = true;
        for (const d of p1APL) {
            const x = layout2.padding + (d.time / maxTime) * layout2.drawW;
            const y = canvas2.height - layout2.padding - (Math.min(3.0, d.apl) / 3.0) * layout2.drawH;
            if (first) { ctx2.moveTo(x, y); first = false; } else { ctx2.lineTo(x, y); }
        }
        ctx2.stroke();

        if (gameMode === '2P') {
            const p2APL = calculateMovingAvgAPL('2');
            ctx2.strokeStyle = '#4444ff'; ctx2.lineWidth = 2; ctx2.beginPath();
            first = true;
            for (const d of p2APL) {
                const x = layout2.padding + (d.time / maxTime) * layout2.drawW;
                const y = canvas2.height - layout2.padding - (Math.min(3.0, d.apl) / 3.0) * layout2.drawH;
                if (first) { ctx2.moveTo(x, y); first = false; } else { ctx2.lineTo(x, y); }
            }
            ctx2.stroke();
        }

        // --- Statistics ---
        const calcStats = (pId) => {
            const pActs = actions.filter(a => a.playerId === pId);
            const totalLines = pActs.reduce((s, a) => s + a.lines, 0);
            const totalAttack = pActs.reduce((s, a) => s + a.attack, 0);
            const pps = pActs.length / (maxTime / 1000);
            const apm = totalAttack / (maxTime / 60000);
            const avgApl = totalLines > 0 ? totalAttack / totalLines : 0;
            return { pps, apm, avgApl };
        };

        const s1 = calcStats('1');
        const totalResources = resources.map(r => r.p1_R + (gameMode === '2P' ? r.p2_R : 0));
        const avgTotalRes = totalResources.reduce((a, b) => a + b, 0) / totalResources.length;
        
        let highToLow = 0; let highCount = 0;
        let lowToHigh = 0; let lowCount = 0;
        for(let i = 0; i < totalResources.length - 5; i++) {
            const current = totalResources[i];
            const future = totalResources[i+5]; // 1 sec later (approx)
            if (current > avgTotalRes + 10) {
                highCount++;
                if (future < current) highToLow++;
            } else if (current < avgTotalRes - 10) {
                lowCount++;
                if (future > current) lowToHigh++;
            }
        }
        const decreaseRate = highCount > 0 ? (highToLow / highCount * 100) : 0;
        const increaseRate = lowCount > 0 ? (lowToHigh / lowCount * 100) : 0;

        let html = `<div style="display:flex; flex-wrap:wrap; gap:15px; justify-content:center;">`;
        const cardStyle = 'background:rgba(255,255,255,0.05); padding:10px; border-radius:5px; border:1px solid #4b4b7c;';
        
        html += `<div style="${cardStyle}"><h4 style="margin:0 0 5px 0; color:#ff4444;">Player 1</h4>PPS: ${s1.pps.toFixed(2)}<br>APM: ${s1.apm.toFixed(1)}<br>APL: ${s1.avgApl.toFixed(2)}</div>`;
        
        if (gameMode === '2P') {
            const s2 = calcStats('2');
            html += `<div style="${cardStyle}"><h4 style="margin:0 0 5px 0; color:#4444ff;">Player 2</h4>PPS: ${s2.pps.toFixed(2)}<br>APM: ${s2.apm.toFixed(1)}<br>APL: ${s2.avgApl.toFixed(2)}</div>`;
        }

        html += `<div style="${cardStyle}"><h4 style="margin:0 0 5px 0; color:#44ff44;">総リソース</h4>平均値(収束点): ${avgTotalRes.toFixed(1)}<br>高リソース時減少率: ${decreaseRate.toFixed(1)}%<br>低リソース時増加率: ${increaseRate.toFixed(1)}%</div>`;
        html += `</div>`;
        
        statsDiv.innerHTML = html;
    });

    document.getElementById('analysis-close').addEventListener('click', () => {
        document.getElementById('analysis-modal').style.display = 'none';
    });

});
function logCurrentGameState(lockingPlayer = null) {
    if (players.length === 0) return;
const isInitialCall = gameHistoryLog.length === 0;

const getPlayerData = (player, isOpponentOfLocking) => {
        if (!player) return null;
let nextPieces;
        if (isInitialCall) {
            nextPieces = [
                player.player.pieceType, 
                ...player.nextQueue.slice(0, Math.max(0, gameSettings.maxNext - 1))
            ];
} else if (lockingPlayer && isOpponentOfLocking) {
            nextPieces = [
                player.player.pieceType,
                ...player.nextQueue.slice(0, Math.max(0, gameSettings.maxNext - 1))
            ];
        } else {
            nextPieces = [
                ...player.nextQueue.slice(0, Math.max(0, gameSettings.maxNext))
            ];
}

const next = nextPieces.filter(p => p !== null && p !== 'E').join('');
return {
            b: boardToString(player.board),
            h: player.holdPiece ||
'',
            n: next
        };
};

    const p1 = players[0];
    const p2 = (gameMode === '2P' && players[1]) ? players[1] : null;

    const p1IsOpponent = (lockingPlayer && p2 && lockingPlayer.id === p2.id);
    const p2IsOpponent = (lockingPlayer && p1 && lockingPlayer.id === p1.id);

    const p1Data = getPlayerData(p1, p1IsOpponent);
    const p2Data = getPlayerData(p2, p2IsOpponent);

const pageData = { p1: p1Data };
    if (p2Data) {
        pageData.p2 = p2Data;
}
    
    gameHistoryLog.push(pageData);
}
