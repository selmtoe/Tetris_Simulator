/*
 * Cold Clear standard-mode port for this simulator.
 * SPDX-License-Identifier: MPL-2.0
 *
 * This file is an independent JavaScript implementation.  It does not import,
 * modify, or bundle the reference archive.  Its data model and algorithms are
 * based on the MPL-2.0 Cold Clear sources supplied with this project:
 * bot/src/{dag.rs,modes/normal.rs,evaluation/standard.rs} and
 * libtetris/src/{board.rs,moves.rs,piece.rs,lock_data.rs}.
 */

'use strict';

const CC_WIDTH = 10;
const CC_HEIGHT = 40;
const CC_FULL_ROW = (1 << CC_WIDTH) - 1;
const CC_PIECES = Object.freeze(['I', 'O', 'T', 'L', 'J', 'S', 'Z']);
const CC_ALL_BAG = (1 << CC_PIECES.length) - 1;
const CC_COMBO_GARBAGE = Object.freeze([0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 5]);
const CC_TSPIN_NONE = 0;
const CC_TSPIN_MINI = 1;
const CC_TSPIN_FULL = 2;

const CC_DEFAULT_WEIGHTS = Object.freeze({
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
    wasted_t: -152,
    use_bag: true,
    timed_jeopardy: true,
    stack_pc_damage: false
});

// Rotation order is the simulator's order: North, East, South, West.
// These coordinates deliberately match Player.getMinoShape_forAI().
const CC_SHAPES = Object.freeze({
    I: [
        [[0, 0], [-1, 0], [1, 0], [2, 0]],
        [[1, 0], [1, -1], [1, 1], [1, 2]],
        [[0, 1], [-1, 1], [1, 1], [2, 1]],
        [[0, 0], [0, -1], [0, 1], [0, 2]]
    ],
    O: [
        [[0, 0], [1, 0], [0, -1], [1, -1]],
        [[0, 0], [1, 0], [0, -1], [1, -1]],
        [[0, 0], [1, 0], [0, -1], [1, -1]],
        [[0, 0], [1, 0], [0, -1], [1, -1]]
    ],
    T: [
        [[0, 0], [-1, 0], [1, 0], [0, -1]],
        [[0, 0], [0, -1], [1, 0], [0, 1]],
        [[0, 0], [1, 0], [-1, 0], [0, 1]],
        [[0, 0], [0, 1], [-1, 0], [0, -1]]
    ],
    S: [
        [[0, 0], [-1, 0], [0, -1], [1, -1]],
        [[0, 0], [0, -1], [1, 0], [1, 1]],
        [[0, 0], [1, 0], [0, 1], [-1, 1]],
        [[0, 0], [0, 1], [-1, 0], [-1, -1]]
    ],
    Z: [
        [[0, 0], [1, 0], [0, -1], [-1, -1]],
        [[0, 0], [0, 1], [1, 0], [1, -1]],
        [[0, 0], [-1, 0], [0, 1], [1, 1]],
        [[0, 0], [0, -1], [-1, 0], [-1, 1]]
    ],
    J: [
        [[0, 0], [-1, 0], [1, 0], [-1, -1]],
        [[0, 0], [0, -1], [0, 1], [1, -1]],
        [[0, 0], [1, 0], [-1, 0], [1, 1]],
        [[0, 0], [0, 1], [0, -1], [-1, 1]]
    ],
    L: [
        [[0, 0], [1, 0], [-1, 0], [1, -1]],
        [[0, 0], [0, 1], [0, -1], [1, 1]],
        [[0, 0], [-1, 0], [1, 0], [-1, 1]],
        [[0, 0], [0, -1], [0, 1], [-1, -1]]
    ]
});

const CC_KICKS = Object.freeze({
    common: Object.freeze({
        '0>1': [[0, 0], [-1, 0], [-1, 1], [0, -2], [-1, -2]],
        '1>0': [[0, 0], [1, 0], [1, -1], [0, 2], [1, 2]],
        '1>2': [[0, 0], [1, 0], [1, -1], [0, 2], [1, 2]],
        '2>1': [[0, 0], [-1, 0], [-1, 1], [0, -2], [-1, -2]],
        '2>3': [[0, 0], [1, 0], [1, 1], [0, -2], [1, -2]],
        '3>2': [[0, 0], [-1, 0], [-1, -1], [0, 2], [-1, 2]],
        '3>0': [[0, 0], [-1, 0], [-1, -1], [0, 2], [-1, 2]],
        '0>3': [[0, 0], [1, 0], [1, 1], [0, -2], [1, -2]]
    }),
    I: Object.freeze({
        '0>1': [[0, 0], [-2, 0], [1, 0], [-2, -1], [1, 2]],
        '1>0': [[0, 0], [2, 0], [-1, 0], [2, 1], [-1, -2]],
        '1>2': [[0, 0], [-1, 0], [2, 0], [-1, 2], [2, -1]],
        '2>1': [[0, 0], [1, 0], [-2, 0], [1, -2], [-2, 1]],
        '2>3': [[0, 0], [2, 0], [-1, 0], [2, 1], [-1, -2]],
        '3>2': [[0, 0], [-2, 0], [1, 0], [-2, -1], [1, 2]],
        '3>0': [[0, 0], [1, 0], [-2, 0], [1, -2], [-2, 1]],
        '0>3': [[0, 0], [-1, 0], [2, 0], [-1, 2], [2, -1]]
    })
});

function ccPieceBit(piece) {
    const index = CC_PIECES.indexOf(piece);
    return index < 0 ? 0 : (1 << index);
}

function ccIsPiece(piece) {
    return ccPieceBit(piece) !== 0;
}

function ccBagSize(mask) {
    let n = 0;
    while (mask) {
        mask &= mask - 1;
        n++;
    }
    return n;
}

function ccTakeBag(mask, piece) {
    const bit = ccPieceBit(piece);
    if (!bit || !(mask & bit)) return mask;
    const remaining = mask & ~bit;
    return remaining === 0 ? CC_ALL_BAG : remaining;
}

function ccBagFromKnownPieces(pieces) {
    let bag = CC_ALL_BAG;
    for (const piece of pieces) {
        if (ccIsPiece(piece) && (bag & ccPieceBit(piece))) {
            bag = ccTakeBag(bag, piece);
        }
    }
    return bag;
}

