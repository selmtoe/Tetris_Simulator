/* Keyboard bindings, preferences, skins, and settings modal behavior. */

function bindKey(binding) {
    if (!isBindingKey || !bindingPlayer || !bindingAction) return;
    
    keyBindings[bindingPlayer][bindingAction] = binding;
    isBindingKey = false;
    bindingAction = null;
    const tabToReopen = bindingPlayer === 'p1' ? 'p1-keys' : 'p2-keys';
    openUnifiedSettingsModal(tabToReopen);
}

function loadKeyBindings() {
    try {
        const savedBindings = JSON.parse(localStorage.getItem('tetrisKeyBindings'));
        if (savedBindings) {
            if (savedBindings.p1) Object.assign(keyBindings.p1, savedBindings.p1);
            if (savedBindings.p2) Object.assign(keyBindings.p2, savedBindings.p2);
        }
        // Profiles created before PC search was added have no entry.  Keep any
        // valid user assignment, but restore a usable default for old/corrupt
        // saved data so the P1 controls list always exposes the action.
        const pcSearchBinding = keyBindings.p1.pcSearch;
        if (!pcSearchBinding || typeof pcSearchBinding !== 'object' ||
            !['key', 'pad_button', 'pad_axis'].includes(pcSearchBinding.type) ||
            pcSearchBinding.value === undefined || pcSearchBinding.value === null) {
            keyBindings.p1.pcSearch = { ...DEFAULT_PC_SEARCH_BINDING };
        } else if (typeof pcSearchBinding.label !== 'string' || !pcSearchBinding.label) {
            pcSearchBinding.label = String(pcSearchBinding.value);
        }
    } catch (e) { console.error("Failed to load key bindings from localStorage:", e); }
}

function saveGameSettings() {
    try {
        localStorage.setItem('tetrisGameSettings', JSON.stringify(gameSettings));
    } catch (e) { console.error("Failed to save game settings to localStorage:", e); }
}
    
function loadGameSettings() {
    try {
        const saved = localStorage.getItem('tetrisGameSettings');
if (saved) {
            const parsed = JSON.parse(saved);
            const { aiType: discardedAiType, ...compatibleSettings } = parsed;
            // The first JS port exposed a 45 ms default and no persistent DAG
            // capacity setting.  Treat that exact combination as a migrated
            // default, not as an intentional low-strength custom profile.
            const migrateFirstPortBudget = compatibleSettings.aiThinkTime === 45 && !Number.isFinite(compatibleSettings.aiNodeLimit);
            // Upgrade the exact default object used by the former fake Worker.
            // Custom tuning is retained; only the recognizable old defaults are
            // replaced with Cold Clear Standard's real default coefficients.
            const savedWeights = compatibleSettings.aiWeights;
            if (savedWeights && savedWeights.soft_drop === -100 &&
                savedWeights.clear1 === -320 && savedWeights.clear2 === -200 &&
                savedWeights.clear3 === -178 && savedWeights.clear4 === 270 &&
                savedWeights.tspin1 === 1 && savedWeights.tspin2 === 290 && savedWeights.tspin3 === 482) {
                compatibleSettings.aiWeights = JSON.parse(JSON.stringify(DEFAULT_AI_WEIGHTS));
            }
            Object.assign(gameSettings, compatibleSettings);
            if (migrateFirstPortBudget) gameSettings.aiThinkTime = 180;
            if (!Number.isFinite(gameSettings.aiNodeLimit)) gameSettings.aiNodeLimit = 120000;
            gameSettings.aiType = 'cold-clear';
}
    } catch(e) { console.error('Failed to load settings from localStorage:', e);
}
}

const SKIN_STORAGE_KEY = 'tetrisCustomSkins';

