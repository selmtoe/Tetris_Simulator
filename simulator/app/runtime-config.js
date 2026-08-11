/* Shared simulator constants, colors, settings, and mutable runtime state. */

const RESOLUTION_SCALE = 2;
const BLOCK_SIZE = 28;
const EDITOR_BLOCK_SIZE = 50;
const BOARD_WIDTH = 10;
const BOARD_VISIBLE_HEIGHT = 20;
const BOARD_HEIGHT = 40; 
const HOLD_AREA_WIDTH = 5 * BLOCK_SIZE;
const PLAYFIELD_WIDTH = BOARD_WIDTH * BLOCK_SIZE;
const NEXT_AREA_WIDTH = 5 * BLOCK_SIZE;
const PADDING = 20;
const PLAYER_CANVAS_WIDTH = HOLD_AREA_WIDTH + PLAYFIELD_WIDTH + NEXT_AREA_WIDTH + PADDING * 2;
const CANVAS_HEIGHT = (BOARD_VISIBLE_HEIGHT + 0.5) * BLOCK_SIZE;
const PLAYFIELD_X_OFFSET = HOLD_AREA_WIDTH + PADDING;
const AXIS_THRESHOLD = 0.8;

// The previous Worker exposed hand-written opening templates.  Cold Clear's
// Standard mode does not force those templates; opening books are a separate
// optional feature and no book data is bundled here.
const AI_TEMPLATE_CONFIG = [];

const DEFAULT_AI_WEIGHTS = {
    back_to_back: 52,
    bumpiness: -24,
    bumpiness_sq: -7,
    row_transitions: -5,
    height: -39,
    top_half: -150,
    top_quarter: -511,
    jeopardy: -11,
    cavity_cells: -173,
    cavity_cells_sq: -3,
    overhang_cells: -34,
    overhang_cells_sq: -1,
    covered_cells: -17,
    covered_cells_sq: -1,
    tslot: [8, 148, 192, 407],
    well_depth: 57,
    max_well_depth: 17,
    well_column: [20, 23, 20, 50, 59, 21, 59, 10, -10, 24],
    b2b_clear: 104,
    clear1: -143,
    clear2: -100,
    clear3: -58,
    clear4: 390,
    tspin1: 121,
    tspin2: 410,
    tspin3: 602,
    mini_tspin1: -158,
    mini_tspin2: -93,
    perfect_clear: 999,
    combo_garbage: 150,
    move_time: -3,
    wasted_t: -152
};

// デフォルトのレイアウト生成関数
function generateDefaultLayout() {
    const blockSize = 28;
    // 従来の定数計算に基づく配置
    const p1OriginX = 0;
    const p2OriginX = (5 * blockSize + 10 * blockSize + 5 * blockSize + 20 * 2);
    // PLAYER_CANVAS_WIDTH

    const createPlayerLayout = (offsetX) => ({
        board: { x: offsetX + (5 * blockSize + 20), y: 0.5 * blockSize },
        hold: { x: offsetX + (5 * blockSize / 2), y: 70 },
        next: Array.from({ length: 8 }).map((_, i) => ({
                    x: offsetX + 
(5 * blockSize + 20 + 10 * blockSize + 20 + 5 * blockSize / 2),
            y: 70 + (i * blockSize * 2.5)
        }))
    });
    return {
        backgroundImage: null, // 設定されている場合、個別背景やUIラベルを描画しない
        blockSize: blockSize,
        uiBlockSize: blockSize,
        p1: createPlayerLayout(p1OriginX),
        p2: createPlayerLayout(p2OriginX)
    };
}  

