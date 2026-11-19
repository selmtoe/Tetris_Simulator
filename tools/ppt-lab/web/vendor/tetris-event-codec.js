/*
 * Tetris event replay codec (te1 / schema v4).
 *
 * The editor keeps a page-oriented model internally, but persisted replays
 * contain only player advances: the previous lock, optional garbage rows,
 * and changed queue/HOLD metadata.  An exact board is emitted every 64
 * player advances and whenever the semantic transition cannot reproduce the
 * source board.  Those checkpoints make recovered video data self-healing
 * without repeating two complete 40x10 boards at every 2P timestamp.
 */
(function exposeTetrisEventCodec(root) {
    'use strict';

    const WIDTH = 10;
    const HEIGHT = 40;
    const EMPTY = '_';
    const FORMAT = 'te1';
    const VERSION = 4;
    const DEFAULT_CHECKPOINT_INTERVAL = 64;
    const PIECES = new Set(['I', 'O', 'T', 'L', 'J', 'S', 'Z']);
    const ROTATIONS = ['spawn', 'right', 'reverse', 'left'];
    const SHAPES = {
        I: { cells: [[0, 0], [1, 0], [2, 0], [3, 0]], center: [1.5, 0.5] },
        O: { cells: [[0, 0], [1, 0], [0, -1], [1, -1]], center: [0.5, -0.5] },
        T: { cells: [[0, 0], [-1, 0], [0, -1], [1, 0]], center: [0, 0] },
        L: { cells: [[-1, 0], [0, 0], [1, 0], [1, -1]], center: [0, 0] },
        J: { cells: [[0, 0], [-1, 0], [1, 0], [-1, -1]], center: [0, 0] },
        S: { cells: [[1, -1], [-1, 0], [0, 0], [0, -1]], center: [0, 0] },
        Z: { cells: [[0, 0], [1, 0], [0, -1], [-1, -1]], center: [0, 0] }
    };

    const hasOwn = (value, key) => Object.prototype.hasOwnProperty.call(value || {}, key);
    const emptyBoardString = () => EMPTY.repeat(WIDTH * HEIGHT);
    const cleanPiece = value => {
        const piece = String(value || '').toUpperCase();
        return PIECES.has(piece) ? piece : '';
    };
    const cleanSequence = value => String(value || '').toUpperCase().split('')
        .filter(piece => PIECES.has(piece)).join('');

    function boardString(value) {
        if (typeof value === 'string') {
            if (value.length === WIDTH * HEIGHT) {
                return value.split('').map(cell => cell === 'E' || cell === '0' ? EMPTY : cell).join('');
            }
            if (value.includes('.') || value === '') return unpackBoard(value);
        }
        if (Array.isArray(value)) {
            if (value.length && Array.isArray(value[0])) {
                const result = emptyBoardString().split('');
                const offset = value.length === 20 ? 20 : 0;
                value.slice(0, HEIGHT).forEach((row, sourceY) => {
                    const y = sourceY + offset;
                    if (y >= HEIGHT) return;
                    for (let x = 0; x < WIDTH; x++) {
                        const cell = row?.[x];
                        result[y * WIDTH + x] = cell && cell !== 'E' && cell !== '0' ? String(cell) : EMPTY;
                    }
                });
                return result.join('');
            }
            return boardString(value.join(''));
        }
        return emptyBoardString();
    }

    function boardMatrix(value) {
        const text = boardString(value);
        return Array.from({ length: HEIGHT }, (_, y) => Array.from({ length: WIDTH }, (_, x) => {
            const cell = text[y * WIDTH + x];
            return cell === EMPTY ? null : cell;
        }));
    }

    function packBoard(value) {
        const text = boardString(value);
        let firstRow = 0;
        while (firstRow < HEIGHT && text.slice(firstRow * WIDTH, (firstRow + 1) * WIDTH) === EMPTY.repeat(WIDTH)) {
            firstRow++;
        }
        if (firstRow === HEIGHT) return '';
        return `${firstRow.toString(36)}.${text.slice(firstRow * WIDTH)}`;
    }

    function unpackBoard(value) {
        const packed = String(value || '');
        if (!packed) return emptyBoardString();
        if (packed.length === WIDTH * HEIGHT && !packed.includes('.')) return boardString(packed);
        const dot = packed.indexOf('.');
        if (dot < 0) return (packed + emptyBoardString()).slice(0, WIDTH * HEIGHT);
        const firstRow = Math.max(0, Math.min(HEIGHT, parseInt(packed.slice(0, dot), 36) || 0));
        return (EMPTY.repeat(firstRow * WIDTH) + packed.slice(dot + 1) + emptyBoardString())
            .slice(0, WIDTH * HEIGHT);
    }

    function rotationIndex(value) {
        if (Number.isFinite(Number(value))) return ((Math.round(Number(value)) % 4) + 4) % 4;
        const index = ROTATIONS.indexOf(String(value || '').toLowerCase());
        return index < 0 ? 0 : index;
    }

    function normalizeOperation(value) {
        if (!value || typeof value !== 'object') return null;
        const type = cleanPiece(value.type || value.piece);
        const x = Number(value.x);
        const y = Number(value.y);
        if (!type || !Number.isFinite(x) || !Number.isFinite(y)) return null;
        return {
            type,
            rotation: ROTATIONS[rotationIndex(value.rotation)],
            x: Math.round(x),
            y: Math.round(y),
            coordinateSpace: 'simulator',
            lock: value.lock !== false,
            holdUsed: Boolean(value.holdUsed || value.hold || value.source === 'hold')
        };
    }

    function operationArray(value) {
        const operation = normalizeOperation(value);
        if (!operation) return 0;
        const packed = [operation.type, rotationIndex(operation.rotation), operation.x, operation.y];
        if (operation.holdUsed) packed.push(1);
        return packed;
    }

    function operationObject(value) {
        if (!Array.isArray(value) || value.length < 4) return null;
        const operation = normalizeOperation({
            type: value[0], rotation: Number(value[1]), x: Number(value[2]), y: Number(value[3]),
            holdUsed: Boolean(value[4])
        });
        return operation ? operation : null;
    }

    function operationKey(value) {
        return JSON.stringify(operationArray(value));
    }

    function shapeCells(operation) {
        const normalized = normalizeOperation(operation);
        if (!normalized) return [];
        const definition = SHAPES[normalized.type];
        if (!definition) return [];
        if (normalized.type === 'O' || rotationIndex(normalized.rotation) === 0) {
            return definition.cells.map(([x, y]) => [normalized.x + x, normalized.y + y]);
        }
        return definition.cells.map(([sourceX, sourceY]) => {
            let x = sourceX - definition.center[0];
            let y = sourceY - definition.center[1];
            for (let turn = 0; turn < rotationIndex(normalized.rotation); turn++) [x, y] = [-y, x];
            return [normalized.x + Math.round(x + definition.center[0]),
                normalized.y + Math.round(y + definition.center[1])];
        });
    }

    function applyOperation(value, operationValue) {
        const operation = normalizeOperation(operationValue);
        if (!operation) return boardString(value);
        const cells = shapeCells(operation);
        if (cells.length !== 4 || cells.some(([x, y]) => x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT)) {
            return boardString(value);
        }
        const result = boardString(value).split('');
        for (const [x, y] of cells) result[y * WIDTH + x] = operation.type;
        const rows = [];
        for (let y = 0; y < HEIGHT; y++) {
            const row = result.slice(y * WIDTH, (y + 1) * WIDTH);
            if (row.some(cell => cell === EMPTY)) rows.push(row);
        }
        while (rows.length < HEIGHT) rows.unshift(Array(WIDTH).fill(EMPTY));
        return rows.flat().join('');
    }

    function detectGarbage(beforeValue, afterValue) {
        const before = boardString(beforeValue);
        const after = boardString(afterValue);
        for (let lines = 1; lines <= 20; lines++) {
            if (after.slice(0, (HEIGHT - lines) * WIDTH) !== before.slice(lines * WIDTH)) continue;
            let holes = '';
            let fills = '';
            let valid = true;
            for (let row = HEIGHT - lines; row < HEIGHT; row++) {
                const cells = after.slice(row * WIDTH, (row + 1) * WIDTH).split('');
                const empty = cells.reduce((indices, cell, x) => cell === EMPTY ? indices.concat(x) : indices, []);
                const occupied = cells.filter(cell => cell !== EMPTY);
                const fill = occupied[0] || '';
                if (empty.length !== 1 || occupied.length !== WIDTH - 1 || occupied.some(cell => cell !== fill)) {
                    valid = false;
                    break;
                }
                holes += empty[0].toString(36);
                fills += fill;
            }
            if (!valid) continue;
            const uniformFill = fills.split('').every(cell => cell === fills[0]) ? fills[0] : fills;
            return [holes, uniformFill];
        }
        return null;
    }

    function applyGarbage(value, encoded) {
        if (!Array.isArray(encoded) || encoded.length < 1) return boardString(value);
        const holes = String(encoded[0] || '');
        const fillValue = String(encoded[1] || 'G');
        if (!holes || holes.length > HEIGHT) return boardString(value);
        const before = boardString(value);
        const rows = [];
        for (let index = 0; index < holes.length; index++) {
            const hole = parseInt(holes[index], 36);
            if (!Number.isFinite(hole) || hole < 0 || hole >= WIDTH) return before;
            const fill = fillValue.length === holes.length ? fillValue[index] : fillValue[0] || 'G';
            const row = Array(WIDTH).fill(fill);
            row[hole] = EMPTY;
            rows.push(row.join(''));
        }
        return before.slice(holes.length * WIDTH) + rows.join('');
    }

    function pagePlayer(value) {
        const player = value || {};
        return {
            board: boardString(player.board || player.b),
            activePresent: hasOwn(player, 'active') || hasOwn(player, 'a'),
            active: cleanPiece(player.active || player.a),
            hold: cleanPiece(player.hold || player.h),
            next: cleanSequence(hasOwn(player, 'next') ? player.next : player.n),
            operation: normalizeOperation(player.operation || player.o || player.placement),
            ui: [
                String(player.activeColor || 'I'),
                Number.isFinite(Number(player.viewY)) ? Math.round(Number(player.viewY)) : 20,
                hasOwn(player, 'nextInsertionIndex') ? player.nextInsertionIndex : -1,
                player.placementMode ? 1 : 0,
                Array.isArray(player.placementDraft) ? player.placementDraft : []
            ]
        };
    }

    function uiKey(value) {
        return JSON.stringify(value || ['I', 20, -1, 0, []]);
    }

    function playerKey(value) {
        const player = pagePlayer(value);
        return [player.board, player.activePresent ? `a:${player.active}` : '-', player.hold,
            player.next, operationKey(player.operation), uiKey(player.ui)].join('|');
    }

    function initialArray(value, fallbackPage) {
        const initial = value || {};
        return [
            packBoard(initial.board || initial.b || fallbackPage?.board || fallbackPage?.b),
            cleanPiece(initial.hold || initial.h),
            cleanSequence(initial.sequence || initial.s)
        ];
    }

    function encodePlayerDelta(currentValue, previousValue, first, kind, forceCheckpoint) {
        const current = pagePlayer(currentValue);
        const previous = previousValue ? pagePlayer(previousValue) : null;
        const delta = {};
        let expected = first || kind !== 'replay'
            ? (previous ? previous.board : current.board)
            : applyOperation(previous.board, previous.operation);

        if (!first && kind === 'replay' && expected !== current.board) {
            const garbage = detectGarbage(expected, current.board);
            if (garbage) {
                delta.g = garbage;
                expected = applyGarbage(expected, garbage);
            }
        }
        if ((first && (!previous || current.board !== previous.board)) || expected !== current.board || forceCheckpoint) {
            delta.b = packBoard(current.board);
            delete delta.g;
        }

        if (!previous || current.activePresent !== previous.activePresent ||
            (current.activePresent && current.active !== previous.active)) {
            delta.a = current.activePresent ? current.active : null;
        }
        if (!previous || current.hold !== previous.hold) delta.h = current.hold;
        if (!previous || current.next !== previous.next) delta.n = current.next;
        if (!previous || operationKey(current.operation) !== operationKey(previous.operation)) {
            delta.o = operationArray(current.operation);
        }
        if (!previous || uiKey(current.ui) !== uiKey(previous.ui)) delta.u = current.ui;
        return delta;
    }

    function eventTime(page, index) {
        const seconds = Number(page?.time ?? page?._time);
        return Number.isFinite(seconds) ? Math.max(0, Math.round(seconds * 1000)) : index;
    }

    function encodeCase(caseValue, checkpointInterval) {
        const source = caseValue || {};
        const pages = Array.isArray(source.pages) && source.pages.length ? source.pages : [{}];
        const mode = source.gameMode === '2P' ? 2 : 1;
        const kind = source.kind === 'replay' ? 'replay' : 'snapshot';
        const initial = source.initial || {};
        const encoded = {
            i: String(source.id || ''),
            n: String(source.name || 'Case'),
            k: kind === 'replay' ? 'r' : 's',
            m: mode,
            s: {
                1: initialArray(initial.p1, pagePlayer(pages[0]?.p1)),
                2: initialArray(initial.p2, pagePlayer(pages[0]?.p2))
            },
            e: []
        };
        const previous = { p1: null, p2: null };
        const advances = { p1: 0, p2: 0 };
        let previousTime = 0;
        pages.forEach((page, index) => {
            const time = eventTime(page, index);
            const event = [Math.max(0, time - previousTime), 0, 0];
            previousTime = time;
            ['p1', 'p2'].forEach((playerId, playerIndex) => {
                if (playerId === 'p2' && mode !== 2) return;
                const currentValue = page?.[playerId] || {};
                const changed = !previous[playerId] || playerKey(currentValue) !== playerKey(previous[playerId]);
                if (!changed) return;
                advances[playerId]++;
                const forceCheckpoint = advances[playerId] > 1 && advances[playerId] % checkpointInterval === 0;
                event[playerIndex + 1] = encodePlayerDelta(
                    currentValue, previous[playerId], !previous[playerId], kind, forceCheckpoint);
                previous[playerId] = currentValue;
            });
            encoded.e.push(event);
        });
        return encoded;
    }

    function encodeCollection(collection, options = {}) {
        if (isEventReplay(collection)) return collection;
        if (!collection || collection.v !== 3 || !Array.isArray(collection.cases)) {
            throw new Error('TetrisEventCodec expects a v3 collection');
        }
        const checkpointInterval = Math.max(1, Math.round(Number(options.checkpointInterval) || DEFAULT_CHECKPOINT_INTERVAL));
        return {
            v: VERSION,
            f: FORMAT,
            m: collection.m === '2P' ? 2 : 1,
            c: Math.max(0, Math.round(Number(collection.currentCase) || 0)),
            x: checkpointInterval,
            cases: collection.cases.map(caseValue => encodeCase(caseValue, checkpointInterval))
        };
    }

    function initialState(value) {
        const initial = Array.isArray(value) ? value : [];
        return {
            board: unpackBoard(initial[0]),
            activePresent: false,
            active: '',
            hold: cleanPiece(initial[1]),
            next: '',
            operation: null,
            ui: ['I', 20, -1, 0, []],
            advances: 0
        };
    }

    function applyPlayerDelta(state, value, kind) {
        if (!value || typeof value !== 'object') return;
        if (state.advances > 0 && kind === 'replay' && !hasOwn(value, 'b')) {
            state.board = applyOperation(state.board, state.operation);
            if (value.g) state.board = applyGarbage(state.board, value.g);
        }
        if (hasOwn(value, 'b')) state.board = unpackBoard(value.b);
        if (hasOwn(value, 'a')) {
            state.activePresent = value.a !== null;
            state.active = value.a === null ? '' : cleanPiece(value.a);
        }
        if (hasOwn(value, 'h')) state.hold = cleanPiece(value.h);
        if (hasOwn(value, 'n')) state.next = cleanSequence(value.n);
        if (hasOwn(value, 'o')) state.operation = value.o ? operationObject(value.o) : null;
        if (hasOwn(value, 'u') && Array.isArray(value.u)) state.ui = value.u;
        state.advances++;
    }

    function decodedPlayer(state, options = {}) {
        const ui = Array.isArray(state.ui) ? state.ui : [];
        const player = {
            // The editor can keep untouched replay boards compact and expand
            // only the page being viewed.  The public/default decode shape is
            // unchanged for callers that expect a 40x10 matrix.
            board: options.compactBoards ? boardString(state.board) : boardMatrix(state.board),
            hold: state.hold,
            next: state.next,
            operation: state.operation ? { ...state.operation } : null,
            activeColor: String(ui[0] || 'I'),
            viewY: Number.isFinite(Number(ui[1])) ? Number(ui[1]) : 20,
            nextInsertionIndex: hasOwn(ui, 2) ? ui[2] : -1,
            placementMode: Boolean(ui[3]),
            placementDraft: Array.isArray(ui[4]) ? JSON.parse(JSON.stringify(ui[4])) : []
        };
        if (state.activePresent) player.active = state.active;
        return player;
    }

    function decodeCase(source, options = {}) {
        const kind = source?.k === 'r' ? 'replay' : 'snapshot';
        const mode = Number(source?.m) === 2 ? '2P' : '1P';
        const state = {
            p1: initialState(source?.s?.['1']),
            p2: initialState(source?.s?.['2'])
        };
        const pages = [];
        let time = 0;
        for (const event of Array.isArray(source?.e) ? source.e : []) {
            if (!Array.isArray(event)) continue;
            time += Math.max(0, Number(event[0]) || 0);
            applyPlayerDelta(state.p1, event[1], kind);
            if (mode === '2P') applyPlayerDelta(state.p2, event[2], kind);
            const page = { p1: decodedPlayer(state.p1, options), time: time / 1000 };
            if (mode === '2P') page.p2 = decodedPlayer(state.p2, options);
            pages.push(page);
        }
        if (!pages.length) {
            const page = { p1: decodedPlayer(state.p1, options), time: 0 };
            if (mode === '2P') page.p2 = decodedPlayer(state.p2, options);
            pages.push(page);
        }
        const initialFor = (value, playerState) => {
            const packed = Array.isArray(value) ? value : [];
            return {
                board: options.compactBoards ? boardString(playerState.board) : boardMatrix(playerState.board),
                hold: cleanPiece(packed[1]),
                sequence: cleanSequence(packed[2])
            };
        };
        return {
            id: String(source?.i || ''),
            name: String(source?.n || 'Case'),
            kind,
            gameMode: mode,
            initial: {
                p1: initialFor(source?.s?.['1'], initialState(source?.s?.['1'])),
                p2: initialFor(source?.s?.['2'], initialState(source?.s?.['2']))
            },
            pages
        };
    }

    function decodeCollection(value, options = {}) {
        if (!isEventReplay(value)) throw new Error('Unsupported event replay format');
        const cases = Array.isArray(value.cases) ? value.cases.map(caseValue => decodeCase(caseValue, options)) : [];
        return {
            v: 3,
            m: Number(value.m) === 2 ? '2P' : '1P',
            currentCase: Math.max(0, Math.min(Number(value.c) || 0, Math.max(0, cases.length - 1))),
            cases
        };
    }

    function isEventReplay(value) {
        return Boolean(value && Number(value.v) === VERSION && value.f === FORMAT && Array.isArray(value.cases));
    }

    const api = {
        VERSION,
        FORMAT,
        DEFAULT_CHECKPOINT_INTERVAL,
        isEventReplay,
        encodeCollection,
        decodeCollection,
        packBoard,
        unpackBoard,
        boardString,
        boardMatrix,
        applyOperation,
        applyGarbage,
        detectGarbage,
        operationArray,
        operationObject
    };
    root.TetrisEventCodec = api;
    if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : window);