function saveSkinsToLocalStorage() {
    try {
        const skinsToSave = {};
        const validTypes = ['I', 'O', 'T', 'L', 'J', 'S', 'Z', 'G', 'E', 'BG'];
        validTypes.forEach(key => {
            if (activeSkin[key] && activeSkin[key].src) {
                skinsToSave[key] = activeSkin[key].src;
            }
        });
        localStorage.setItem(SKIN_STORAGE_KEY, JSON.stringify(skinsToSave));
    } catch (e) {
        console.error("Failed to save skins to localStorage:", e);
    }
}

function loadSkinsFromLocalStorage() {
    try {
        const savedSkins = localStorage.getItem(SKIN_STORAGE_KEY);
        if (savedSkins) {
            const parsedSkins = JSON.parse(savedSkins);
            const validTypes = ['I', 'O', 'T', 'L', 'J', 'S', 'Z', 'G', 'E', 'BG'];
            let skinsLoaded = false;
            Object.keys(parsedSkins).forEach(key => {
                if (validTypes.includes(key) && parsedSkins[key]) {
                    activeSkin[key] = new Image();
                    activeSkin[key].src = parsedSkins[key];
                    skinsLoaded = true;
                }
            });
            return skinsLoaded;
        }
    } catch (e) {
        console.error("Failed to load skins from localStorage:", e);
    }
    return false;
}

function resetSkins() {
    if (!confirm('保存されているカスタムスキンをすべて削除し、デフォルトに戻しますか？')) return;
    try {
        localStorage.removeItem(SKIN_STORAGE_KEY);
        activeSkin = MINO_SKINS.default;
        ['I', 'O', 'T', 'L', 'J', 'S', 'Z', 'G', 'E', 'BG'].forEach(k => MINO_SKINS.default[k] = new Image());
        activeSkin = MINO_SKINS.default;
        
        ['p1', 'p2'].forEach(setupPlayerEditor);
        ['p1', 'p2'].forEach(updateNextQueueDisplay);
        ['p1', 'p2'].forEach(drawEditorField);
        
        alert('スキンをリセットしました。');
    } catch (e) {
        console.error("Failed to reset skins:", e);
        alert('スキンのリセットに失敗しました。');
    }
}

function populateGeneralSettingsTab() {
    const list = document.getElementById('settings-list');
    list.innerHTML = '';
    const settingDetails = {
        das: { label: 'DAS (ms)', min: 0, max: 500, step: 10 },
        arr: { label: 'ARR (ms)', min: 0, max: 100, step: 1 },
        sdf: { label: 'SDF (ms)', min: 0, max: 100, step: 1 },
        lineClearDelay: { label: 'ライン消去時間 (ms)', min: 0, max: 2000, step: 50 },
        spawnDelay: { label: '設置時硬直時間 (ms)', min: 0, max: 2000, step: 50 },
        gravity: { label: '落下間隔時間 (ms)', min: 0, max: 9999999, step: 50 },
        lockDelay: { label: '設置猶予時間 (ms)', min: 0, max: 9999999, step: 50 },
        maxNext: { label: 'ネクスト表示数', min: 1, max: 8, step: 1 },
        garbageGrace: { label: 'おじゃま猶予時間 (ms)', min: 0, max: 5000, step: 100 },
        garbageRandomness: { label: '穴バラ率 (%)', min: 0, max: 100, step: 1 }
    };
    Object.keys(settingDetails).forEach(key => {
        const item = document.createElement('div'); item.className = 'setting-item';
        const label = document.createElement('span'); label.textContent = settingDetails[key].label;
        const input = document.createElement('input'); input.type = 'number';
        Object.assign(input, settingDetails[key]);
        let currentValue = gameSettings[key];
        if (key === 'garbageRandomness') currentValue *= 100;
        input.value = currentValue;
        input.onchange = e => {
            let val = parseFloat(e.target.value);
            if (key === 'garbageRandomness') gameSettings[key] = Math.max(0, Math.min(100, val)) / 100;
            else gameSettings[key] = Math.max(settingDetails[key].min, Math.min(settingDetails[key].max, val));
        };
        item.append(label, input); list.appendChild(item);
    });
    
    const checkboxSettings = { showEffects: 'エフェクトを表示する', showTimer: 'タイマーを表示する', touchControlsEnabled: 'タッチ操作を有効にする (P1)' };
    Object.entries(checkboxSettings).forEach(([key, text], index) => {
        const item = document.createElement('div'); item.className = 'setting-item';
        if (index === 0) { item.style.cssText = 'margin-top:20px; border-top:1px solid var(--primary-color); padding-top:15px;'; }
        const label = document.createElement('label'); const checkbox = document.createElement('input');
        checkbox.type = 'checkbox'; checkbox.id = `setting-${key}`; checkbox.checked = !!gameSettings[key];
        checkbox.onchange = e => { gameSettings[key] = e.target.checked; };
        const span = document.createElement('span'); span.textContent = text;
 

       label.append(checkbox, span); item.appendChild(label); list.appendChild(item);
    });
}