function ccWeights(overrides) {
    const result = {
        ...CC_DEFAULT_WEIGHTS,
        tslot: [...CC_DEFAULT_WEIGHTS.tslot],
        well_column: [...CC_DEFAULT_WEIGHTS.well_column]
    };
    if (!overrides || typeof overrides !== 'object') return result;
    for (const key of Object.keys(CC_DEFAULT_WEIGHTS)) {
        const value = overrides[key];
        if (typeof CC_DEFAULT_WEIGHTS[key] === 'number' && Number.isFinite(value)) {
            result[key] = value;
        }
    }
    if (Array.isArray(overrides.tslot) && overrides.tslot.length === 4 && overrides.tslot.every(Number.isFinite)) {
        result.tslot = [...overrides.tslot];
    }
    if (Array.isArray(overrides.well_column) && overrides.well_column.length === 10 && overrides.well_column.every(Number.isFinite)) {
        result.well_column = [...overrides.well_column];
    }
    return result;
}

class CCBoard {
    constructor(rows, heights) {
        this.rows = rows ? new Uint16Array(rows) : new Uint16Array(CC_HEIGHT);
        this.heights = heights ? new Int8Array(heights) : CCBoard.computeHeights(this.rows);
        this._key = null;
    }

    static fromSimulator(layout) {
        const rows = new Uint16Array(CC_HEIGHT);
        if (Array.isArray(layout)) {
            const offset = CC_HEIGHT - layout.length;
            for (let y = 0; y < layout.length; y++) {
                let row = 0;
                for (let x = 0; x < CC_WIDTH; x++) {
                    if (layout[y] && layout[y][x] !== null && layout[y][x] !== undefined) row |= 1 << x;
                }
                if (y + offset >= 0 && y + offset < CC_HEIGHT) rows[y + offset] = row;
            }
        }
        return new CCBoard(rows);
    }

    static computeHeights(rows) {
        const heights = new Int8Array(CC_WIDTH);
        for (let x = 0; x < CC_WIDTH; x++) {
            for (let y = 0; y < CC_HEIGHT; y++) {
                if (rows[y] & (1 << x)) {
                    heights[x] = CC_HEIGHT - y;
                    break;
                }
            }
        }
        return heights;
    }

    key() {
        if (this._key === null) this._key = Array.from(this.rows).join(',');
        return this._key;
    }

    occupied(x, y) {
        if (x < 0 || x >= CC_WIDTH) return true;
        if (y < 0) return false;
        if (y >= CC_HEIGHT) return true;
        return (this.rows[y] & (1 << x)) !== 0;
    }

    // Same board through the source's bottom-left coordinate system.
    occupiedBottom(x, y) {
        if (x < 0 || x >= CC_WIDTH || y < 0) return true;
        if (y >= CC_HEIGHT) return false;
        return this.occupied(x, CC_HEIGHT - 1 - y);
    }

    rowBottom(y) {
        if (y < 0) return CC_FULL_ROW;
        if (y >= CC_HEIGHT) return 0;
        return this.rows[CC_HEIGHT - 1 - y];
    }

    isEmpty() {
        return this.heights.every(height => height === 0);
    }

    valid(piece) {
        for (const [dx, dy] of CC_SHAPES[piece.type][piece.rotation]) {
            if (this.occupied(piece.x + dx, piece.y + dy)) return false;
        }
        return true;
    }

    lock(piece, b2b, combo) {
        const rows = new Uint16Array(this.rows);
        const cells = ccCells(piece);
        let lockedOut = true;
        for (const [x, y] of cells) {
            if (y >= 20) lockedOut = false;
            if (x >= 0 && x < CC_WIDTH && y >= 0 && y < CC_HEIGHT) rows[y] |= 1 << x;
        }

        let lines = 0;
        for (let y = 0; y < CC_HEIGHT; y++) if (rows[y] === CC_FULL_ROW) lines++;
        let nextRows = rows;
        if (lines) {
            nextRows = new Uint16Array(CC_HEIGHT);
            let write = lines;
            for (let read = 0; read < CC_HEIGHT; read++) {
                if (rows[read] !== CC_FULL_ROW) nextRows[write++] = rows[read];
            }
        }
        const board = new CCBoard(nextRows);
        const kind = ccPlacementKind(lines, piece.tspin);
        const isClear = lines > 0;
        const hard = ccIsHard(kind);
        let garbage = ccGarbage(kind);
        let didB2b = false;
        let nextB2b = b2b;
        let nextCombo = combo;
        if (isClear) {
            if (hard) {
                if (b2b) {
                    garbage++;
                    didB2b = true;
                }
                nextB2b = true;
            } else {
                nextB2b = false;
            }
            garbage += CC_COMBO_GARBAGE[Math.min(combo, CC_COMBO_GARBAGE.length - 1)];
            nextCombo = combo + 1;
        } else {
            nextCombo = 0;
        }
        const perfectClear = board.isEmpty();
        if (perfectClear) garbage = 10;
        return {
            board,
            b2b: nextB2b,
            combo: nextCombo,
            lock: {
                kind,
                lines,
                lockedOut,
                b2b: didB2b,
                perfectClear,
                combo: isClear ? combo : null,
                garbage,
                isClear
            }
        };
    }
}

function ccCells(piece) {
    return CC_SHAPES[piece.type][piece.rotation].map(([dx, dy]) => [piece.x + dx, piece.y + dy]);
}

function ccPlacementKind(lines, tspin) {
    if (tspin === CC_TSPIN_FULL) return ['tspin', 'tspin1', 'tspin2', 'tspin3'][lines] || 'none';
    if (tspin === CC_TSPIN_MINI) return ['mini', 'mini1', 'mini2'][lines] || 'none';
    return ['none', 'clear1', 'clear2', 'clear3', 'clear4'][lines] || 'none';
}

function ccIsHard(kind) {
    return kind === 'clear4' || kind === 'mini' || kind === 'mini1' || kind === 'mini2' || kind === 'tspin' || kind === 'tspin1' || kind === 'tspin2' || kind === 'tspin3';
}

function ccGarbage(kind) {
    switch (kind) {
        case 'clear2':
        case 'mini2': return 1;
        case 'clear3':
        case 'tspin1': return 2;
        case 'clear4':
        case 'tspin2': return 4;
        case 'tspin3': return 6;
        default: return 0;
    }
}

function ccCompareValue(a, b) {
    if (a.value !== b.value) return a.value - b.value;
    return a.spike - b.spike;
}

function ccAddValue(a, b) {
    return { value: a.value + b.value, spike: a.spike + b.spike };
}

