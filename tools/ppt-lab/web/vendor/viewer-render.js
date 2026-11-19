/* エディタ（F/app/30-viewer.js）と同じ描画でテト譜の盤面を描く。
   定数と描画手順は 10-state.js / 30-viewer.js からそのまま持ってきている。 */
(function (root) {
    'use strict';

    const BOARD_WIDTH = 10;
    const BOARD_HEIGHT = 40;
    const BOARD_VISIBLE_HEIGHT = 20;
    const BLOCK_SIZE = 28;
    const HOLD_AREA_WIDTH = 5 * BLOCK_SIZE;
    const PLAYFIELD_WIDTH = BOARD_WIDTH * BLOCK_SIZE;
    const NEXT_AREA_WIDTH = 5 * BLOCK_SIZE;
    const PADDING = 20;
    const PLAYER_CANVAS_WIDTH = HOLD_AREA_WIDTH + PLAYFIELD_WIDTH + NEXT_AREA_WIDTH + PADDING * 2;
    const CANVAS_HEIGHT = (BOARD_VISIBLE_HEIGHT + 0.5) * BLOCK_SIZE;
    const PLAYFIELD_X_OFFSET = HOLD_AREA_WIDTH + PADDING;

    const COLORS = { I: '#009999', O: '#999900', T: '#990099', L: '#996600', J: '#0000bb', S: '#009900', Z: '#990000', G: '#999999' };
    const NEXT_COLORS = { I: '#00ffff', O: '#ffff00', T: '#ff00ff', L: '#ff9900', J: '#0000ff', S: '#00ff00', Z: '#ff0000', G: '#cccccc' };

    const TETROMINOS = {
        I: { shape: [[0, 0], [1, 0], [2, 0], [3, 0]], center: [1.5, 0.5] },
        O: { shape: [[0, 0], [1, 0], [0, -1], [1, -1]], center: [0.5, -0.5] },
        T: { shape: [[0, 0], [-1, 0], [0, -1], [1, 0]], center: [0, 0] },
        L: { shape: [[-1, 0], [0, 0], [1, 0], [1, -1]], center: [0, 0] },
        J: { shape: [[0, 0], [-1, 0], [1, 0], [-1, -1]], center: [0, 0] },
        S: { shape: [[1, -1], [-1, 0], [0, 0], [0, -1]], center: [0, 0] },
        Z: { shape: [[0, 0], [1, 0], [0, -1], [-1, -1]], center: [0, 0] }
    };

    function getShape(piece, rotation) {
        if (!piece || !TETROMINOS[piece]) return [];
        const shape = TETROMINOS[piece].shape;
        if (!rotation || piece === 'O') return shape;
        const center = TETROMINOS[piece].center;
        return shape.map(cell => {
            let x = cell[0] - center[0];
            let y = cell[1] - center[1];
            for (let i = 0; i < rotation; i += 1) {
                const next = [-y, x];
                x = next[0];
                y = next[1];
            }
            return [x + center[0], y + center[1]];
        });
    }

    function drawBlock(ctx, piece, x, y, alpha) {
        if (y < -BLOCK_SIZE) return;
        ctx.globalAlpha = alpha === undefined ? 1 : alpha;
        ctx.fillStyle = NEXT_COLORS[piece] || '#FFF';
        ctx.fillRect(x, y, BLOCK_SIZE, BLOCK_SIZE);
        ctx.strokeStyle = '#444';
        ctx.lineWidth = 0.5;
        ctx.strokeRect(x, y, BLOCK_SIZE, BLOCK_SIZE);
        ctx.globalAlpha = 1;
    }

    function drawUI(ctx, pageData) {
        ctx.textAlign = 'center';
        ctx.fillStyle = '#FFF';
        ctx.font = `bold ${BLOCK_SIZE * 0.8}px "Orbitron", system-ui, sans-serif`;

        ctx.fillText('HOLD', HOLD_AREA_WIDTH / 2, 40);
        const hold = pageData && pageData.hold;
        if (hold && TETROMINOS[hold]) {
            getShape(hold, 0).forEach(cell => {
                const px = (HOLD_AREA_WIDTH / 2) - (TETROMINOS[hold].center[0] * BLOCK_SIZE) + (cell[0] * BLOCK_SIZE);
                const py = 70 - (TETROMINOS[hold].center[1] * BLOCK_SIZE) + (cell[1] * BLOCK_SIZE);
                drawBlock(ctx, hold, px, py);
            });
        }

        const nextX = PLAYFIELD_X_OFFSET + PLAYFIELD_WIDTH + PADDING + NEXT_AREA_WIDTH / 2;
        ctx.fillStyle = '#FFF';
        ctx.fillText('NEXT', nextX, 40);
        const queue = String((pageData && pageData.next) || '').split('');
        const spacing = Math.min(2.5, (CANVAS_HEIGHT - 90) / (BLOCK_SIZE * Math.max(1, Math.min(queue.length, 10) - 1)));
        const size = queue.length > 8 ? BLOCK_SIZE * 0.85 : BLOCK_SIZE;
        for (let i = 0; i < Math.min(queue.length, 10); i += 1) {
            const piece = queue[i];
            if (!piece || !TETROMINOS[piece]) continue;
            const shape = getShape(piece, 0);
            const center = size === BLOCK_SIZE ? TETROMINOS[piece].center : [
                (Math.min(...shape.map(cell => cell[0])) + Math.max(...shape.map(cell => cell[0]))) / 2,
                (Math.min(...shape.map(cell => cell[1])) + Math.max(...shape.map(cell => cell[1]))) / 2
            ];
            shape.forEach(cell => {
                const px = nextX - (center[0] * size) + (cell[0] * size);
                const py = 70 + (i * BLOCK_SIZE * spacing) - (center[1] * size) + (cell[1] * size);
                ctx.save();
                ctx.translate(px, py);
                ctx.scale(size / BLOCK_SIZE, size / BLOCK_SIZE);
                drawBlock(ctx, piece, 0, 0);
                ctx.restore();
            });
        }
    }

    function boardAt(board, x, y) {
        if (!board) return '';
        if (Array.isArray(board)) {
            const row = board[y];
            if (!row) return '';
            return row[x] || '';
        }
        return String(board)[y * BOARD_WIDTH + x] || '';
    }

    function drawBoard(ctx, pageData) {
        ctx.save();
        ctx.translate(PLAYFIELD_X_OFFSET, 0.5 * BLOCK_SIZE);

        ctx.fillStyle = 'rgba(0,0,0,0.5)';
        ctx.fillRect(0, 0, PLAYFIELD_WIDTH, BOARD_VISIBLE_HEIGHT * BLOCK_SIZE);

        ctx.strokeStyle = '#444';
        ctx.lineWidth = 0.5;
        for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y += 1) {
            for (let x = 0; x < BOARD_WIDTH; x += 1) {
                ctx.strokeRect(x * BLOCK_SIZE, y * BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE);
            }
        }

        ctx.strokeStyle = '#4b4b7c';
        ctx.lineWidth = 2;
        ctx.strokeRect(1, 1, PLAYFIELD_WIDTH - 2, (BOARD_VISIBLE_HEIGHT * BLOCK_SIZE) - 2);

        const viewY = BOARD_HEIGHT - BOARD_VISIBLE_HEIGHT;
        for (let y = 0; y < BOARD_VISIBLE_HEIGHT; y += 1) {
            const boardY = y + viewY;
            let lineClear = true;
            for (let x = 0; x < BOARD_WIDTH; x += 1) {
                if (!boardAt(pageData.board, x, boardY)) { lineClear = false; break; }
            }
            for (let x = 0; x < BOARD_WIDTH; x += 1) {
                const piece = boardAt(pageData.board, x, boardY);
                if (!piece) continue;
                const drawX = x * BLOCK_SIZE;
                const drawY = y * BLOCK_SIZE;
                ctx.fillStyle = COLORS[piece] || '#FFF';
                ctx.fillRect(drawX, drawY, BLOCK_SIZE, BLOCK_SIZE);
                if (lineClear) {
                    ctx.fillStyle = 'rgba(255, 255, 255, 0.3)';
                    ctx.fillRect(drawX, drawY, BLOCK_SIZE, BLOCK_SIZE);
                }
                ctx.strokeStyle = '#444';
                ctx.lineWidth = 0.5;
                ctx.strokeRect(drawX, drawY, BLOCK_SIZE, BLOCK_SIZE);
            }
        }
        ctx.restore();
    }

    function drawPlayer(ctx, pageData, offsetX) {
        ctx.save();
        ctx.translate(offsetX, 0);
        drawUI(ctx, pageData || {});
        drawBoard(ctx, pageData || {});
        ctx.restore();
    }

    function render(canvas, page, options) {
        const opts = options || {};
        const players = opts.players || ['p1', 'p2'];
        const width = PLAYER_CANVAS_WIDTH * players.length;
        canvas.width = width;
        canvas.height = CANVAS_HEIGHT;
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, width, CANVAS_HEIGHT);
        ctx.fillStyle = '#0b0f14';
        ctx.fillRect(0, 0, width, CANVAS_HEIGHT);
        players.forEach((side, index) => drawPlayer(ctx, page && page[side], index * PLAYER_CANVAS_WIDTH));
    }

    root.PPTViewerRender = { render, PLAYER_CANVAS_WIDTH, CANVAS_HEIGHT };
})(window);