function populateKeyConfigTab(playerId) {
    const list = document.getElementById(`${playerId}-key-config-list`);
    list.innerHTML = ''; 
    Object.keys(keyBindings[playerId]).forEach(action => {
        const item = document.createElement('div'); item.className = 'key-config-item';
        const label = document.createElement('span'); label.textContent = keyActionLabels[action];
        const btn = document.createElement('button'); btn.className = 'button';
        btn.textContent = keyBindings[playerId][action].label;
        btn.onclick = () => {
            isBindingKey = true; bindingPlayer = playerId; bindingAction = action;
            btn.textContent = 
'入力待機中...';
            list.querySelectorAll('button').forEach(b => { if (b !== btn) b.disabled = true; });
        };
        item.appendChild(label); item.appendChild(btn); list.appendChild(item);
    });
    if (playerId === 'p1') {
        const drawMoveDelayInput = document.getElementById('draw-move-delay-input');
        if (drawMoveDelayInput) {
            drawMoveDelayInput.value = gameSettings.drawMoveDelay;
            drawMoveDelayInput.onchange = e => {
                const val = parseInt(e.target.value, 10);
                if (!isNaN(val)) gameSettings.drawMoveDelay = Math.max(0, Math.min(200, val));
            };
        }
        const touchModeButtons = document.querySelectorAll('#p1-touch-mode-selection .button');
        touchModeButtons.forEach(btn => {
            btn.classList.toggle('active', btn.dataset.touchMode === gameSettings.touchControlType);
        });
        document.getElementById('p1-touch-button-controls').style.display = gameSettings.touchControlType === 'button' ? 'flex' : 'none';
        document.getElementById('p1-touch-draw-controls').style.display = gameSettings.touchControlType === 'draw' ? 'flex' : 'none';
    }
}