let gameSettings = {
    aiType: 'cold-clear',
    das: 140,
    arr: 30,
    sdf: 20,
    lineClearDelay: 0,
    spawnDelay: 0,
    gravity: 9999999,
    lockDelay: 9999999,
    garbageGrace: 1000,
    garbageRandomness: 0.3,
    maxNext: 8,
    showEffects: true,
    showTimer: false,
    touchControlsEnabled: 'ontouchstart' in window,
    touchControlType: 'button',
    aiMoveDelay: 30,
    aiSdfDelay: 30,
    aiThinkTime: 180,
    aiNodeLimit: 120000,
    drawMoveDelay: 30,
    aiTemplates: { meisou: true, sangaku_2: 
true },
    aiWeights: JSON.parse(JSON.stringify(DEFAULT_AI_WEIGHTS)),
    debugEnabled: false,
    pieceForPieceMode: false,
    banPC: false,
   
 layout: generateDefaultLayout() 
};

const DRAW_SHAPE_MAP = {
    "1,0;2,0;3,0": { type: "I", rot: 0, offset: [0, 0] },
    "0,1;0,2;0,3": { type: "I", rot: 1, offset: [2, -1] },
    "0,1;1,0;1,1": { type: "O", rot: 0, offset: [0, -1] },
    "-1,1;0,1;1,1": { type: "T", rot: 0, offset: [0, -1] },
    "0,1;0,2;1,1": { type: "T", rot: 1, offset: [0, -1] },
    "1,0;1,1;2,0": { type: "T", rot: 2, offset: [-1, 0] },
    "-1,1;0,1;0,2": { type: "T", rot: 3, offset: [0, -1] },
    "-1,1;-2,1;0,1": { type: "L", rot: 0, offset: [1, -1] },
    "0,1;0,2;1,2": { type: "L", rot: 1, offset: [0, -1] },
    "0,1;1,0;2,0": { type: "L", rot: 2, offset: [-1, 0] },
    "1,0;1,1;1,2": { type: "L", rot: 3, offset: [-1, -1] },
    "0,1;1,1;2,1": { type: "J", rot: 0, offset: [-1, -1] },
    "0,1;0,2;1,0": { type: "J", rot: 1, offset: [0, -1] },
    "1,0;2,0;2,1": { type: "J", rot: 2, offset: [-1, 0] },
    "-1,2;0,1;0,2": { type: "J", rot: 3, offset: [0, -1] 
},
    "-1,1;0,1;1,0": { type: "S", rot: 0, offset: [0, -1] },
    "0,1;1,1;1,2": { type: "S", rot: 1, offset: [0, -1] },
    "1,0;1,1;2,1": { type: "Z", rot: 0, offset: [-1, -1] },
    "-1,1;-1,2;0,1": { type: "Z", rot: 1, offset: [1, -1] },
};
const COLORS = { 'I': '#00f0f0', 'O': '#f0f000', 'T': '#a000f0', 'L': '#f0a000', 'J': '#0000f0', 'S': '#00f000', 'Z': '#f00000', 'G': '#999999', 'E': '#808080' };
const SCAN_COLOR_PALETTE = {
    'NULL': ['#000000', '#302838'],
    'G':    ['#999999', '#D8D8D8'],
    'I':    ['#019899', '#0199D5'],
    'O':    ['#999A02', '#F9B900'],
    'T':    ['#980099', '#871E88'],
    'L':    ['#996700', '#F56100'],
    'J':    ['#0000BB', '#004BA5'],
    'S':    ['#10971F', '#5CB523'],
    'Z':    ['#990000', '#DA1822']
};
const hexToRgb = (hex) => {
    const r = parseInt(hex.slice(1, 3), 16);
const g = parseInt(hex.slice(3, 5), 16);
    const b = parseInt(hex.slice(5, 7), 16);
    return { r, g, b };
};
const PARSED_SCAN_COLORS = {};
for (const key in SCAN_COLOR_PALETTE) {
    PARSED_SCAN_COLORS[key] = SCAN_COLOR_PALETTE[key].map(hexToRgb);
}


const SCAN_COLORS = { ...COLORS, 'O': '#999A02', 'L': '#f0a000' };
const activeSkinColors = { ...COLORS };
const MINO_SKINS = { default: {} };
['I', 'O', 'T', 'L', 'J', 'S', 'Z', 'G', 'E', 'BG'].forEach(k => MINO_SKINS.default[k] = new Image());
let activeSkin = MINO_SKINS.default;