function ccValueWithReward(value, reward) {
    return {
        value: value.value + reward.value,
        spike: reward.attack === -1 ? 0 : value.spike + reward.attack
    };
}

function ccImproveValue(current, candidate) {
    return {
        value: Math.max(current.value, candidate.value),
        spike: Math.max(current.spike, candidate.spike)
    };
}

function ccTspinStatus(board, piece, kickIndex) {
    if (piece.type !== 'T') return CC_TSPIN_NONE;
    const corners = [[-1, -1], [1, -1], [-1, 1], [1, 1]];
    let filled = 0;
    for (const [dx, dy] of corners) if (board.occupied(piece.x + dx, piece.y + dy)) filled++;
    if (filled < 3) return CC_TSPIN_NONE;
    const front = [
        [[-1, -1], [1, -1]],
        [[1, -1], [1, 1]],
        [[-1, 1], [1, 1]],
        [[-1, -1], [-1, 1]]
    ][piece.rotation];
    const full = front.every(([dx, dy]) => board.occupied(piece.x + dx, piece.y + dy));
    return (full || kickIndex === 4) ? CC_TSPIN_FULL : CC_TSPIN_MINI;
}

function ccRotate(board, piece, direction) {
    if (piece.type === 'O') return null;
    const target = (piece.rotation + direction + 4) % 4;
    const key = `${piece.rotation}>${target}`;
    const tests = (piece.type === 'I' ? CC_KICKS.I : CC_KICKS.common)[key] || [[0, 0]];
    for (let i = 0; i < tests.length; i++) {
        const [dx, sourceDy] = tests[i];
        const candidate = {
            type: piece.type,
            x: piece.x + dx,
            y: piece.y - sourceDy,
            rotation: target,
            tspin: CC_TSPIN_NONE
        };
        if (board.valid(candidate)) {
            candidate.tspin = ccTspinStatus(board, candidate, i);
            return candidate;
        }
    }
    return null;
}

function ccShift(board, piece, dx) {
    const candidate = { ...piece, x: piece.x + dx, tspin: CC_TSPIN_NONE };
    return board.valid(candidate) ? candidate : null;
}

function ccSonicDrop(board, piece) {
    let y = piece.y;
    while (board.valid({ ...piece, y: y + 1 })) y++;
    return y === piece.y ? piece : { ...piece, y, tspin: CC_TSPIN_NONE };
}

function ccPlacementKey(piece) {
    const cells = ccCells(piece).map(([x, y]) => `${x}:${y}`).sort().join('|');
    return `${piece.type}/${cells}/${piece.tspin}`;
}

class CCMinHeap {
    constructor() { this.data = []; }
    get length() { return this.data.length; }
    push(value) {
        const data = this.data;
        data.push(value);
        let i = data.length - 1;
        while (i > 0) {
            const parent = (i - 1) >> 1;
            if (data[parent].time <= value.time) break;
            data[i] = data[parent];
            i = parent;
        }
        data[i] = value;
    }
    pop() {
        const data = this.data;
        if (data.length === 0) return null;
        const result = data[0];
        const last = data.pop();
        if (data.length) {
            let i = 0;
            while (true) {
                let child = i * 2 + 1;
                if (child >= data.length) break;
                if (child + 1 < data.length && data[child + 1].time < data[child].time) child++;
                if (data[child].time >= last.time) break;
                data[i] = data[child];
                i = child;
            }
            data[i] = last;
        }
        return result;
    }
}

// Port of libtetris::find_moves' core search.  The simulator uses a complete
// zero-gravity search: lateral/rotation moves and sonic drops are explored in
// increasing input-time order, retaining the true SRS/T-spin state.
function ccFindMoves(board, type) {
    let spawn = null;
    for (const y of [20, 19]) {
        const candidate = { type, x: 4, y, rotation: 0, tspin: CC_TSPIN_NONE };
        if (board.valid(candidate)) {
            spawn = candidate;
            break;
        }
    }
    if (!spawn) return [];

    const queue = new CCMinHeap();
    const checked = new Map();
    const locked = new Map();
    const push = (piece, time, inputs) => {
        const key = `${piece.x},${piece.y},${piece.rotation},${piece.tspin}`;
        const previous = checked.get(key);
        if (previous !== undefined && previous <= time) return;
        checked.set(key, time);
        queue.push({ piece, time, inputs });
    };
    push(spawn, 0, []);

    while (queue.length) {
        const current = queue.pop();
        if (current.inputs.length > 32) continue;
        const landed = ccSonicDrop(board, current.piece);
        if (!ccCells(landed).every(([, y]) => y < 20)) {
            const key = ccPlacementKey(landed);
            const existing = locked.get(key);
            if (!existing || current.time < existing.time) {
                locked.set(key, { ...landed, time: current.time, inputs: current.inputs });
            }
        }
        if (current.inputs.length === 32) continue;

        const steps = [
            ['left', ccShift(board, current.piece, -1), 1],
            ['right', ccShift(board, current.piece, 1), 1],
            ['cw', ccRotate(board, current.piece, 1), 1],
            ['ccw', ccRotate(board, current.piece, -1), 1]
        ];
        for (const [input, candidate, cost] of steps) {
            if (candidate) push(candidate, current.time + cost, [...current.inputs, input]);
        }
        const dropped = ccSonicDrop(board, current.piece);
        if (dropped.y !== current.piece.y) {
            const distance = dropped.y - current.piece.y;
            push(dropped, current.time + distance * 2, [...current.inputs, 'sonicDrop']);
        }
    }
    return [...locked.values()];
}

function ccTopHeight(board) {
    let maximum = 0;
    for (const height of board.heights) maximum = Math.max(maximum, height);
    return maximum;
}

function ccBumpiness(board, well) {
    let bump = -1;
    let bumpSq = -1;
    let previous = well === 0 ? 1 : 0;
    for (let x = 1; x < CC_WIDTH; x++) {
        if (x === well) continue;
        const difference = Math.abs(board.heights[previous] - board.heights[x]);
        bump += difference;
        bumpSq += difference * difference;
        previous = x;
    }
    return [Math.abs(bump), Math.abs(bumpSq)];
}

function ccCavitiesAndOverhangs(board) {
    let cavities = 0;
    let overhangs = 0;
    const highest = ccTopHeight(board);
    for (let y = 0; y < highest; y++) {
        for (let x = 0; x < CC_WIDTH; x++) {
            if (board.occupiedBottom(x, y) || y >= board.heights[x]) continue;
            if (x > 1 && board.heights[x - 1] <= y - 1 && board.heights[x - 2] <= y) {
                overhangs++;
                continue;
            }
            if (x < 8 && board.heights[x + 1] <= y - 1 && board.heights[x + 2] <= y) {
                overhangs++;
                continue;
            }
            cavities++;
        }
    }
    return [cavities, overhangs];
}

