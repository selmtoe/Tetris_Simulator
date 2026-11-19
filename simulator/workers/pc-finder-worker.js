/*
 * On-demand perfect-clear search Worker.
 *
 * The Cold Clear Worker is deliberately not reused: a PC query is a bounded
 * one-shot operation and can be safely terminated by the UI on timeout.
 */

'use strict';

const MAX_SOLVER_HEIGHT = 24;
const OPERATION_STRIDE = 12;
const PIECE_CODES = Object.freeze({ T: 0, I: 1, L: 2, J: 3, S: 4, Z: 5, O: 6 });
const PIECE_NAMES = Object.freeze(['T', 'I', 'L', 'J', 'S', 'Z', 'O']);
const PC_GUIDE_ASSET_VERSION = 'app-v5';

let solverPromise = null;
let glueLoaded = false;

function isPiece(value) {
    return typeof value === 'string' && Object.hasOwn(PIECE_CODES, value);
}

function loadSolver() {
    if (!solverPromise) {
        if (!glueLoaded) {
            importScripts(`../pc-solver/sfinder-pc.js?v=${PC_GUIDE_ASSET_VERSION}`);
            glueLoaded = true;
        }

        if (typeof self.createSfinderPcModule !== 'function') {
            throw new Error('PC solver glue did not expose createSfinderPcModule. Build sfinder-pc.js first.');
        }

        solverPromise = self.createSfinderPcModule({
            // The glue is imported by this Worker, so its default base URL is
            // the workers/ directory rather than pc-solver/.
            locateFile: file => {
                const assetUrl = new URL(`../pc-solver/${file}`, self.location.href);
                assetUrl.searchParams.set('v', PC_GUIDE_ASSET_VERSION);
                return assetUrl.href;
            },
            print: () => {},
            printErr: message => console.warn('[PC solver]', message)
        });
    }

    return solverPromise;
}

function boardToRows(board) {
    if (!Array.isArray(board) || board.length < 1) {
        return { error: 'invalid_board' };
    }

    const rows = new Uint16Array(MAX_SOLVER_HEIGHT);
    let blockCount = 0;
    let highestOccupiedRow = -1;
    const boardHeight = board.length;

    for (let sourceY = 0; sourceY < boardHeight; sourceY++) {
        const sourceRow = board[sourceY];
        if (!Array.isArray(sourceRow) || sourceRow.length < 10) {
            return { error: 'invalid_board' };
        }

        const solverY = boardHeight - 1 - sourceY;
        for (let x = 0; x < 10; x++) {
            if (sourceRow[x] == null) continue;
            if (solverY >= MAX_SOLVER_HEIGHT) {
                return { error: 'board_too_high' };
            }
            rows[solverY] |= 1 << x;
            blockCount++;
            highestOccupiedRow = Math.max(highestOccupiedRow, solverY);
        }
    }

    return { rows, blockCount, highestOccupiedRow, boardHeight };
}

function buildSequence(data) {
    if (!isPiece(data.currentPiece)) {
        return { error: 'invalid_current_piece' };
    }
    if (data.holdDisabled || data.canHold === false) {
        // The C++ core assumes the first hold is available.  Searching a
        // hold-locked state would produce a misleading guide, so ask for the
        // next spawn instead.
        return { error: 'hold_unavailable' };
    }

    const known = [data.currentPiece];
    const queue = Array.isArray(data.nextQueue) ? data.nextQueue : [];
    for (const piece of queue) {
        if (piece === 'E') break;
        if (!isPiece(piece)) return { error: 'invalid_next_piece' };
        known.push(piece);
    }

    const hold = data.holdPiece == null ? null : data.holdPiece;
    if (hold !== null && !isPiece(hold)) {
        return { error: 'invalid_hold_piece' };
    }

    const solverPieces = hold === null ? known : [hold, ...known];
    return {
        knownCount: known.length,
        pieces: Uint8Array.from(solverPieces, piece => PIECE_CODES[piece]),
        holdEmpty: hold === null
    };
}