function populateAiSettingsTab() {
    const list = document.getElementById('ai-settings-list');
    list.innerHTML = '';

    const itemType = document.createElement('div');
    itemType.className = 'setting-item';
    const labelType = document.createElement('span');
    labelType.textContent = 'AIの種類';
    
    const selectType = document.createElement('select');
    selectType.id = 'ai-type-select';
    selectType.style.minWidth = '150px';
    selectType.style.backgroundColor = 'var(--primary-color)';
    selectType.style.color = 'var(--font-color)';
    selectType.style.border = '1px solid var(--border-color)';
    selectType.style.padding = '5px';
    selectType.style.borderRadius = '4px';
    selectType.innerHTML = '<option value="cold-clear">Cold Clear</option>';
    selectType.disabled = true;
    gameSettings.aiType = 'cold-clear';

    itemType.append(labelType, selectType);
    list.appendChild(itemType);

    const settingDetails = {
        aiThinkTime: { label: 'AI思考時間 (ms)', min: 8, max: 500, step: 1 },
        aiNodeLimit: { label: 'AI DAG node 上限', min: 5000, max: 200000, step: 5000 },
        aiMoveDelay: { label: 'AIの操作入力間隔 (ms)', min: 20, max: 500, step: 1 },
        aiSdfDelay: { label: 'AI SDF (ms)', min: 0, max: 500, step: 1 }
    };
    
        Object.keys(settingDetails).forEach(key => {
        const item = document.createElement('div');
        item.className = 'setting-item';
        const label = document.createElement('span');
        label.textContent = settingDetails[key].label;
        const input = document.createElement('input');
        input.type = 'number';
        Object.assign(input, settingDetails[key]);
        input.value = gameSettings[key];
        input.onchange = e => {
            gameSettings[key] = Math.max(settingDetails[key].min, Math.min(settingDetails[key].max, parseInt(e.target.value, 10)));
        };
                item.append(label, input);
        list.appendChild(item);
    });
    const pfpItem = document.createElement('div');
    pfpItem.className = 'setting-item';
    
    const pfpLabel = document.createElement('label');
    const pfpCheckbox = document.createElement('input');
    pfpCheckbox.type = 'checkbox';
    pfpCheckbox.checked = !!gameSettings.pieceForPieceMode;
    pfpCheckbox.onchange = e => {
        gameSettings.pieceForPieceMode = e.target.checked;
    };
    
const pfpSpan = document.createElement('span');
pfpSpan.textContent = 'Piece For Piece';
pfpLabel.append(pfpCheckbox, pfpSpan);
pfpItem.appendChild(pfpLabel);
list.appendChild(pfpItem);

const banPcItem = document.createElement('div');
banPcItem.className = 'setting-item';

const banPcLabel = document.createElement('label');
const banPcCheckbox = document.createElement('input');
banPcCheckbox.type = 'checkbox';
banPcCheckbox.checked = !!gameSettings.banPC;
banPcCheckbox.onchange = e => {
    gameSettings.banPC = e.target.checked;
};
const banPcSpan = document.createElement('span');
banPcSpan.textContent = 'PC禁止';
banPcLabel.append(banPcCheckbox, banPcSpan);
banPcItem.appendChild(banPcLabel);

list.appendChild(banPcItem);
if (gameSettings.debugEnabled) {

        const weightHeader = document.createElement('div');
        weightHeader.className = 'setting-item';
        weightHeader.style.cssText = 'margin-top:20px; border-top:1px solid var(--primary-color); padding-top:15px; font-weight:bold; flex-direction: column; gap: 10px;';
        weightHeader.innerHTML = '<span>AI重み設定 (Debug)</span>';
        
        const controlsDiv = document.createElement('div');
        controlsDiv.style.display = 'flex';
        controlsDiv.style.gap = '10px';
        
        const copyBtn = document.createElement('button');
        copyBtn.className = 'button';
        copyBtn.textContent = '設定をコピー';
        copyBtn.onclick = () => {
            navigator.clipboard.writeText(JSON.stringify(gameSettings.aiWeights, null, 2));
            alert('AI設定をクリップボードにコピーしました');
        };
        
        const pasteBtn = document.createElement('button');
        pasteBtn.className = 'button';
        pasteBtn.textContent = '設定をペースト';
        pasteBtn.onclick = async () => {
            try {
                const text = await navigator.clipboard.readText();
                const json = JSON.parse(text);
                Object.assign(gameSettings.aiWeights, json);
                populateAiSettingsTab();
                alert('AI設定を読み込みました');
            } catch(e) {
                alert('読み込み失敗: ' + e);
            }
        };

        const resetBtn = document.createElement('button');
        resetBtn.className = 'button';
        resetBtn.textContent = '初期値に戻す';
        resetBtn.onclick = () => {
            if(confirm('AI設定を初期値に戻しますか？')) {
                gameSettings.aiWeights = JSON.parse(JSON.stringify(DEFAULT_AI_WEIGHTS));
                populateAiSettingsTab();
            }
        };

        controlsDiv.append(copyBtn, pasteBtn, resetBtn);
        weightHeader.appendChild(controlsDiv);
        list.appendChild(weightHeader);

        Object.keys(gameSettings.aiWeights).forEach(key => {
            const val = gameSettings.aiWeights[key];
            const isArray = Array.isArray(val);
            
            const item = document.createElement('div');
            item.className = 'setting-item';
            
            const label = document.createElement('span');
            label.textContent = key;
            
            const input = document.createElement('input');
            if (isArray) {
                input.type = 'text';
                input.value = JSON.stringify(val);
                input.style.width = '200px';
                input.onchange = (e) => {
                    try {
                        gameSettings.aiWeights[key] = JSON.parse(e.target.value);
                    } catch(err) {
                        alert('配列の形式が不正です');
                        e.target.value = JSON.stringify(gameSettings.aiWeights[key]);
                    }
                };
            } else {
                input.type = 'number';
                input.value = val;
                input.onchange = (e) => {
                    gameSettings.aiWeights[key] = parseFloat(e.target.value);
                };
            }
            
            item.append(label, input);
            list.appendChild(item);
        });
    }

}