function ccCoveredCells(board) {
    let covered = 0;
    let coveredSq = 0;
    for (let x = 0; x < CC_WIDTH; x++) {
        for (let y = board.heights[x] - 3; y >= 0; y--) {
            if (!board.occupiedBottom(x, y)) {
                const cells = Math.min(6, board.heights[x] - y - 1);
                covered += cells;
                coveredSq += cells * cells;
            }
        }
    }
    return [covered, coveredSq];
}

function ccToTopPiece(bottomPiece) {
    return {
        type: bottomPiece.type,
        x: bottomPiece.x,
        y: CC_HEIGHT - 1 - bottomPiece.y,
        rotation: bottomPiece.rotation,
        tspin: bottomPiece.tspin || CC_TSPIN_NONE
    };
}

function ccSonicDropBottom(board, piece) {
    const top = ccSonicDrop(board, ccToTopPiece(piece));
    return { ...piece, y: CC_HEIGHT - 1 - top.y, tspin: top.tspin };
}

function ccBottomEmpty(board, x, y) {
    return !board.occupiedBottom(x, y);
}

function ccCutoutTslot(board, piece) {
    const topPiece = ccToTopPiece({ ...piece, tspin: CC_TSPIN_FULL });
    const result = board.lock(topPiece, false, 0);
    switch (result.lock.kind) {
        case 'tspin': return { lines: 0, board: null };
        case 'tspin1': return { lines: 1, board: null };
        case 'tspin2': return { lines: 2, board: result.board };
        case 'tspin3': return { lines: 3, board: result.board };
        default: return null;
    }
}

function ccSkyTslotRight(board) {
    for (let x = 0; x <= 7; x++) {
        const h1 = board.heights[x + 1];
        const h2 = board.heights[x + 2];
        if (h1 > h2 - 1) continue;
        const y = h2 + 1;
        if (board.occupiedBottom(x, y) && ccBottomEmpty(board, x, y - 1) && board.occupiedBottom(x, y - 2)) {
            return { type: 'T', x: x + 1, y: h2, rotation: 2, tspin: CC_TSPIN_NONE };
        }
    }
    return null;
}

function ccSkyTslotLeft(board) {
    for (let x = 0; x <= 7; x++) {
        const h1 = board.heights[x];
        const h2 = board.heights[x + 1];
        if (h2 > h1 - 1) continue;
        const y = h1 + 1;
        if (board.occupiedBottom(x + 2, y) && ccBottomEmpty(board, x + 2, y - 1) && board.occupiedBottom(x + 2, y - 2)) {
            return { type: 'T', x: x + 1, y: h1, rotation: 2, tspin: CC_TSPIN_NONE };
        }
    }
    return null;
}

function ccTstTwistLeft(board) {
    for (let x = 0; x <= 7; x++) {
        const h1 = board.heights[x];
        const h2 = board.heights[x + 1];
        if (h1 > h2 || board.occupiedBottom(x - 1, h2) !== board.occupiedBottom(x - 1, h2 + 1)) continue;
        const y = h2 + 1;
        if (
            board.occupiedBottom(x + 2, y) && ccBottomEmpty(board, x + 2, y - 1) &&
            ccBottomEmpty(board, x + 2, y - 2) && ccBottomEmpty(board, x + 1, y - 3) &&
            ccBottomEmpty(board, x + 2, y - 4)
        ) return { type: 'T', x: x + 2, y: h2 - 2, rotation: 3, tspin: CC_TSPIN_NONE };
    }
    return null;
}

function ccTstTwistRight(board) {
    for (let x = 0; x <= 7; x++) {
        const h1 = board.heights[x + 1];
        const h2 = board.heights[x + 2];
        if (h2 > h1 || board.occupiedBottom(x + 3, h1) !== board.occupiedBottom(x + 3, h1 + 1)) continue;
        const y = h1 + 1;
        if (
            board.occupiedBottom(x, y) && ccBottomEmpty(board, x, y - 1) &&
            ccBottomEmpty(board, x, y - 2) && ccBottomEmpty(board, x + 1, y - 3) &&
            ccBottomEmpty(board, x, y - 4)
        ) return { type: 'T', x, y: h1 - 2, rotation: 1, tspin: CC_TSPIN_NONE };
    }
    return null;
}

function ccFinLeft(board) {
    for (let x = 0; x <= 6; x++) {
        const h1 = board.heights[x];
        const h2 = board.heights[x + 1];
        if (h1 > h2 + 1) continue;
        const y = h2 + 2;
        if (
            board.occupiedBottom(x + 2, y) && board.occupiedBottom(x + 3, y) &&
            ccBottomEmpty(board, x + 2, y - 1) && ccBottomEmpty(board, x + 3, y - 1) &&
            ccBottomEmpty(board, x + 2, y - 2) && ccBottomEmpty(board, x + 3, y - 2) && board.occupiedBottom(x + 4, y - 2) &&
            ccBottomEmpty(board, x + 2, y - 3) && ccBottomEmpty(board, x + 3, y - 3) &&
            board.occupiedBottom(x + 2, y - 4) && ccBottomEmpty(board, x + 3, y - 4) && board.occupiedBottom(x + 4, y - 4)
        ) return { type: 'T', x: x + 3, y: h2 - 1, rotation: 3, tspin: CC_TSPIN_NONE };
    }
    return null;
}

function ccFinRight(board) {
    for (let x = 0; x <= 6; x++) {
        const h1 = board.heights[x + 2];
        const h2 = board.heights[x + 3];
        if (h2 > h1 + 1 || !board.occupiedBottom(x - 1, h1) || !board.occupiedBottom(x - 1, h1 - 2)) continue;
        const y = h1 + 2;
        if (
            board.occupiedBottom(x, y) && board.occupiedBottom(x + 1, y) &&
            ccBottomEmpty(board, x, y - 1) && ccBottomEmpty(board, x + 1, y - 1) &&
            ccBottomEmpty(board, x, y - 2) && ccBottomEmpty(board, x + 1, y - 2) &&
            ccBottomEmpty(board, x, y - 3) && ccBottomEmpty(board, x + 1, y - 3) &&
            ccBottomEmpty(board, x, y - 4) && board.occupiedBottom(x + 1, y - 4)
        ) return { type: 'T', x, y: h1 - 1, rotation: 1, tspin: CC_TSPIN_NONE };
    }
    return null;
}

