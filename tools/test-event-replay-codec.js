'use strict';

const assert = require('assert');
const fs = require('fs');
const zlib = require('zlib');
const { performance } = require('perf_hooks');
const codec = require('../shared/tetris-event-codec.js');

const empty = () => Array.from({ length: 40 }, () => Array(10).fill(null));
const pagePlayer = (board, active, next, operation, hold = '') => ({
    board: codec.boardMatrix(board), active, hold, next, operation,
    activeColor: 'I', viewY: 20, nextInsertionIndex: -1,
    placementMode: false, placementDraft: []
});
const operation = (type, rotation, x, y, holdUsed = false) => ({
    type, rotation, x, y, holdUsed, coordinateSpace: 'simulator', lock: true
});

const blank = codec.boardString(empty());
const p1Lock = operation('I', 'spawn', 3, 39);
const p2Lock = operation('O', 'spawn', 4, 39);
const p1AfterLock = codec.applyOperation(blank, p1Lock);
const p1AfterGarbage = codec.applyGarbage(p1AfterLock, ['27', 'G']);
const p2AfterLock = codec.applyOperation(blank, p2Lock);
const corrected = p2AfterLock.split('');
corrected[39 * 10] = 'T';

const collection = {
    v: 3,
    m: '2P',
    currentCase: 0,
    cases: [{
        id: 'event-codec-test', name: 'Event codec test', kind: 'replay', gameMode: '2P',
        initial: {
            p1: { board: empty(), hold: '', sequence: 'ITZ' },
            p2: { board: empty(), hold: '', sequence: 'OJS' }
        },
        pages: [
            { time: 0, p1: pagePlayer(blank, 'I', 'TZ', p1Lock), p2: pagePlayer(blank, 'O', 'JS', p2Lock) },
            { time: .1, p1: pagePlayer(p1AfterGarbage, 'T', 'Z', operation('T', 'spawn', 4, 36)), p2: pagePlayer(blank, 'O', 'JS', p2Lock) },
            { time: .2, p1: pagePlayer(p1AfterGarbage, 'T', 'Z', operation('T', 'spawn', 4, 36)), p2: pagePlayer(corrected.join(''), 'J', 'S', operation('J', 'right', 4, 37)) }
        ]
    }]
};

const encoded = codec.encodeCollection(collection, { checkpointInterval: 64 });
assert(codec.isEventReplay(encoded));
assert.strictEqual(encoded.cases[0].e.length, collection.cases[0].pages.length);
assert.deepStrictEqual(encoded.cases[0].e[1][1].g, ['27', 'G'], 'garbage should be an event, not a board snapshot');
assert(!Object.prototype.hasOwnProperty.call(encoded.cases[0].e[1][1], 'b'));
assert(Object.prototype.hasOwnProperty.call(encoded.cases[0].e[2][2], 'b'), 'unexplained correction needs an exact checkpoint');

const decoded = codec.decodeCollection(encoded);
const compactDecoded = codec.decodeCollection(encoded, { compactBoards: true });
assert.strictEqual(typeof compactDecoded.cases[0].pages[0].p1.board, 'string');
assert.strictEqual(compactDecoded.cases[0].pages[0].p1.board, blank);
assert.strictEqual(typeof compactDecoded.cases[0].initial.p1.board, 'string');
const sourcePages = collection.cases[0].pages;
const decodedPages = decoded.cases[0].pages;
assert.strictEqual(decodedPages.length, sourcePages.length);
for (let index = 0; index < sourcePages.length; index++) {
    for (const playerId of ['p1', 'p2']) {
        assert.strictEqual(codec.boardString(decodedPages[index][playerId].board),
            codec.boardString(sourcePages[index][playerId].board), `${playerId} board ${index}`);
        assert.deepStrictEqual(codec.operationArray(decodedPages[index][playerId].operation),
            codec.operationArray(sourcePages[index][playerId].operation), `${playerId} operation ${index}`);
        assert.strictEqual(decodedPages[index][playerId].active, sourcePages[index][playerId].active);
        assert.strictEqual(decodedPages[index][playerId].hold, sourcePages[index][playerId].hold);
        assert.strictEqual(decodedPages[index][playerId].next, sourcePages[index][playerId].next);
    }
}

const checkpointed = codec.encodeCollection(collection, { checkpointInterval: 2 });
assert(Object.prototype.hasOwnProperty.call(checkpointed.cases[0].e[1][1], 'b'), 'periodic checkpoint missing');

if (process.argv[2]) {
    const recovery = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
    const source = recovery.simulatorData || recovery.eventReplay || recovery;
    const sourceCollection = codec.isEventReplay(source) ? codec.decodeCollection(source) : source;
    const encodeStarted = performance.now();
    const measured = codec.encodeCollection(sourceCollection);
    const encodeMilliseconds = performance.now() - encodeStarted;
    const decodeStarted = performance.now();
    const roundTrip = codec.decodeCollection(measured);
    const compactRoundTrip = codec.decodeCollection(measured, { compactBoards: true });
    const decodeMilliseconds = performance.now() - decodeStarted;
    const sourceJsonBytes = Buffer.byteLength(JSON.stringify(source));
    const eventText = JSON.stringify(measured);
    const eventJsonBytes = Buffer.byteLength(eventText);
    const gzipBytes = zlib.gzipSync(eventText).length;
    const deltas = measured.cases.flatMap(caseData => caseData.e.flatMap(event => event.slice(1)))
        .filter(value => value && typeof value === 'object');
    assert.strictEqual(roundTrip.cases[0].pages.length, sourceCollection.cases[0].pages.length);
    assert.strictEqual(compactRoundTrip.cases[0].pages.length, sourceCollection.cases[0].pages.length);
    for (let index = 0; index < sourceCollection.cases[0].pages.length; index++) {
        for (const playerId of sourceCollection.cases[0].gameMode === '2P' ? ['p1', 'p2'] : ['p1']) {
            const actual = roundTrip.cases[0].pages[index][playerId];
            const expected = sourceCollection.cases[0].pages[index][playerId];
            assert.strictEqual(codec.boardString(actual.board), codec.boardString(expected.board));
            assert.deepStrictEqual(codec.operationArray(actual.operation || actual.o),
                codec.operationArray(expected.operation || expected.o));
            assert.strictEqual(actual.hold || actual.h || '', expected.hold || expected.h || '');
            assert.strictEqual(actual.next || actual.n || '', expected.next || expected.n || '');
            assert.strictEqual(actual.active || actual.a || '', expected.active || expected.a || '');
            assert.strictEqual(typeof compactRoundTrip.cases[0].pages[index][playerId].board, 'string');
            assert.strictEqual(compactRoundTrip.cases[0].pages[index][playerId].board,
                codec.boardString(expected.board));
        }
    }
    console.log(JSON.stringify({
        sourceJsonBytes,
        eventJsonBytes,
        gzipBytes,
        estimatedGzipHashCharacters: Math.ceil(gzipBytes / 3) * 4 + 3,
        ratio: eventJsonBytes / sourceJsonBytes,
        encodeMilliseconds,
        decodeMilliseconds,
        events: measured.cases.reduce((sum, caseData) => sum + caseData.e.length, 0),
        placements: deltas.filter(delta => Array.isArray(delta.o)).length,
        garbageRises: deltas.filter(delta => Array.isArray(delta.g)).length,
        exactBoards: deltas.filter(delta => Object.prototype.hasOwnProperty.call(delta, 'b')).length
    }));
}

console.log('event replay codec test passed');