const EDITOR_COLORS = {...COLORS, 'EMPTY': '#000000'};
const TETROMINOS = {
    'I': { shape: [[0, 0], [1, 0], [2, 0], [3, 0]], center: [1.5, 0.5] }, 'O': { shape: [[0, 0], [1, 0], [0, -1], [1, -1]], center: [0.5, -0.5] },
    'T': { shape: [[0, 0], [-1, 0], [0, -1], [1, 0]], center: [0, 0] }, 'L': { shape: [[-1, 0], [0, 0], [1, 0], [1, -1]], center: [0, 0] },
    'J': { shape: [[0, 0], [-1, 0], [1, 0], [-1, -1]], center: [0, 0] }, 'S': { shape: [[1, -1], [-1, 0], [0, 0], [0, -1]], center: [0, 0] },
    'Z': { shape: [[0, 0], [1, 0], [0, -1], [-1, -1]], center: [0, 0] }
};
const SRS_OFFSETS = { "JLSTZ": { "0_1": [[0, 0], [-1, 0], [-1, 1], [0, -2], [-1, -2]], "1_0": [[0, 0], [1, 0], [1, -1], [0, 2], [1, 2]], "1_2": [[0, 0], [1, 0], [1, -1], [0, 2], [1, 2]], "2_1": [[0, 0], [-1, 0], [-1, 1], [0, -2], [-1, -2]], "2_3": [[0, 0], [1, 0], [1, 1], [0, -2], [1, -2]], "3_2": [[0, 0], [-1, 0], [-1, -1], [0, 2], [-1, 2]], "3_0": [[0, 0], [-1, 0], [-1, -1], [0, 2], [-1, 2]], "0_3": [[0, 0], [1, 0], [1, 1], [0, -2], [1, -2]], }, "I": { "0_1": [[0, 0], [-2, 0], [1, 0], [-2, -1], [1, 2]], "1_0": [[0, 0], [2, 0], [-1, 0], [2, 1], [-1, -2]], "1_2": [[0, 0], [-1, 0], [2, 0], [-1, 2], [2, -1]], "2_1": [[0, 0], [1, 0], [-2, 0], [1, -2], [-2, 1]], "2_3": [[0, 0], [2, 0], [-1, 0], [2, 1], [-1, -2]], "3_2": [[0, 0], [-2, 0], [1, 0], [-2, -1], [1, 2]], "3_0": [[0, 0], [1, 0], [-2, 0], [1, -2], [-2, 1]], "0_3": [[0, 0], [-1, 0], [2, 0], [-1, 2], [2, -1]], } };

let players = [], gameMode = '1P', gameState = 'EDITING', lastTime = 0, mainCanvas, ctx, gameStartTime = 0;
let gameHistoryLog = [];
let analysisData = []; // { time, p1_R, p2_R, offsetLines }
function updateAiDebugDisplay(payload) {
    const display = document.getElementById('ai-debug-display');

    if (!display || !gameSettings.debugEnabled) return;
// 盤面データをJavaScriptの2D配列形式の文字列に変換します
    let boardArrayString = '[\n';
    for (let y = 0; y < payload.board.length; y++) {
        const row = payload.board[y];
        // 各セルを 'null' または引用符付きの文字列 ('G'など) に変換します
        const rowString = row.map(cell => cell === null ? 'null' : `"${cell}"`).join(', ');
        boardArrayString += `  [${rowString}]`;
        if (y < payload.board.length - 1) {
            boardArrayString += ',\n'; // 最終行以外はカンマと改行を追加
        }
    }
    boardArrayString += '\n]';

    // 表示用のHTMLを生成します。盤面部分はtextareaに置き換えます。
    display.innerHTML = `
        PlayerID: ${payload.playerId}<br>
        REN: ${payload.ren}, B2B: ${payload.isB2B}<br>
        Hold: ${payload.holdPiece || 'none'} (CanHold: ${payload.canHold})<br>
        Next: ${payload.minoSequence.join(', ')}<br>
        Board (2D Array):<br>
        <textarea readonly style="width: 95%; height: 150px; font-family: monospace; font-size: 10px; background: #222; color: #eee; border: 1px solid #555; resize: vertical;"></textarea>
    `;

    // innerHTMLで設定した後、textareaのvalueに文字列を代入することで、
    // 改行が正しく反映され、コピー可能な状態になります。
    display.querySelector('textarea').value = boardArrayString;
}