function ccCaveTslot(board, start) {
    const piece = ccSonicDropBottom(board, start);
    const { x, y, rotation } = piece;
    if (rotation === 1) {
        if (!board.occupiedBottom(x - 1, y) && board.occupiedBottom(x - 1, y - 1) && board.occupiedBottom(x + 1, y - 1) && board.occupiedBottom(x - 1, y + 1)) {
            return { type: 'T', x, y, rotation: 2, tspin: CC_TSPIN_NONE };
        }
        if (!board.occupiedBottom(x + 1, y - 1) && !board.occupiedBottom(x + 2, y - 1) && !board.occupiedBottom(x + 1, y - 2) && board.occupiedBottom(x - 1, y) && board.occupiedBottom(x + 2, y) && board.occupiedBottom(x, y - 2) && board.occupiedBottom(x + 2, y - 2)) {
            return { type: 'T', x: x + 1, y: y - 1, rotation: 2, tspin: CC_TSPIN_NONE };
        }
    } else if (rotation === 3) {
        if (!board.occupiedBottom(x + 1, y) && board.occupiedBottom(x + 1, y + 1) && board.occupiedBottom(x + 1, y - 1) && board.occupiedBottom(x - 1, y - 1)) {
            return { type: 'T', x, y, rotation: 2, tspin: CC_TSPIN_NONE };
        }
        if (!board.occupiedBottom(x - 1, y - 1) && !board.occupiedBottom(x - 2, y - 1) && !board.occupiedBottom(x - 1, y - 2) && board.occupiedBottom(x + 1, y) && board.occupiedBottom(x - 2, y) && board.occupiedBottom(x - 2, y - 2) && board.occupiedBottom(x, y - 2)) {
            return { type: 'T', x: x - 1, y: y - 1, rotation: 2, tspin: CC_TSPIN_NONE };
        }
    }
    return null;
}

function ccNextBagMask(state, knownFuture = []) {
    let bag = state.bagMask;
    const known = state.current ? [state.current, ...knownFuture] : [...knownFuture];
    for (let i = known.length - 1; i >= 0; i--) {
        if (bag === CC_ALL_BAG) bag = 0;
        bag |= ccPieceBit(known[i]);
    }
    return bag;
}

function ccTslotScore(board, state, weights, knownFuture) {
    const nextBag = ccNextBagMask(state, knownFuture);
    const slots = weights.use_bag
        ? Number(Boolean(nextBag & ccPieceBit('T'))) + Number(ccBagSize(nextBag) <= 3) + Number(state.hold === 'T')
        : 1 + Number(state.hold === 'T');
    let working = board;
    let score = 0;
    for (let i = 0; i < slots; i++) {
        const candidate = ccSkyTslotLeft(working) || ccSkyTslotRight(working) || (() => {
            const twist = ccTstTwistLeft(working) || ccTstTwistRight(working);
            if (!twist) return null;
            const cave = ccCaveTslot(working, twist);
            if (cave) return cave;
            const corners = [
                working.occupiedBottom(twist.x - 1, twist.y - 1),
                working.occupiedBottom(twist.x + 1, twist.y - 1),
                working.occupiedBottom(twist.x - 1, twist.y + 1),
                working.occupiedBottom(twist.x + 1, twist.y + 1)
            ].filter(Boolean).length;
            const onStack = ccToTopPiece(twist);
            return corners >= 3 && working.occupied(onStack.x, onStack.y + 1) ? twist : null;
        })() || ccFinLeft(working) || ccFinRight(working);
        if (!candidate) break;
        const cutout = ccCutoutTslot(working, candidate);
        if (!cutout) break;
        score += weights.tslot[cutout.lines];
        if (!cutout.board) break;
        working = cutout.board;
    }
    return { score, board: working };
}

class CCStandardEvaluator {
    constructor(weights) {
        this.weights = ccWeights(weights);
    }

    evaluate(lock, board, state, moveTime, placed, knownFuture = []) {
        const weights = this.weights;
        let transient = 0;
        let reward = 0;
        if (lock.perfectClear) reward += weights.perfect_clear;
        if (weights.stack_pc_damage || !lock.perfectClear) {
            if (lock.b2b) reward += weights.b2b_clear;
            if (lock.combo !== null) reward += weights.combo_garbage * CC_COMBO_GARBAGE[Math.min(lock.combo, CC_COMBO_GARBAGE.length - 1)];
            const rewardKey = {
                clear1: 'clear1', clear2: 'clear2', clear3: 'clear3', clear4: 'clear4',
                tspin1: 'tspin1', tspin2: 'tspin2', tspin3: 'tspin3',
                mini1: 'mini_tspin1', mini2: 'mini_tspin2'
            }[lock.kind];
            if (rewardKey) reward += weights[rewardKey];
        }
        if (placed === 'T' && !['tspin1', 'tspin2', 'tspin3'].includes(lock.kind)) reward += weights.wasted_t;
        const timedMove = moveTime + (lock.isClear ? 40 : 0);
        reward += weights.move_time * timedMove;

        if (state.b2b) transient += weights.back_to_back;
        let highest = ccTopHeight(board);
        transient += weights.top_quarter * Math.max(0, highest - 15);
        transient += weights.top_half * Math.max(0, highest - 10);
        reward += weights.jeopardy * Math.max(0, highest - 10) * (weights.timed_jeopardy ? timedMove : 10) / 10;

        const tslot = ccTslotScore(board, state, weights, knownFuture);
        transient += tslot.score;
        const geometry = tslot.board;
        highest = ccTopHeight(geometry);
        transient += weights.height * highest;

        let well = 0;
        for (let x = 1; x < CC_WIDTH; x++) if (geometry.heights[x] <= geometry.heights[well]) well = x;
        let depth = 0;
        wellLoop: for (let y = geometry.heights[well]; y < 20; y++) {
            for (let x = 0; x < CC_WIDTH; x++) {
                if (x !== well && !geometry.occupiedBottom(x, y)) break wellLoop;
            }
            depth++;
        }
        depth = Math.min(depth, weights.max_well_depth);
        transient += weights.well_depth * depth;
        if (depth) transient += weights.well_column[well];

        if (weights.row_transitions) {
            let transitions = 0;
            for (let y = 0; y < CC_HEIGHT; y++) {
                let bits = ((geometry.rows[y] | (1 << CC_WIDTH)) ^ (1 | (geometry.rows[y] << 1))) >>> 0;
                while (bits) {
                    bits &= bits - 1;
                    transitions++;
                }
            }
            transient += weights.row_transitions * transitions;
        }
        if (weights.bumpiness || weights.bumpiness_sq) {
            const [bump, bumpSq] = ccBumpiness(geometry, well);
            transient += bump * weights.bumpiness + bumpSq * weights.bumpiness_sq;
        }
        if (weights.cavity_cells || weights.cavity_cells_sq || weights.overhang_cells || weights.overhang_cells_sq) {
            const [cavities, overhangs] = ccCavitiesAndOverhangs(geometry);
            transient += weights.cavity_cells * cavities + weights.cavity_cells_sq * cavities * cavities;
            transient += weights.overhang_cells * overhangs + weights.overhang_cells_sq * overhangs * overhangs;
        }
        if (weights.covered_cells || weights.covered_cells_sq) {
            const [covered, coveredSq] = ccCoveredCells(geometry);
            transient += weights.covered_cells * covered + weights.covered_cells_sq * coveredSq;
        }
        return {
            value: { value: Math.round(transient), spike: 0 },
            reward: {
                value: Math.round(reward),
                attack: lock.isClear ? lock.garbage : -1
            }
        };
    }
}

