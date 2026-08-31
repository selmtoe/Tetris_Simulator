/* Run with Node.js: node tools/test-ai-garbage-replan.js */

'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const assert = (condition, message) => {
    if (!condition) throw new Error(message);
};

const context = vm.createContext({
    console,
    setTimeout,
    clearTimeout,
    BOARD_WIDTH: 10,
    BOARD_HEIGHT: 40,
    gameState: 'PLAYING',
    gameSettings: {
        aiMoveDelay: 0,
        aiSdfDelay: 0,
        debugEnabled: false,
        garbageRandomness: 0
    },
    document: { getElementById: () => null }
});

const source = fs.readFileSync(path.join(__dirname, '../simulator/app/player-engine.js'), 'utf8');
vm.runInContext(`${source}\nglobalThis.PlayerUnderTest = Player;`, context);
const Player = context.PlayerUnderTest;
const emptyBoard = () => Array.from({ length: 40 }, () => Array(10).fill(null));

function makeAi(aiModel = 'cold-clear') {
    const messages = [];
    const player = Object.create(Player.prototype);
    Object.assign(player, {
        isAi: true,
        isAiThinking: true,
        aiRequestId: 11,
        aiSearchInitialized: true,
        aiWorker: { postMessage: message => messages.push(message) },
        aiModel,
        gameOver: false,
        board: emptyBoard(),
        player: { pieceType: 'T', x: 3, y: 20, rotation: 0 },
        canHold: true,
        lastGarbageHoleX: 4
    });
    return { player, messages };
}

(async () => {
    const risen = makeAi();
    risen.player.pendingGarbage = 2;
    risen.player.riseGarbage();

    assert(risen.player.pendingGarbage === 0, 'Applied garbage was not consumed.');
    assert(risen.player.aiRequestId === 12, 'Garbage rise did not invalidate the current AI request.');
    assert(!risen.player.isAiThinking && !risen.player.aiSearchInitialized, 'Garbage rise retained stale AI state.');
    assert(risen.messages.some(message => message.type === 'reset'), 'Garbage rise did not reset the Cold Clear worker.');
    assert(risen.player.board.slice(-2).every(row => row[4] === null && row.filter(Boolean).length === 9), 'Garbage rows were not applied as expected.');

    const kasaneRisen = makeAi('kasane-strategy');
    kasaneRisen.player.pendingGarbage = 2;
    kasaneRisen.player.riseGarbage();

    assert(kasaneRisen.player.aiRequestId === 12, 'KASANE garbage rise did not invalidate the current AI request.');
    assert(kasaneRisen.messages.some(message => message.type === 'invalidate'), 'KASANE garbage rise did not preserve strategy memory while invalidating search.');
    assert(!kasaneRisen.messages.some(message => message.type === 'reset'), 'KASANE garbage rise reset strategy memory instead of only invalidating search.');

    const unreachable = makeAi();
    unreachable.player.findShortestPath_forAI = () => null;
    unreachable.player.getShape = () => [[0, 0]];
    unreachable.player.lockPiece = () => { throw new Error('Unreachable placement was locked.'); };
    await unreachable.player.executeAiMove({ requestId: 11, piece: 'T', x: 7, y: 35, rotation: 0 });

    assert(unreachable.player.player.x === 3 && unreachable.player.player.y === 20, 'Unreachable placement teleported the active piece.');
    assert(!unreachable.messages.some(message => message.type === 'commit'), 'Unreachable placement was committed to Cold Clear.');
    assert(unreachable.messages.some(message => message.type === 'reset'), 'Unreachable placement did not request a fresh search.');

    const staleTarget = makeAi();
    staleTarget.player.findShortestPath_forAI = () => [];
    staleTarget.player.getShape = () => [[0, 0]];
    staleTarget.player.board[35][7] = 'G';
    staleTarget.player.lockPiece = () => { throw new Error('Occupied placement was locked.'); };
    await staleTarget.player.executeAiMove({ requestId: 11, piece: 'T', x: 7, y: 35, rotation: 0 });

    assert(staleTarget.player.player.x === 3 && staleTarget.player.player.y === 20, 'Occupied placement teleported the active piece.');
    assert(!staleTarget.messages.some(message => message.type === 'commit'), 'Occupied placement was committed to Cold Clear.');

    const valid = makeAi();
    let locks = 0;
    valid.player.findShortestPath_forAI = () => [];
    valid.player.getShape = () => [[0, 0]];
    valid.player.lockPiece = () => { locks++; };
    await valid.player.executeAiMove({ requestId: 11, piece: 'T', x: 7, y: 35, rotation: 0 });

    assert(valid.player.player.x === 7 && valid.player.player.y === 35, 'Valid placement was not applied.');
    assert(valid.messages.some(message => message.type === 'commit'), 'Valid placement was not committed to Cold Clear.');
    assert(locks === 1, 'Valid placement was not locked exactly once.');

    console.log(JSON.stringify({ passed: true, cases: 5 }, null, 2));
})().catch(error => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
