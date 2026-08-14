/* Raw WASM bridge for the reference Cold Clear Standard bot. */

'use strict';

const COLD_CLEAR_MOVE_SIZE = 60;

class ColdClearWasmBridge {
    constructor(instance) {
        this.instance = instance;
        this.exports = instance.exports;
        this.memory = this.exports.memory;
        this.moveSize = this.exports.cc_move_size ? this.exports.cc_move_size() : COLD_CLEAR_MOVE_SIZE;
        if (!this.memory || !this.exports.cc_create || !this.exports.cc_alloc) {
            throw new Error('Cold Clear WASM ABI is incomplete.');
        }
    }

    static async load(wasmPath = './cold-clear.wasm') {
        const url = new URL(wasmPath, self.location.href);
        const response = await fetch(url);
        if (!response.ok) throw new Error(`Cold Clear WASM HTTP ${response.status}`);
        const bytes = await response.arrayBuffer();
        const result = await WebAssembly.instantiate(bytes, {});
        return new ColdClearWasmBridge(result.instance);
    }

    allocBytes(bytes) {
        const ptr = this.exports.cc_alloc(bytes.length);
        if (!ptr) throw new Error(`Cold Clear WASM allocation failed (${bytes.length}).`);
        new Uint8Array(this.memory.buffer, ptr, bytes.length).set(bytes);
        return ptr;
    }

    free(ptr, bytes) {
        this.exports.cc_dealloc(ptr, bytes.length);
    }

    create(snapshot) {
        const boardBytes = new Uint8Array(400);
        for (let y = 0; y < 40; y++) {
            const row = Array.isArray(snapshot.board?.[y]) ? snapshot.board[y] : [];
            for (let x = 0; x < 10; x++) boardBytes[y * 10 + x] = row[x] == null ? 0 : 1;
        }
        const current = new TextEncoder().encode(String(snapshot.currentPiece || ''))[0] || 0;
        const nextBytes = new TextEncoder().encode((snapshot.nextQueue || []).join(''));
        const hold = new TextEncoder().encode(String(snapshot.holdPiece || ''))[0] || 0;
        const weights = new TextEncoder().encode(JSON.stringify(snapshot.weights || {}));
        const boardPtr = this.allocBytes(boardBytes);
        const nextPtr = nextBytes.length ? this.allocBytes(nextBytes) : 0;
        const weightsPtr = weights.length ? this.allocBytes(weights) : 0;
        try {
            const handle = this.exports.cc_create(
                boardPtr,
                current,
                nextPtr,
                nextBytes.length,
                hold,
                snapshot.canHold === false ? 0 : 1,
                snapshot.isB2B ? 1 : 0,
                Number.isFinite(snapshot.ren) ? snapshot.ren : -1,
                Math.max(1, snapshot.nodeLimit | 0),
                weightsPtr,
                weights.length
            );
            if (!handle) throw new Error('Cold Clear WASM could not create a bot.');
            return handle;
        } finally {
            this.free(boardPtr, boardBytes);
            if (nextPtr) this.free(nextPtr, nextBytes);
            if (weightsPtr) this.free(weightsPtr, weights);
        }
    }

    nodeCount(handle) {
        return this.exports.cc_node_count(handle) >>> 0;
    }

    think(handle, milliseconds, nodeLimit) {
        const before = this.nodeCount(handle);
        const deadline = performance.now() + Math.max(1, milliseconds);
        let calls = 0;
        let after = before;
        while (performance.now() < deadline && after < nodeLimit) {
            const remaining = Math.max(1, nodeLimit - after);
            const batch = Math.min(256, remaining);
            const previous = after;
            after = this.exports.cc_think(handle, batch) >>> 0;
            calls += batch;
            if (after <= previous) break;
        }
        return { iterations: calls, nodesAdded: Math.max(0, after - before), count: after };
    }

    suggest(handle, incoming) {
        const ptr = this.exports.cc_alloc(this.moveSize);
        if (!ptr) throw new Error('Cold Clear WASM move allocation failed.');
        try {
            if (!this.exports.cc_suggest(handle, Math.max(0, incoming | 0), ptr)) return null;
            const view = new DataView(this.memory.buffer, ptr, this.moveSize);
            const bytes = new Uint8Array(this.memory.buffer, ptr, this.moveSize);
            return {
                piece: String.fromCharCode(bytes[4]),
                hold: bytes[5] !== 0,
                rotation: bytes[6],
                tspin: bytes[7] === 2 ? 'full' : (bytes[7] === 1 ? 'mini' : null),
                x: view.getInt32(8, true),
                y: view.getInt32(12, true),
                inputs: Array.from(bytes.slice(17, 17 + bytes[16])).map(value => String.fromCharCode(value)),
                nodes: view.getUint32(52, true),
                depth: view.getUint32(56, true)
            };
        } finally {
            this.exports.cc_dealloc(ptr, this.moveSize);
        }
    }

    commit(handle) {
        return this.exports.cc_commit(handle) !== 0;
    }

    addNextPiece(handle, piece) {
        return this.exports.cc_add_next_piece(handle, String(piece || '').charCodeAt(0)) !== 0;
    }

    destroy(handle) {
        if (handle) this.exports.cc_destroy(handle);
    }
}

self.ColdClearWasmBridge = ColdClearWasmBridge;
