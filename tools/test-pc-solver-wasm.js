'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');
const createSfinderPcModule = require('../simulator/pc-solver/sfinder-pc.js');

const OPERATION_STRIDE = 12;

function rowMask(row) {
    let mask = 0;
    for (let x = 0; x < 10; x++) {
        if (row[x] !== '_') mask |= 1 << x;
    }
    return mask;
}

async function main() {
    const module = await createSfinderPcModule({
        locateFile: file => path.join(__dirname, '..', 'simulator', 'pc-solver', file),
        print: () => {},
        printErr: message => process.stderr.write(`${message}\n`)
    });

    const rows = new Uint16Array(24);
    rows[0] = rowMask('XXXXXX_XXX');
    rows[1] = rowMask('_XXXXXXXXX');
    rows[2] = rowMask('_XXXXXX___');
    const pieces = Uint8Array.from([1, 3, 3, 4, 6, 2, 5, 0, 1, 5]);
    const depth = 9;

    const rowsPtr = module._malloc(rows.byteLength);
    const piecesPtr = module._malloc(pieces.byteLength);
    const outputPtr = module._malloc(depth * OPERATION_STRIDE * Int32Array.BYTES_PER_ELEMENT);
    try {
        module.HEAPU16.set(rows, rowsPtr >>> 1);
        module.HEAPU8.set(pieces, piecesPtr);
        const count = module._sfinder_find_pc(
            rowsPtr, rows.length, piecesPtr, pieces.length, depth, 6, 0, outputPtr, depth
        );
        assert.equal(count, depth, 'the bundled sample must produce a nine-step PC');

        const output = module.HEAP32.slice(outputPtr >>> 2, (outputPtr >>> 2) + count * OPERATION_STRIDE);
        for (let operation = 0; operation < count; operation++) {
            for (let cell = 0; cell < 4; cell++) {
                const x = output[operation * OPERATION_STRIDE + 4 + cell * 2];
                const y = output[operation * OPERATION_STRIDE + 5 + cell * 2];
                assert.ok(x >= 0 && x < 10 && y >= 0 && y < 24, 'guide cell must be in bounds');
            }
        }
    } finally {
        module._free(outputPtr);
        module._free(piecesPtr);
        module._free(rowsPtr);
    }

    console.log('PC WASM smoke test passed.');
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