// Kept separately so settings migration can restore this action for profiles
// saved before the PC guide was added.
const DEFAULT_PC_SEARCH_BINDING = Object.freeze({ type: 'key', value: 'p', label: 'p' });

let keyBindings = {
        p1: {
        left:      { type: 'key', value: 'a',           label: 'a' },
        right:     { type: 'key', value: 'd',           label: 'd' },
        softDrop:  { type: 'key', value: 's',           label: 's' },
    
        hardDrop:  { type: 'key', value: ' ',           label: 'Space' },
        rotateCW:  { type: 'key', value: 'e',           label: 'e' },
        rotateCCW: { type: 'key', value: 'q',           label: 'q' },
        hold:      { type: 'key', value: 'w',     
      label: 'w' },
        pcSearch:  { ...DEFAULT_PC_SEARCH_BINDING },
        retry:     { type: 'key', value: 'r',           label: 'r' },
        exit:      { type: 'key', value: 'escape',      label: 'Esc' },
    },
    p2: {
        left:      { type: 'key', value: 'arrowleft',   label: 'ArrowLeft' },
        right:     { type: 'key', value: 'arrowright',  label: 'ArrowRight' },
        softDrop:  { type: 'key', value: 'arrowdown',   label: 'ArrowDown' },
        hardDrop:  { type: 'key', value: 'enter',      
 label: 'Enter' },
        rotateCW:  { type: 'key', value: 'arrowup',     label: 'ArrowUp' },
        rotateCCW: { type: 'key', value: '.',           label: '.'
},
        hold:      { type: 'key', value: '/',           label: '/' },
        retry:     { type: 'key', value: 'backspace',   label: 'BackSpace' },
        exit:      { type: 'key', value: 'delete',      label: 'Del' },
    }
};
const keyActionLabels = {
    left: '左移動', right: '右移動', softDrop: 'ソフトドロップ',
    rotateCW: '右回転', rotateCCW: '左回転',
    hardDrop: 'ハードドロップ', hold: 'ホールド', pcSearch: 'PC探索',
    retry: 'リトライ', exit: '中断'
};
let isBindingKey = false, bindingPlayer = null, bindingAction = null;
let gamepads = {}, prevGamepads = {};

const editorData = {
    p1: { board: null, nextQueue: [], hold: null, viewY: BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT, activeColor: 'I', nextInsertionIndex: -1 },
    p2: { board: null, nextQueue: [], hold: null, viewY: BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT, activeColor: 'I', nextInsertionIndex: -1 },
    rule: { description: '', code: '' }
};

let scanState = { image: null, bottomLeft: null, topRight: null, currentMousePos: {x: 0, y: 0}, targetPlayerId: null, parsedColors: null };

function* createMinoGenerator(customQueue) {
    if (customQueue && customQueue.length > 0) {
        yield* customQueue;
    }
    const bag = [];
    const pieces = Object.keys(TETROMINOS);
    while (true) {
        if (bag.length === 0) {
            const newBag = [...pieces];
            for (let i = newBag.length - 1; i > 0; i--) {
                const j = Math.floor(Math.random() * (i + 1));
                [newBag[i], newBag[j]] = [newBag[j], newBag[i]];
            }
            bag.push(...newBag);
        }
        yield bag.shift();
    }
}