function populateLayoutSettingsTab() {
    const list = document.getElementById('layout-settings-list');
    list.innerHTML = '';
if (!gameSettings.layout) {
        gameSettings.layout = generateDefaultLayout();
}
    const layout = gameSettings.layout;

    // --- Custom Skins (Moved from General) ---
    const skinItem = document.createElement('div');
    skinItem.className = 'setting-item';
    skinItem.style.cssText = 'flex-direction: column; align-items: flex-start; gap: 10px; margin-bottom: 20px;';
    
    const skinLabel = document.createElement('label');
    skinLabel.textContent = 'カスタムスキン (I,O,T,L,J,S,Z,G,BG.png)';
    
    const skinInput = document.createElement('input');
    skinInput.type = 'file';
    skinInput.multiple = true;
    skinInput.accept = 'image/*';
    skinInput.style.display = 'block';
    skinInput.style.minWidth = '0';
    
    skinInput.addEventListener('change', (e) => {
        const files = e.target.files;
        if (!files) return;
        
        let filesLoaded = 0;
        const totalFiles = files.length;
        
        const redrawEditors = () => {
            if (++filesLoaded === totalFiles) {
                 ['p1', 'p2'].forEach(setupPlayerEditor);
                ['p1', 'p2'].forEach(updateNextQueueDisplay);
                ['p1', 'p2'].forEach(drawEditorField);
                alert(`${totalFiles}個のスキンを読み込みました。`);
            }
        };
        
        for (const file of files) {
            const pieceType = file.name.split('.')[0].toUpperCase();
            const validTypes = ['I', 'O', 'T', 'L', 'J', 'S', 'Z', 'G', 'E', 'BG'];
            
            if (validTypes.includes(pieceType)) {
                const reader = new FileReader();
                reader.onload = (re) => {
                    activeSkin[pieceType].src = re.target.result;
                    activeSkin[pieceType].onload = () => {
                        redrawEditors();
                        saveSkinsToLocalStorage();
                    };
                    activeSkin[pieceType].onerror = () => {
                        redrawEditors();
                        saveSkinsToLocalStorage();
                    };
                };
                reader.onerror = redrawEditors;
                reader.readAsDataURL(file);
            } else {
                redrawEditors();
            }
        }
    });
    
    const skinResetButton = document.createElement('button');
    skinResetButton.className = 'button';
    skinResetButton.textContent = 'カスタムスキンをリセット';
    skinResetButton.style.marginTop = '10px';
    skinResetButton.onclick = resetSkins;
    
    skinItem.append(skinLabel, skinInput, skinResetButton);
    list.appendChild(skinItem);
    // --- Global Settings ---
    const globalHeader = document.createElement('h3');
globalHeader.textContent = '全体設定';
    globalHeader.style.marginTop = '10px';
    list.appendChild(globalHeader);

    // Block Size
    const blockSizeItem = document.createElement('div');
    blockSizeItem.className = 'setting-item';
const bsLabel = document.createElement('span'); bsLabel.textContent = 'ブロックサイズ (px)';
    const bsInput = document.createElement('input'); bsInput.type = 'number';
    bsInput.value = layout.blockSize;
bsInput.min = 10; bsInput.max = 100;
    bsInput.onchange = (e) => { layout.blockSize = parseInt(e.target.value, 10); updateScale(); };
    blockSizeItem.append(bsLabel, bsInput);
    list.appendChild(blockSizeItem);
const uiBlockSizeItem = document.createElement('div');
    uiBlockSizeItem.className = 'setting-item';
    const uibsLabel = document.createElement('span'); uibsLabel.textContent = 'UIブロックサイズ (px)';
    const uibsInput = document.createElement('input');
uibsInput.type = 'number';
    uibsInput.value = layout.uiBlockSize || layout.blockSize;
    uibsInput.min = 10; uibsInput.max = 100;
uibsInput.onchange = (e) => { layout.uiBlockSize = parseInt(e.target.value, 10); updateScale(); };
    uiBlockSizeItem.append(uibsLabel, uibsInput);
    list.appendChild(uiBlockSizeItem);
// Background Image
    const bgItem = document.createElement('div');
    bgItem.className = 'setting-item';
    bgItem.style.flexDirection = 'column'; bgItem.style.alignItems = 'flex-start';
const bgLabel = document.createElement('span'); bgLabel.textContent = '背景画像';
    const bgInput = document.createElement('input'); bgInput.type = 'file'; bgInput.accept = 'image/*';
    bgInput.style.display = 'block';
// CSSで非表示になっているため明示的に表示
    bgInput.style.marginTop = '5px';
    bgInput.onchange = (e) => {
        const file = e.target.files[0];
if (file) {
            const reader = new FileReader();
reader.onload = (evt) => {
                layout.backgroundImage = evt.target.result;
layout._bgImageCache = null;
                alert('背景画像を設定しました。');
            };
            reader.readAsDataURL(file);
        }
    };
    const bgClearBtn = document.createElement('button'); bgClearBtn.className = 'button';
bgClearBtn.textContent = '背景をクリア'; bgClearBtn.style.marginTop = '5px';
    bgClearBtn.onclick = () => { layout.backgroundImage = null; alert('背景をクリアしました。'); };
    
    bgItem.append(bgLabel, bgInput, bgClearBtn);
    list.appendChild(bgItem);
// --- Player Layouts ---
    const createCoordInputs = (label, obj, keyX, keyY) => {
        const div = document.createElement('div');
div.className = 'setting-item';
        div.style.flexWrap = 'wrap';
        
        const title = document.createElement('span');
        title.textContent = label;
        title.style.width = '100%';
        title.style.marginBottom = '5px';
const xLabel = document.createElement('span'); xLabel.textContent = 'X:';
        const xInput = document.createElement('input'); xInput.type = 'number';
        xInput.value = obj[keyX]; xInput.style.width = '60px';
xInput.onchange = (e) => { obj[keyX] = parseInt(e.target.value, 10); };

        const yLabel = document.createElement('span'); yLabel.textContent = 'Y:';
const yInput = document.createElement('input'); yInput.type = 'number';
        yInput.value = obj[keyY]; yInput.style.width = '60px';
yInput.onchange = (e) => { obj[keyY] = parseInt(e.target.value, 10); };

        div.append(title, xLabel, xInput, yLabel, yInput);
        return div;
    };
const playersList = (gameMode === '1P') ? ['p1'] : ['p1', 'p2'];
playersList.forEach(pid => {
        const pHeader = document.createElement('h3');
        pHeader.textContent = `Player ${pid.replace('p','')}`;
        list.appendChild(pHeader);
        const pLayout = layout[pid];

        // Board
        list.appendChild(createCoordInputs('盤面位置', pLayout.board, 'x', 'y'));
        
        // Hold
        list.appendChild(createCoordInputs('HOLD位置', pLayout.hold, 'x', 'y'));

        // Next (Start + Offset)
        const nextDiv = document.createElement('div');
        nextDiv.className = 'setting-item';
        nextDiv.style.flexWrap = 'wrap';
        
        const nextTitle = document.createElement('span');
        nextTitle.textContent = 'NEXT (開始位置 & 間隔)';
        nextTitle.style.width = '100%'; nextTitle.style.marginBottom = '5px';

        const nxLabel = document.createElement('span'); nxLabel.textContent = 'Start X:';
      
  const nxInput = document.createElement('input'); nxInput.type = 'number';
        nxInput.value = pLayout.next[0].x;
nxInput.style.width = '50px';

        const nyLabel = document.createElement('span'); nyLabel.textContent = 'Start Y:';
        const nyInput = document.createElement('input'); nyInput.type = 'number';
nyInput.value = pLayout.next[0].y; nyInput.style.width = '50px';

        const offsetYLabel = document.createElement('span'); offsetYLabel.textContent = 'Step Y:';
        const offsetYInput = document.createElement('input');
offsetYInput.type = 'number';
        // 推定オフセット
        const currentStep = (pLayout.next.length > 1) ?
(pLayout.next[1].y - pLayout.next[0].y) : (layout.blockSize * 2.5);
        offsetYInput.value = currentStep; offsetYInput.style.width = '50px';
const updateNextArray = () => {
            const startX = parseInt(nxInput.value, 10);
const startY = parseInt(nyInput.value, 10);
            const step = parseInt(offsetYInput.value, 10);
pLayout.next = Array.from({ length: 8 }).map((_, i) => ({
                x: startX,
                y: startY + (i * step)
            }));
};

        nxInput.onchange = updateNextArray;
        nyInput.onchange = updateNextArray;
        offsetYInput.onchange = updateNextArray;

        nextDiv.append(nextTitle, nxLabel, nxInput, nyLabel, nyInput, offsetYLabel, offsetYInput);
        list.appendChild(nextDiv);
    });

    // --- Reset Layout Button (Moved to bottom) ---
    const resetLayoutBtn = document.createElement('button');
    resetLayoutBtn.className = 'button';
    resetLayoutBtn.style.width = '100%';
    resetLayoutBtn.style.marginTop = '20px';
    resetLayoutBtn.textContent = 'レイアウト設定を全て初期化';
    resetLayoutBtn.onclick = () => {
        if (confirm('レイアウト設定を全て初期状態に戻しますか？')) {
            gameSettings.layout = generateDefaultLayout();
            updateScale();
            populateLayoutSettingsTab(); // UI Refresh
        }
    };
    list.appendChild(resetLayoutBtn);
}

function openUnifiedSettingsModal(initialTab = 'general') {
    const modal = document.getElementById('settings-modal');
    
    const p2TabBtn = document.getElementById('p2-keys-tab-btn');
if (gameMode === '2P') {
        p2TabBtn.style.display = 'block';
} else {
        p2TabBtn.style.display = 'none';
        if (initialTab === 'p2-keys') initialTab = 'general';
}

    populateGeneralSettingsTab();
    populateLayoutSettingsTab();
    populateKeyConfigTab('p1');
    populateAiSettingsTab();
    if (gameMode === '2P') {
        populateKeyConfigTab('p2');
}

    const tabs = modal.querySelectorAll('.tab-button');
    const contents = modal.querySelectorAll('.tab-content');
    
    function switchTab(tabName) {
        tabs.forEach(tab => tab.classList.toggle('active', tab.dataset.tab === tabName));
        contents.forEach(content => content.classList.toggle('active', content.id === `tab-content-${tabName}`));
    }
    
    tabs.forEach(tab => {
        tab.addEventListener('click', () => switchTab(tab.dataset.tab));
    });

    switchTab(initialTab);
    modal.style.display = 'flex';
}
