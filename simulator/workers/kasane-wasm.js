/* Raw WASM bridge for KASANE Base v3 + Basic/Guard tactics. */

'use strict';

const KASANE_MOVE_SIZE = 32;
const KASANE_INTENTS = [
    'Stack',
    'Spike now',
    'Cancellation dodge',
    'Counter',
    'Tank then fire',
    'Charge',
    'Dig',
    'Combo continue',
    'Survival'
];
const KASANE_STRATEGY_EVENTS = [
    'None',
    'Attack expert',
    'Attack expert hold-fire',
    'Survival expert',
    'Survival expert hold-fire',
    'Charge enter',
    'Charge continue',
    'Armed',
    'Charge release incoming edge',
    'Charge release garbage rise edge',
    'Release dodge',
    'Release counter',
    'Release immediate',
    'Charge abort',
    'REN start',
    'REN continue',
    'Stack-REN enter',
    'Stack-REN build',
    'Stack-REN fire',
    'Stack-REN continue',
    'Stack-REN abort'
];
const KASANE_POLICY_OVERRIDES = [
    'None',
    'Same placement dodge',
    'Same placement counter',
    'Same placement wait',
    'Placement',
    'Placement and wait'
];

class KasaneWasmBridge {
    constructor(instance) {
        this.instance = instance;
        this.exports = instance.exports;
        this.memory = this.exports.memory;
        this.moveSize = this.exports.ks_move_size ? this.exports.ks_move_size() : KASANE_MOVE_SIZE;
        if (!this.memory || !this.exports.ks_alloc || !this.exports.ks_dealloc ||
            !this.exports.ks_choose_json || this.moveSize < KASANE_MOVE_SIZE) {
            throw new Error('KASANE WASM ABI is incomplete.');
        }
    }

    static async load(wasmPath = './kasane.wasm?v=kasane-v8') {
        const response = await fetch(wasmPath, { credentials: 'same-origin' });
        if (!response.ok) throw new Error(`KASANE WASM HTTP ${response.status}`);
        let result;
        try {
            result = await WebAssembly.instantiateStreaming(response.clone(), {});
        } catch (_) {
            result = await WebAssembly.instantiate(await response.arrayBuffer(), {});
        }
        return new KasaneWasmBridge(result.instance);
    }

    normalizeBoard(board) {
        if (!Array.isArray(board) || board.length !== 40 ||
            board.some(row => !Array.isArray(row) || row.length !== 10)) {
            throw new Error('KASANE requires a 40x10 board snapshot.');
        }
        const flat = new Array(400);
        let index = 0;
        for (const row of board) {
            for (const cell of row) flat[index++] = cell === null ? 0 : 1;
        }
        return flat;
    }

    normalizePlayer(player) {
        if (!player) throw new Error('KASANE player snapshot is missing.');
        return {
            ...player,
            board: this.normalizeBoard(player.board),
            currentPiece: String(player.currentPiece || ''),
            nextQueue: Array.isArray(player.nextQueue) ? player.nextQueue : [],
            holdPiece: player.holdPiece || null,
            incoming: Array.isArray(player.incoming) ? player.incoming : [],
            phase: player.phase || { kind: 'ready' }
        };
    }

    choose(snapshot) {
        const payload = {
            ...snapshot,
            own: this.normalizePlayer(snapshot.own),
            opponent: this.normalizePlayer(snapshot.opponent)
        };
        const bytes = new TextEncoder().encode(JSON.stringify(payload));
        const inputPtr = this.exports.ks_alloc(bytes.length);
        const outputPtr = this.exports.ks_alloc(this.moveSize);
        if (!inputPtr || !outputPtr) {
            if (inputPtr) this.exports.ks_dealloc(inputPtr, bytes.length);
            if (outputPtr) this.exports.ks_dealloc(outputPtr, this.moveSize);
            throw new Error('KASANE WASM allocation failed.');
        }

        try {
            new Uint8Array(this.memory.buffer, inputPtr, bytes.length).set(bytes);
            this.exports.ks_choose_json(inputPtr, bytes.length, outputPtr);
            const view = new DataView(this.memory.buffer, outputPtr, this.moveSize);
            const status = view.getUint32(0, true);
            if (status === 0 || status === 3) return null;
            if (status !== 1) throw new Error(`KASANE rejected the snapshot (status ${status}).`);
            const intentIndex = view.getUint8(24);
            return {
                piece: String.fromCharCode(view.getUint8(4)),
                hold: view.getUint8(5) !== 0,
                rotation: view.getUint8(6),
                tspin: view.getUint8(7),
                x: view.getInt32(8, true),
                y: view.getInt32(12, true),
                waitMs: view.getUint32(16, true),
                score: view.getFloat32(20, true),
                intent: KASANE_INTENTS[intentIndex] || `Intent ${intentIndex}`,
                strategyEvent: KASANE_STRATEGY_EVENTS[view.getUint8(25)] || 'None',
                policyOverride: KASANE_POLICY_OVERRIDES[view.getUint8(26)] || 'None',
                strategyDetail: view.getUint8(27),
                attack: view.getUint32(28, true)
            };
        } finally {
            this.exports.ks_dealloc(outputPtr, this.moveSize);
            this.exports.ks_dealloc(inputPtr, bytes.length);
        }
    }

    resetController() {
        if (this.exports.ks_reset_controller_cache) {
            this.exports.ks_reset_controller_cache();
        }
    }

    invalidateSearch() {
        if (this.exports.ks_invalidate_search) {
            this.exports.ks_invalidate_search();
        }
    }
}

self.KasaneWasmBridge = KasaneWasmBridge;