function isEmptyAboveTarget(highestOccupiedRow, targetLines) {
    return highestOccupiedRow >= targetLines;
}

function callSolver(Module, rows, pieces, maxDepth, maxLines, holdEmpty) {
    const rowsPtr = Module._malloc(rows.byteLength);
    const piecesPtr = Module._malloc(pieces.byteLength);
    const outputPtr = Module._malloc(maxDepth * OPERATION_STRIDE * Int32Array.BYTES_PER_ELEMENT);

    try {
        Module.HEAPU16.set(rows, rowsPtr >>> 1);
        Module.HEAPU8.set(pieces, piecesPtr);
        const operationCount = Module._sfinder_find_pc(
            rowsPtr,
            rows.length,
            piecesPtr,
            pieces.length,
            maxDepth,
            maxLines,
            holdEmpty ? 1 : 0,
            outputPtr,
            maxDepth
        );

        if (operationCount <= 0) {
            return { operationCount };
        }

        const raw = Module.HEAP32.slice(
            outputPtr >>> 2,
            (outputPtr >>> 2) + operationCount * OPERATION_STRIDE
        );
        return { operationCount, raw };
    } finally {
        Module._free(outputPtr);
        Module._free(piecesPtr);
        Module._free(rowsPtr);
    }
}

function decodePlan(raw, operationCount, boardHeight) {
    const plan = [];
    for (let index = 0; index < operationCount; index++) {
        const offset = index * OPERATION_STRIDE;
        const pieceCode = raw[offset];
        const cells = [];
        for (let cell = 0; cell < 4; cell++) {
            cells.push({
                x: raw[offset + 4 + cell * 2],
                y: boardHeight - 1 - raw[offset + 5 + cell * 2]
            });
        }
        plan.push({
            piece: PIECE_NAMES[pieceCode] || '?',
            rotation: raw[offset + 1],
            cells
        });
    }
    return plan;
}

async function findPerfectClear(data) {
    const field = boardToRows(data.board);
    if (field.error) return { status: 'unsupported', reason: field.error };

    const sequence = buildSequence(data);
    if (sequence.error) return { status: 'unsupported', reason: sequence.error };

    const candidates = [];
    for (let depth = 1; depth <= sequence.knownCount; depth++) {
        const totalBlocks = field.blockCount + depth * 4;
        if (totalBlocks % 10 !== 0) continue;

        const targetLines = totalBlocks / 10;
        if (targetLines < 1 || targetLines > MAX_SOLVER_HEIGHT) continue;
        if (isEmptyAboveTarget(field.highestOccupiedRow, targetLines)) continue;
        candidates.push({ depth, targetLines });
    }

    if (candidates.length === 0) {
        return { status: 'not_found', checked: 0 };
    }

    const Module = await loadSolver();
    for (const candidate of candidates) {
        const result = callSolver(
            Module,
            field.rows,
            sequence.pieces,
            candidate.depth,
            candidate.targetLines,
            sequence.holdEmpty
        );
        if (result.operationCount > 0) {
            return {
                status: 'found',
                depth: candidate.depth,
                lines: candidate.targetLines,
                plan: decodePlan(result.raw, result.operationCount, field.boardHeight)
            };
        }
        if (result.operationCount < 0) {
            throw new Error(`PC solver rejected input (${result.operationCount}).`);
        }
    }

    return { status: 'not_found', checked: candidates.length };
}

self.onmessage = async event => {
    const data = event.data || {};
    if (data.type !== 'search') return;

    const startedAt = performance.now();
    try {
        const result = await findPerfectClear(data);
        self.postMessage({
            type: 'result',
            requestId: data.requestId,
            elapsedMs: Math.round(performance.now() - startedAt),
            ...result
        });
    } catch (error) {
        self.postMessage({
            type: 'error',
            requestId: data.requestId,
            message: String(error && error.stack || error)
        });
    }
};