function ccStateKey(state) {
    // `index` is the position in the revealed piece stream.  Keeping it in
    // the key is the JS equivalent of Cold Clear's generation boundary: two
    // visually equal boards with different known future pieces must not merge.
    return `${state.board.key()};${state.index};${state.current || '-'};${state.bagMask};${state.hold || '-'};${state.b2b ? 1 : 0};${state.combo};${state.canHold ? 1 : 0}`;
}

function ccReadSnapshot(snapshot) {
    const current = ccIsPiece(snapshot.currentPiece) ? snapshot.currentPiece : null;
    const queue = Array.isArray(snapshot.nextQueue) ? snapshot.nextQueue.filter(ccIsPiece) : [];
    return {
        board: CCBoard.fromSimulator(snapshot.board),
        current,
        queue,
        pieces: current ? [current, ...queue] : queue,
        hold: ccIsPiece(snapshot.holdPiece) ? snapshot.holdPiece : null,
        b2b: Boolean(snapshot.isB2B),
        combo: Math.max(0, Number.isFinite(snapshot.ren) ? snapshot.ren + 1 : 0),
        canHold: snapshot.canHold !== false
    };
}

class CCSearchNode {
    constructor(state, value, key) {
        this.state = state;
        this.value = value || { value: 0, spike: 0 };
        this.key = key || ccStateKey(state);
        this.parents = [];
        this.children = null;
        this.chanceGroups = null;
        this.terminal = false;
    }
}

class CCSearch {
    constructor(weights) {
        this.evaluator = new CCStandardEvaluator(weights);
        this.root = null;
        this.nodes = new Map();
        this.moveCache = new Map();
        this.pendingEdge = null;
        this.lastReset = true;
        // The simulator reveals one extra preview on every spawn.  Unlike the
        // old snapshot-only port, retain that stream independently from a
        // board node so a revealed piece can resolve speculative branches.
        this.knownPieces = [];
        this.knownBagMask = CC_ALL_BAG;
    }

    setWeights(weights) {
        this.evaluator = new CCStandardEvaluator(weights);
        // Evaluation values are part of every node; changing weights invalidates the DAG.
        this.root = null;
        this.nodes.clear();
        this.moveCache.clear();
        this.pendingEdge = null;
        this.knownPieces = [];
        this.knownBagMask = CC_ALL_BAG;
        this.lastReset = true;
    }

    resetFromObservation(observation) {
        this.knownPieces = [...observation.pieces];
        this.knownBagMask = ccBagFromKnownPieces(this.knownPieces);
        const rootState = {
            board: observation.board,
            index: 0,
            current: this.knownPieces[0] || null,
            bagMask: this.knownBagMask,
            hold: observation.hold,
            b2b: observation.b2b,
            combo: observation.combo,
            canHold: observation.canHold
        };
        this.root = new CCSearchNode(rootState);
        this.nodes = new Map([[this.root.key, this.root]]);
        this.moveCache.clear();
        this.pendingEdge = null;
        this.lastReset = true;
    }

    currentFor(state) {
        return state.current || this.knownPieces[state.index] || null;
    }

    stateMatchesObservation(state, observation) {
        return state.board.key() === observation.board.key() &&
            this.currentFor(state) === observation.current &&
            state.hold === observation.hold &&
            state.b2b === observation.b2b &&
            state.combo === observation.combo &&
            state.canHold === observation.canHold;
    }

    // The full snapshot is a recovery path as well as a verifier.  Normally
    // Player sends addNextPiece at spawn time; if a message was missed, append
    // only the compatible trailing pieces instead of dropping the entire DAG.
    appendObservedPieces(observation, index) {
        for (let offset = 0; offset < observation.pieces.length; offset++) {
            const streamIndex = index + offset;
            const piece = observation.pieces[offset];
            if (streamIndex < this.knownPieces.length) {
                if (this.knownPieces[streamIndex] !== piece) return false;
            } else {
                this.addNextPiece(piece);
            }
        }
        return true;
    }

    rekeyNodes() {
        const indexed = new Map();
        for (const node of this.nodes.values()) {
            node.key = ccStateKey(node.state);
            // A collision here is a legitimate DAG transposition.  Edges keep
            // their object references, while the map retains one canonical
            // lookup entry for future expansions.
            if (!indexed.has(node.key)) indexed.set(node.key, node);
        }
        if (this.root) indexed.set(this.root.key, this.root);
        this.nodes = indexed;
    }

    // Port of DagState::add_next_piece.  A preview does not create a new
    // board root: it turns every speculative generation at this stream index
    // into the actually revealed piece and discards its other chance cases.
    addNextPiece(piece) {
        if (!ccIsPiece(piece)) return { added: false, resolved: 0 };
        const streamIndex = this.knownPieces.length;
        const oldMask = this.knownBagMask;
        this.knownPieces.push(piece);
        this.knownBagMask = ccTakeBag(oldMask, piece);
        const changed = new Set();
        let resolved = 0;

        for (const node of this.nodes.values()) {
            if (node.state.index < streamIndex) {
                // The state has a fully known queue in front of its unknown
                // generation, so its bag is now the bag after this new tail.
                node.state.bagMask = this.knownBagMask;
                changed.add(node);
                continue;
            }
            if (node.state.index !== streamIndex || node.state.current) continue;

            node.state.current = piece;
            node.state.bagMask = this.knownBagMask;
            if (node.chanceGroups) {
                const group = node.chanceGroups.find(candidate => candidate.piece === piece);
                node.children = group ? group.edges : [];
                node.chanceGroups = null;
                node.terminal = node.children.length === 0;
                resolved++;
            }
            changed.add(node);
        }

        this.rekeyNodes();
        const refreshed = new Set();
        for (const node of changed) this.refresh(node, refreshed);
        if (resolved) this.prune();
        return { added: true, resolved };
    }

