
const EDITOR_BLOCK_SIZE = 50;
const BOARD_WIDTH = 10;
const BOARD_VISIBLE_HEIGHT = 20;
const BOARD_HEIGHT = 40;
const RESOLUTION_SCALE = 2;
const BLOCK_SIZE = 28;
const HOLD_AREA_WIDTH = 5 * BLOCK_SIZE;
const PLAYFIELD_WIDTH = BOARD_WIDTH * BLOCK_SIZE;
const NEXT_AREA_WIDTH = 5 * BLOCK_SIZE;
const PADDING = 20;
const PLAYER_CANVAS_WIDTH = HOLD_AREA_WIDTH + PLAYFIELD_WIDTH + NEXT_AREA_WIDTH + PADDING * 2;
const CANVAS_HEIGHT = (BOARD_VISIBLE_HEIGHT + 0.5) * BLOCK_SIZE;
const PLAYFIELD_X_OFFSET = HOLD_AREA_WIDTH + PADDING;
// Official tetris-fumen dark/non-guideline mino palette.
const COLORS = { 'I': '#009999', 'O': '#999900', 'T': '#990099', 'L': '#996600', 'J': '#0000bb', 'S': '#009900', 'Z': '#990000', 'G': '#999999' };
// NEXT/HOLD and attached-piece previews retain the original bright palette.
const NEXT_COLORS = { 'I': '#00ffff', 'O': '#ffff00', 'T': '#ff00ff', 'L': '#ff9900', 'J': '#0000ff', 'S': '#00ff00', 'Z': '#ff0000', 'G': '#cccccc' };
const EDITOR_COLORS = {...COLORS, 'EMPTY': '#000000'};
const LAZY_BOARD_INFO = Symbol('lazyBoardInfo');

