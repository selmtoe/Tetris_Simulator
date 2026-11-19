(function (root, factory) {
    const api = factory();
    root.TetrisCellCnn = api;
    if (typeof module === 'object' && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : self, function () {
    'use strict';

    const CLASS_NAMES = Object.freeze(['null', 'G', 'S', 'Z', 'L', 'J', 'O', 'I', 'T']);
    const BOARD_COLUMNS = 10;
    const BOARD_ROWS = 20;
    const CELL_SIZE = 32;
    const BOARD_WIDTH = BOARD_COLUMNS * CELL_SIZE;
    const BOARD_HEIGHT = BOARD_ROWS * CELL_SIZE;
    const CHANNELS = 3;
    const CELL_VALUES = CHANNELS * CELL_SIZE * CELL_SIZE;

    function tensorDataFromBoardRgba(imageData) {
        if (!imageData || imageData.width !== BOARD_WIDTH || imageData.height !== BOARD_HEIGHT) {
            throw new Error(`Cell CNN expects a ${BOARD_WIDTH}x${BOARD_HEIGHT} RGBA board image.`);
        }
        const rgba = imageData.data;
        if (!rgba || rgba.length !== BOARD_WIDTH * BOARD_HEIGHT * 4) {
            throw new Error('Cell CNN received invalid RGBA data.');
        }
        const tensor = new Float32Array(BOARD_COLUMNS * BOARD_ROWS * CELL_VALUES);
        for (let boardRow = 0; boardRow < BOARD_ROWS; boardRow++) {
            for (let boardColumn = 0; boardColumn < BOARD_COLUMNS; boardColumn++) {
                const cell = boardRow * BOARD_COLUMNS + boardColumn;
                const cellOffset = cell * CELL_VALUES;
                for (let y = 0; y < CELL_SIZE; y++) {
                    const sourceY = boardRow * CELL_SIZE + y;
                    for (let x = 0; x < CELL_SIZE; x++) {
                        const sourceX = boardColumn * CELL_SIZE + x;
                        const source = (sourceY * BOARD_WIDTH + sourceX) * 4;
                        const pixel = y * CELL_SIZE + x;
                        tensor[cellOffset + pixel] = rgba[source] / 255;
                        tensor[cellOffset + CELL_SIZE * CELL_SIZE + pixel] = rgba[source + 1] / 255;
                        tensor[cellOffset + CELL_SIZE * CELL_SIZE * 2 + pixel] = rgba[source + 2] / 255;
                    }
                }
            }
        }
        return tensor;
    }

    function createBoardTensor(ortApi, sourceCanvas, boardRect) {
        if (!ortApi || typeof ortApi.Tensor !== 'function') throw new Error('ONNX Runtime is not available.');
        if (!sourceCanvas || !boardRect) throw new Error('Board source image or rectangle is missing.');
        const canvas = document.createElement('canvas');
        canvas.width = BOARD_WIDTH;
        canvas.height = BOARD_HEIGHT;
        const context = canvas.getContext('2d', { willReadFrequently: true });
        if (!context) throw new Error('Could not create the Cell CNN preprocessing canvas.');
        context.imageSmoothingEnabled = true;
        context.imageSmoothingQuality = 'low';
        context.drawImage(
            sourceCanvas,
            boardRect.x, boardRect.y, boardRect.w, boardRect.h,
            0, 0, BOARD_WIDTH, BOARD_HEIGHT,
        );
        const rgba = context.getImageData(0, 0, BOARD_WIDTH, BOARD_HEIGHT);
        const values = tensorDataFromBoardRgba(rgba);
        return new ortApi.Tensor('float32', values, [BOARD_COLUMNS * BOARD_ROWS, CHANNELS, CELL_SIZE, CELL_SIZE]);
    }

    function labelsFromLogits(logits) {
        const expectedLength = BOARD_COLUMNS * BOARD_ROWS * CLASS_NAMES.length;
        if (!logits || logits.length !== expectedLength) {
            throw new Error(`Cell CNN returned ${logits?.length ?? 0} values; expected ${expectedLength}.`);
        }
        const labels = new Array(BOARD_COLUMNS * BOARD_ROWS);
        for (let cell = 0; cell < labels.length; cell++) {
            const offset = cell * CLASS_NAMES.length;
            let bestClass = 0;
            let bestLogit = Number.NEGATIVE_INFINITY;
            for (let classIndex = 0; classIndex < CLASS_NAMES.length; classIndex++) {
                const logit = Number(logits[offset + classIndex]);
                if (logit > bestLogit) {
                    bestLogit = logit;
                    bestClass = classIndex;
                }
            }
            labels[cell] = CLASS_NAMES[bestClass];
        }
        return labels;
    }

    async function recognizeBoard(session, ortApi, sourceCanvas, boardRect) {
        if (!session?.inputNames?.length || !session?.outputNames?.length) {
            throw new Error('Cell CNN session metadata is unavailable.');
        }
        const tensor = createBoardTensor(ortApi, sourceCanvas, boardRect);
        const inputName = session.inputNames[0];
        const outputName = session.outputNames[0];
        const results = await session.run({ [inputName]: tensor }, [outputName]);
        return labelsFromLogits(results[outputName]?.data);
    }

    return Object.freeze({
        CLASS_NAMES,
        BOARD_WIDTH,
        BOARD_HEIGHT,
        CELL_SIZE,
        tensorDataFromBoardRgba,
        labelsFromLogits,
        recognizeBoard,
    });
});
