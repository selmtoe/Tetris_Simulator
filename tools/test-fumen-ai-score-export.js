/* Run with Node.js: node tools/test-fumen-ai-score-export.js */

'use strict';

const fs = require('fs');
const vm = require('vm');

const assert = (condition, message) => {
    if (!condition) throw new Error(message);
};
const blankBoard = () => Array.from({ length: 40 }, () => Array(10).fill(null));
const boardToString = board => board.map(row => row.map(cell => cell == null ? '_' : cell).join('')).join('');

const context = {
    BOARD_WIDTH: 10,
    BOARD_HEIGHT: 40,
    gameMode: '1P',
    fumenPages: [],
    boardToString,
    console,
    TextEncoder,
    TextDecoder,
    Uint8Array,
    btoa: value => Buffer.from(value, 'binary').toString('base64'),
    atob: value => Buffer.from(value, 'base64').toString('binary')
};
vm.createContext(context);
vm.runInContext(fs.readFileSync('F/app/40-history.js', 'utf8'), context, { filename: '40-history.js' });
vm.runInContext(fs.readFileSync('F/app/60-fumen-codec.js', 'utf8'), context, { filename: '60-fumen-codec.js' });
vm.runInContext('globalThis.__fumenCodecForTest = FumenCodec;', context, { filename: 'capture-fumen-codec.js' });

const p1a = blankBoard();
const p2a = blankBoard();
const p1b = blankBoard();
const p2b = blankBoard();
p1b[39][4] = 'T';
p2a[39][0] = 'G';
p2b[38][9] = 'I';
const pages = [
    { p1: { board: p1a, hold: '', next: 'TIOLJSZ' }, p2: { board: p2a, hold: 'O', next: 'LSZ' } },
    { p1: { board: p1b, hold: 'T', next: 'IOLJSZ' }, p2: { board: p2b, hold: 'O', next: 'SZJ' } }
];

const editorData = context.getFumenDataForExport(pages, '2P');
assert(editorData.v === 'f2' && editorData.m === '2P', 'Editor export did not preserve its f2/2P metadata.');
assert(editorData.p.length === 2 && editorData.p.every(page => page.p1 && page.p2), 'Editor export dropped P2 pages.');
assert(editorData.p[1].p2.n === 'SZJ' && editorData.p[1].p2.h === 'O', 'Editor export dropped P2 next/hold data.');

const mobileCode = context.__fumenCodecForTest.export(pages, 'p1');
assert(mobileCode.startsWith('v115@'), 'Mobile Fumen export did not return a v115 code.');

console.log(JSON.stringify({
    passed: true,
    editorPages: editorData.p.length,
    p2Included: editorData.p.every(page => Boolean(page.p2)),
    mobilePrefix: mobileCode.slice(0, 5)
}, null, 2));
