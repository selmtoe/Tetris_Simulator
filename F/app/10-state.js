
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
const COLORS = { 'I': '#00f0f0', 'O': '#f0f000', 'T': '#a000f0', 'L': '#f0a000', 'J': '#0000f0', 'S': '#00f000', 'Z': '#f00000', 'G': '#999999' };
const EDITOR_COLORS = {...COLORS, 'EMPTY': '#000000'};

let gameMode = '1P';
let fumenPages = [];
let currentPageIndex = 0;
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
const createBlankPlayer = () => ({
    board: createEmptyBoard(),
    viewY: BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT,
    activeColor: 'I',
    hold: '',
    // Compatibility cache for old f2/simulator data. New pages derive NEXT.
    next: '',
    nextInsertionIndex: -1,
    operation: null,
    placementMode: false
});
const createBlankPage = () => ({
    // nextInsertionIndex を追加: 0なら先頭、-1なら末尾、'hold'ならホールド
    p1: createBlankPlayer(),
    p2: createBlankPlayer()
});
function loadPage(index) {

    if (index < 0 || index >= fumenPages.length) return;
    currentPageIndex = index;
    if (typeof normalizeAllPages === 'function') normalizeAllPages();

    ['p1', 'p2'].forEach(playerId => {
        const data = fumenPages[currentPageIndex][playerId];
        const palette = document.getElementById(`${playerId}-palette`);
        
        palette.querySelector('.active')?.classList.remove('active');
        const swatches = palette.querySelectorAll('.color-swatch');
        swatches.forEach(swatch => {
            if (swatch.dataset.color === data.activeColor) {
      swatch.classList.add('active');
            }
        });

        // 修正: テキスト入力への代入を削除し、UI更新関数を呼び出し
        // データ読み込み時は末尾に追加するようにインデックスをリセット
        if (data.nextInsertionIndex === undefined) data.nextInsertionIndex = -1;
        updateNextQueueDisplay(playerId);

        drawEditorField(playerId);
    });
updatePageControls();

    if (currentDisplayMode === 'viewer') {
        drawViewer();
    }}

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
