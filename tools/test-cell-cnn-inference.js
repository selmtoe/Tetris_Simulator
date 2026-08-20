const assert = require('node:assert/strict');
const path = require('node:path');

const cnn = require(path.join(__dirname, '..', 'shared', 'cell-cnn-inference.js'));

const rgba = new Uint8ClampedArray(cnn.BOARD_WIDTH * cnn.BOARD_HEIGHT * 4);
rgba[0] = 255;
rgba[(31 * cnn.BOARD_WIDTH + 31) * 4 + 1] = 255;
rgba[(0 * cnn.BOARD_WIDTH + 32) * 4 + 2] = 255;
const tensor = cnn.tensorDataFromBoardRgba({ width: cnn.BOARD_WIDTH, height: cnn.BOARD_HEIGHT, data: rgba });

assert.equal(tensor.length, 200 * 3 * 32 * 32);
assert.equal(tensor[0], 1, 'cell 0 red channel is not NCHW-packed');
assert.equal(tensor[32 * 32 + 31 * 32 + 31], 1, 'cell 0 green channel is not NCHW-packed');
assert.equal(tensor[3 * 32 * 32 + 2 * 32 * 32], 1, 'cell 1 blue channel crossed a cell boundary');

const logits = new Float32Array(200 * cnn.CLASS_NAMES.length).fill(-10);
for (let cell = 0; cell < 200; cell++) logits[cell * cnn.CLASS_NAMES.length + (cell % cnn.CLASS_NAMES.length)] = 5;
const labels = cnn.labelsFromLogits(logits);
assert.equal(labels.length, 200);
for (let cell = 0; cell < labels.length; cell++) assert.equal(labels[cell], cnn.CLASS_NAMES[cell % cnn.CLASS_NAMES.length]);

console.log(JSON.stringify({ tensorValues: tensor.length, labels: labels.length, classes: cnn.CLASS_NAMES }));