    synchronize(snapshot) {
        const observation = ccReadSnapshot(snapshot);
        if (!this.root) {
            this.resetFromObservation(observation);
            return { reset: true, revealed: observation.pieces.length };
        }

        let revealed = 0;
        const matches = state => {
            const before = this.knownPieces.length;
            if (!this.appendObservedPieces(observation, state.index)) return false;
            revealed += this.knownPieces.length - before;
            return this.stateMatchesObservation(state, observation);
        };

        // An execution may reach us before its explicit commit message.  This
        // is safe only when the full board/hold/combo state confirms the edge.
        if (this.pendingEdge && matches(this.pendingEdge.child.state)) this.commit();
        if (this.root && matches(this.root.state)) {
            this.lastReset = false;
            return { reset: false, revealed };
        }

        this.resetFromObservation(observation);
        return { reset: true, revealed: observation.pieces.length };
    }

    get nodeCount() {
        return this.nodes.size;
    }

    obtainNode(state, value) {
        const key = ccStateKey(state);
        const existing = this.nodes.get(key);
        if (existing) return existing;
        const node = new CCSearchNode(state, value, key);
        this.nodes.set(key, node);
        return node;
    }

    movesFor(board, piece) {
        const key = `${board.key()}/${piece}`;
        let moves = this.moveCache.get(key);
        if (!moves) {
            moves = ccFindMoves(board, piece);
            this.moveCache.set(key, moves);
        }
        return moves;
    }

    advanceState(base) {
        const index = base.index + 1;
        const current = this.knownPieces[index] || null;
        return {
            ...base,
            index,
            current,
            // Until the first speculative generation, all visible pieces have
            // already been removed from the 7-bag.  A speculative child keeps
            // the mask that was produced when its chance case was selected.
            bagMask: current ? this.knownBagMask : base.bagMask,
            canHold: true
        };
    }

    makeEdge(parent, base, placed, hold) {
        const lockResult = base.board.lock(placed, base.b2b, base.combo);
        if (lockResult.lock.lockedOut) return null;
        let next = this.advanceState(base);
        if (hold) next = { ...next, hold: this.currentFor(base) };
        next = {
            ...next,
            board: lockResult.board,
            b2b: lockResult.b2b,
            combo: lockResult.combo,
            canHold: true
        };
        const evaluated = this.evaluator.evaluate(
            lockResult.lock,
            lockResult.board,
            next,
            placed.time,
            placed.type,
            this.knownPieces.slice(next.index + 1)
        );
        const child = this.obtainNode(next, evaluated.value);
        const edge = { parent, child, reward: evaluated.reward, placement: placed, hold, garbage: lockResult.lock.garbage };
        if (!child.parents.includes(edge)) child.parents.push(edge);
        return edge;
    }

    makeChildren(parent, base) {
        const current = this.currentFor(base);
        if (!current) return [];
        const normalized = base.current === current ? base : { ...base, current };
        const children = [];
        for (const placement of this.movesFor(normalized.board, current)) {
            const edge = this.makeEdge(parent, normalized, placement, false);
            if (edge) children.push(edge);
        }
        if (!normalized.canHold) return children;
        if (normalized.hold) {
            for (const placement of this.movesFor(normalized.board, normalized.hold)) {
                const edge = this.makeEdge(parent, normalized, placement, true);
                if (edge) children.push(edge);
            }
        } else {
            // Empty hold: consume the following known piece, place it, and retain current.
            const heldIndex = normalized.index + 1;
            const heldPlacementPiece = this.knownPieces[heldIndex] || null;
            if (!heldPlacementPiece) return children;
            const heldBase = {
                ...normalized,
                index: heldIndex,
                current: heldPlacementPiece,
                hold: current,
                bagMask: this.knownBagMask
            };
            for (const placement of this.movesFor(normalized.board, heldPlacementPiece)) {
                const edge = this.makeEdge(parent, heldBase, placement, false);
                if (edge) {
                    // The move itself used hold even though heldBase has already normalized state.
                    edge.hold = true;
                    children.push(edge);
                }
            }
        }
        return children;
    }

    expand(node) {
        if (node.children || node.chanceGroups || node.terminal) return;
        const current = this.currentFor(node.state);
        if (current) {
            node.state.current = current;
            node.children = this.makeChildren(node, node.state);
            node.terminal = node.children.length === 0;
        } else {
            const groups = [];
            for (const piece of CC_PIECES) {
                if (!(node.state.bagMask & ccPieceBit(piece))) continue;
                const assumed = { ...node.state, current: piece, bagMask: ccTakeBag(node.state.bagMask, piece) };
                const edges = this.makeChildren(node, assumed);
                groups.push({ piece, edges });
            }
            node.chanceGroups = groups;
            node.terminal = groups.length === 0 || groups.every(group => group.edges.length === 0);
        }
        this.refresh(node, new Set());
    }

    edgeValue(edge) {
        return ccValueWithReward(edge.child.value, edge.reward);
    }

    refresh(node, seen) {
        if (seen.has(node)) return;
        seen.add(node);
        if (node.children) {
            const values = node.children.map(edge => this.edgeValue(edge));
            if (values.length) {
                node.value = values.reduce((best, value) => ccImproveValue(best, value), { value: -Infinity, spike: -Infinity });
                node.children.sort((a, b) => ccCompareValue(this.edgeValue(b), this.edgeValue(a)));
            }
        } else if (node.chanceGroups) {
            let sum = { value: 0, spike: 0 };
            let count = 0;
            let worst = null;
            let dead = 0;
            for (const group of node.chanceGroups) {
                if (!group.edges.length) {
                    dead++;
                    continue;
                }
                const groupValue = group.edges.map(edge => this.edgeValue(edge)).reduce((best, value) => ccImproveValue(best, value), { value: -Infinity, spike: -Infinity });
                group.edges.sort((a, b) => ccCompareValue(this.edgeValue(b), this.edgeValue(a)));
                sum = ccAddValue(sum, groupValue);
                count++;
                if (!worst || ccCompareValue(groupValue, worst) < 0) worst = groupValue;
            }
            if (count) {
                if (dead && worst) sum = ccAddValue(sum, { value: (worst.value - 1000) * dead, spike: 0 });
                node.value = { value: Math.trunc(sum.value / (count + dead)), spike: Math.trunc(sum.spike / (count + dead)) };
            }
        }
        for (const parentEdge of node.parents) this.refresh(parentEdge.parent, seen);
    }