let gameMode = '1P';
let fumenPages = [];
let currentPageIndex = 0;
// A document is a collection of independent cases. `fumenPages` remains the
// active case's page array so the existing editor/viewer code can keep its
// page-oriented API.
let fumenCases = [];
let currentCaseIndex = 0;
let historyStack = [];
let historyIndex = -1;
const MAX_HISTORY = 50;
let currentDisplayMode = 'viewer';
let viewerCanvas, viewerCtx;
let viewerLoopHandle;
const TETROMINOS = {
    'I': { shape: [[0, 0], [1, 0], [2, 0], [3, 0]], center: [1.5, 0.5] }, 'O': { shape: [[0, 0], [1, 0], [0, -1], [1, -1]], center: [0.5, -0.5] },
    'T': { shape: [[0, 0], [-1, 0], [0, -1], [1, 0]], center: [0, 0] }, 'L': { shape: [[-1, 0], [0, 0], [1, 0], [1, -1]], center: [0, 0] },
    'J': { shape: [[0, 0], [-1, 0], [1, 0], [-1, -1]], center: [0, 0] }, 'S': { shape: [[1, -1], [-1, 0], [0, 0], [0, -1]], center: [0, 0] },
    'Z': { shape: [[0, 0], [1, 0], [0, -1], [-1, -1]], center: [0, 0] }
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
    "-1,2;0,1;0,2": { type: "J", rot: 3, offset: [0, -1] },
    "-1,1;0,1;1,0": { type: "S", rot: 0, offset: [0, -1] },
    "0,1;1,1;1,2": { type: "S", rot: 1, offset: [0, -1] },
    "1,0;1,1;2,1": { type: "Z", rot: 0, offset: [-1, -1] },
    "-1,1;-1,2;0,1": { type: "Z", rot: 1, offset: [1, -1] },
};
const createEmptyBoard = () => Array.from({ length: BOARD_HEIGHT }, () => Array(BOARD_WIDTH).fill(null));
function normalizedBoardText(value) {
    if (typeof value === 'string' && value.includes('.') && typeof TetrisEventCodec !== 'undefined') {
        return TetrisEventCodec.unpackBoard(value);
    }
    return String(value || '').padEnd(BOARD_WIDTH * BOARD_HEIGHT, '_').slice(0, BOARD_WIDTH * BOARD_HEIGHT)
        .split('').map(cell => cell === 'E' || cell === '0' ? '_' : cell).join('');
}
function createLazyBoard(value) {
    if (value?.[LAZY_BOARD_INFO]) return value;
    if (Array.isArray(value)) return value;
    const info = { source: normalizedBoardText(value), materialized: false };
    const rows = new Array(BOARD_HEIGHT);
    const ensure = () => {
        if (info.materialized) return;
        for (let y = 0; y < BOARD_HEIGHT; y++) {
            rows[y] = Array.from({ length: BOARD_WIDTH }, (_, x) => {
                const cell = info.source[y * BOARD_WIDTH + x];
                return cell === '_' ? null : cell;
            });
        }
        info.materialized = true;
    };
    return new Proxy(rows, {
        get(target, property, receiver) {
            if (property === LAZY_BOARD_INFO) return info;
            if (property !== 'length') ensure();
            return Reflect.get(target, property, receiver);
        },
        set(target, property, nextValue, receiver) {
            ensure();
            return Reflect.set(target, property, nextValue, receiver);
        },
        ownKeys(target) { ensure(); return Reflect.ownKeys(target); },
        getOwnPropertyDescriptor(target, property) {
            if (property !== 'length') ensure();
            return Reflect.getOwnPropertyDescriptor(target, property);
        }
    });
}
const boardToString = board => {
    const lazy = board?.[LAZY_BOARD_INFO];
    if (lazy && !lazy.materialized) return lazy.source;
    return Array.from({ length: BOARD_HEIGHT }, (_, y) =>
        Array.from({ length: BOARD_WIDTH }, (_, x) => board?.[y]?.[x] || '_').join('')).join('');
};
const stringToBoard = value => createLazyBoard(value);
const createBlankPage = () => ({
    // nextInsertionIndex を追加: 0なら先頭、-1なら末尾、'hold'ならホールド
    p1: { board: createEmptyBoard(), viewY: BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT, activeColor: 'I', hold: '', next: '', nextInsertionIndex: -1, operation: null, placementDraft: [], placementMode: false },
    p2: { board: createEmptyBoard(), viewY: BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT, activeColor: 'I', hold: '', next: '', nextInsertionIndex: -1, operation: null, placementDraft: [], placementMode: false }
});
function renderEditorPage() {
    const playerIds = gameMode === '2P' ? ['p1', 'p2'] : ['p1'];
    playerIds.forEach(playerId => {
        const data = fumenPages[currentPageIndex]?.[playerId];
        if (!data) return;
        const palette = document.getElementById(`${playerId}-palette`);
        palette?.querySelector('.active')?.classList.remove('active');
        palette?.querySelectorAll('.color-swatch').forEach(swatch => {
            swatch.classList.toggle('active', swatch.dataset.color === data.activeColor);
        });
        if (data.nextInsertionIndex === undefined) data.nextInsertionIndex = -1;
        updateNextQueueDisplay(playerId);
        drawEditorField(playerId);
    });
}

function loadPage(index) {
    if (index < 0 || index >= fumenPages.length) return;
    currentPageIndex = index;

    if (typeof normalizeActiveCase === 'function') normalizeActiveCase();
    if (typeof updateCaseControls === 'function') updateCaseControls();
    updatePageControls();
    if (currentDisplayMode === 'editor') renderEditorPage();
    else if (viewerCtx) drawViewer();
}

function updatePageControls() {
    const pageText = `Page ${currentPageIndex + 1} / ${fumenPages.length}`;
    
    document.getElementById('page-indicator').textContent = pageText;
    document.getElementById('prev-page').disabled = (currentPageIndex === 0);
    document.getElementById('delete-page').disabled = (fumenPages.length <= 1);
    
    updateUndoRedoButtons();

    const viewerIndicator = document.getElementById('viewer-page-indicator');
    const viewerSlider = document.getElementById('viewer-page-slider');
    
    if (viewerIndicator) {
        viewerIndicator.textContent = pageText;
    }
    if (viewerSlider) {
        viewerSlider.max = fumenPages.length - 1;
        viewerSlider.value = currentPageIndex;
        viewerSlider.disabled = (fumenPages.length <= 1);
    }
}
