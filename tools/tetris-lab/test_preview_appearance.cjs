'use strict';
const assert = require('node:assert/strict');
const { editorFit, drawMeter, drawPlacement, palettes } = require('./preview-appearance.js');

// Whichever viewport dimension is limiting, the complete card fits at one scale.
for (const [vw, vh, width, height] of [
    [1260, 700, 1160, 956], [1260, 700, 720, 880],
    [679, 711, 720, 1040], [370, 824, 720, 1040],
    [680, 380, 1160, 956], [1900, 1060, 1160, 1200],
]) {
    const scale = editorFit(vw, vh, width, height);
    assert(scale > 0);
    assert(width * scale <= vw + 1e-8);
    assert(height * scale <= vh + 1e-8);
    assert(Math.abs(width * scale - vw) < 1e-8 || Math.abs(height * scale - vh) < 1e-8);
    assert(Math.abs((width * scale) / (height * scale) - width / height) < 1e-8);
}

function paint(ready, queued, colors) {
    const fills = [], stack = [];
    const ctx = {
        fillStyle: 'original',
        save() { stack.push(this.fillStyle); },
        restore() { this.fillStyle = stack.pop(); },
        beginPath() {}, roundRect() {}, fill() {}, clip() {},
        fillRect(x, y, w, h) { fills.push({ color: this.fillStyle, x, y, w, h }); },
    };
    drawMeter(ctx, 148, 14, 28, 20, ready, queued, colors);
    assert.equal(ctx.fillStyle, 'original', 'Painting must restore the game canvas state');
    for (const fill of fills) {
        assert(fill.y >= 14 && fill.y + fill.h <= 574, 'Meter stays within the visible board');
        assert(fill.h >= 0);
    }
    return fills;
}
for (const colors of Object.values(palettes)) {
    const mixed = paint(4, 6, colors);
    assert.equal(mixed.filter(f => f.color === colors.pending).length, 4);
    assert.equal(mixed.filter(f => f.color === colors.queued).length, 6);
    assert.equal(paint(25, 8, colors).filter(f => f.color === colors.queued).length, 0);
    assert.equal(paint(0, 99, colors).filter(f => f.color === colors.queued).length, 20);
    assert.equal(paint(0, 0, colors).filter(f => f.color !== colors.tick).length, 0);
}
console.log('Uniform viewport fitting and incoming-meter state/bounds passed.');

// The placement is a plain white light over the cell, bounded and state-safe.
for (const size of [28, 50]) {
    const fills = [], stack = [];
    const original = { fillStyle: '#123', globalAlpha: 0.5 };
    const ctx = { ...original,
        save() { stack.push({ fillStyle: this.fillStyle, globalAlpha: this.globalAlpha }); },
        restore() { Object.assign(this, stack.pop()); },
        strokeRect() { assert.fail('Placed pieces must have no decorative outline'); },
        fillRect(x, y, width, height) { fills.push({x, y, width, height, color:this.fillStyle, alpha:this.globalAlpha}); },
    };
    drawPlacement(ctx, size * 3, size * 19, size);
    assert.deepEqual(fills, [{x:size*3,y:size*19,width:size,height:size,color:'rgba(255, 255, 255, 0.38)',alpha:1}]);
    for (const [key, value] of Object.entries(original)) assert.deepEqual(ctx[key], value);
}
console.log('Placed-piece white light, bounds and canvas state passed.');