    chooseWeighted(edges) {
        if (!edges.length) return null;
        const values = edges.map(edge => this.edgeValue(edge));
        let minimum = values[0];
        for (const value of values) if (ccCompareValue(value, minimum) < 0) minimum = value;
        let total = 0;
        const weights = values.map((value, rank) => {
            const delta = value.value - minimum.value + 10;
            const weight = (delta * delta) / (rank * rank + 1);
            total += weight;
            return weight;
        });
        let pick = Math.random() * total;
        for (let i = 0; i < edges.length; i++) {
            pick -= weights[i];
            if (pick <= 0) return edges[i];
        }
        return edges[edges.length - 1];
    }

    selectChild(node) {
        if (node.children) return this.chooseWeighted(node.children);
        if (node.chanceGroups) {
            const possible = node.chanceGroups.filter(group => group.edges.length);
            if (!possible.length) return null;
            const group = possible[Math.floor(Math.random() * possible.length)];
            return this.chooseWeighted(group.edges);
        }
        return null;
    }

    searchStep(start = this.root) {
        if (!start) return { terminal: true };
        let node = start;
        let depth = 0;
        while ((node.children || node.chanceGroups) && depth < 32) {
            const edge = this.selectChild(node);
            if (!edge) break;
            node = edge.child;
            depth++;
        }
        if (!node.terminal && depth < 32) this.expand(node);
        return { terminal: node.terminal && node === start };
    }

    think(milliseconds, nodeLimit, start = this.root) {
        if (!start) return { iterations: 0, nodesAdded: 0 };
        const deadline = performance.now() + Math.max(1, milliseconds);
        const before = this.nodes.size;
        let iterations = 0;
        while (performance.now() < deadline && this.nodes.size < nodeLimit) {
            const result = this.searchStep(start);
            iterations++;
            if (result.terminal) break;
        }
        return { iterations, nodesAdded: this.nodes.size - before };
    }

    // Score tools need a reproducible "amount of thought" rather than the
    // interactive worker's short time slice.  The deadline remains a safety
    // valve for pathological transposition-only positions; `nodeBudget` is a
    // total DAG-node target, so callers can report exactly how far it got.
    thinkNodes(nodeBudget, maxMilliseconds = 4000, start = this.root) {
        if (!start) return { iterations: 0, nodesAdded: 0, complete: true };
        const before = this.nodes.size;
        const target = Math.max(before, Math.floor(Number(nodeBudget) || 0));
        const deadline = performance.now() + Math.max(1, maxMilliseconds);
        // A transposition can make a search step add no node.  Bound those
        // steps too, otherwise a position with no new states would burn the
        // full deadline without improving the analysis.
        const iterationLimit = Math.max(256, (target - before + 1) * 32);
        let iterations = 0;
        let terminal = false;
        while (this.nodes.size < target && performance.now() < deadline && iterations < iterationLimit) {
            const result = this.searchStep(start);
            iterations++;
            if (result.terminal) {
                terminal = true;
                break;
            }
        }
        return {
            iterations,
            nodesAdded: this.nodes.size - before,
            complete: this.nodes.size >= target || terminal
        };
    }

    // While the UI is animating the chosen move, prioritize the child that is
    // expected to become the next root.  This is the useful "think ahead"
    // work that the old start/stop protocol accidentally removed.
    thinkAhead(milliseconds, nodeLimit) {
        return this.think(milliseconds, nodeLimit, this.pendingEdge ? this.pendingEdge.child : this.root);
    }

    best(incoming) {
        if (!this.root || !this.root.children || !this.root.children.length) return null;
        const candidates = [...this.root.children].sort((a, b) => ccCompareValue(this.edgeValue(b), this.edgeValue(a)));
        let fallback = candidates[0];
        if (incoming > 0) {
            for (const edge of candidates) {
                const heights = edge.child.state.board.heights;
                if (heights.slice(3, 6).every(height => incoming - edge.garbage + height <= 20)) return edge;
                if (edge.child.value.spike > fallback.child.value.spike) fallback = edge;
            }
        }
        return fallback;
    }

    commit() {
        if (!this.pendingEdge) return false;
        this.root = this.pendingEdge.child;
        this.root.parents = [];
        this.pendingEdge = null;
        this.prune();
        return true;
    }

    prune() {
        if (!this.root) return;
        const reachable = new Set();
        const stack = [this.root];
        while (stack.length) {
            const node = stack.pop();
            if (reachable.has(node)) continue;
            reachable.add(node);
            if (node.children) for (const edge of node.children) stack.push(edge.child);
            if (node.chanceGroups) for (const group of node.chanceGroups) for (const edge of group.edges) stack.push(edge.child);
        }
        this.nodes = new Map();
        for (const node of reachable) {
            node.parents = node.parents.filter(edge => reachable.has(edge.parent));
            this.nodes.set(node.key, node);
        }
    }

    analyze(snapshot) {
        const synchronized = this.synchronize(snapshot);
        this.think(snapshot.thinkTimeMs || 45, snapshot.nodeLimit || 30000);
        const edge = this.best(snapshot.incoming || 0);
        this.pendingEdge = edge;
        if (!edge) return { ...synchronized, move: null };
        const placement = edge.placement;
        return {
            ...synchronized,
            move: {
                piece: placement.type,
                // Player's I anchor is one cell left of the solver's SRS anchor.
                x: placement.type === 'I' ? placement.x - 1 : placement.x,
                y: placement.y,
                rotation: placement.rotation,
                tspin: placement.tspin === CC_TSPIN_FULL ? 'full' : (placement.tspin === CC_TSPIN_MINI ? 'mini' : null),
                hold: edge.hold,
                inputs: placement.inputs
            }
        };
    }
}

self.ColdClearSimulatorCore = Object.freeze({
    Search: CCSearch,
    Board: CCBoard,
    findMoves: ccFindMoves,
    defaults: CC_DEFAULT_WEIGHTS
});
