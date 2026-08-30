/* Run with Node.js: node tools/test-ai-no-legal-move.js */

'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { performance } = require('perf_hooks');

const assert = (condition, message) => {
    if (!condition) throw new Error(message);
};

const root = path.join(__dirname, '..');

async function runWorker(file, bridgeName, bridge, requestId) {
    const messages = [];
    const self = {
        postMessage: message => messages.push(message),
        location: { href: 'https://example.test/simulator/workers/test-worker.js' }
    };
    const context = vm.createContext({
        console,
        performance,
        setTimeout,
        clearTimeout,
        importScripts: () => {},
        self,
        [bridgeName]: { load: () => Promise.resolve(bridge) }
    });
    const source = fs.readFileSync(path.join(root, file), 'utf8');
    vm.runInContext(source, context, { filename: file });
    self.onmessage({
        data: {
            type: 'analyze',
            requestId,
            decisionLatencyMs: 0,
            thinkTimeMs: 1,
            nodeLimit: 1,
            incoming: 0,
            snapshot: {}
        }
    });

    for (let attempt = 0; attempt < 20 && !messages.some(message => message.type === 'noLegalMove'); attempt++) {
        await new Promise(resolve => setImmediate(resolve));
    }
    return messages;
}

function makePlayer(Player, requestId = 17) {
    const messages = [];
    const opponent = { gameClear: false };
    const player = Object.create(Player.prototype);
    Object.assign(player, {
        isAi: true,
        isAiThinking: true,
        aiRequestId: requestId,
        aiSearchInitialized: true,
        aiWorker: { postMessage: message => messages.push(message) },
        aiModel: 'kasane-guard',
        gameOver: false,
        gameClear: false,
        opponent,
        board: Array.from({ length: 40 }, () => Array(10).fill(null)),
        player: { pieceType: 'T', x: 3, y: 20, rotation: 0 },
        canHold: true
    });
    return { player, opponent, messages };
}

(async () => {
    const kasaneMessages = await runWorker(
        'simulator/workers/kasane-wasm-worker.js',
        'KasaneWasmBridge',
        { choose: () => null },
        31
    );
    const kasaneTerminal = kasaneMessages.find(message => message.type === 'noLegalMove');
    assert(kasaneTerminal && kasaneTerminal.requestId === 31,
        'KASANE did not return a typed noLegalMove response for the current request.');
    assert(!kasaneMessages.some(message => message.type === 'move'),
        'KASANE represented a terminal result as an empty move.');

    const coldClearMessages = await runWorker(
        'simulator/workers/cold-clear-wasm-worker.js',
        'ColdClearWasmBridge',
        {
            create: () => 1,
            destroy: () => {},
            think: () => ({ nodesAdded: 0 }),
            suggest: () => null,
            nodeCount: () => 0
        },
        47
    );
    const coldClearTerminal = coldClearMessages.find(message => message.type === 'noLegalMove');
    assert(coldClearTerminal && coldClearTerminal.requestId === 47,
        'Cold Clear did not return a typed noLegalMove response for the current request.');
    assert(!coldClearMessages.some(message => message.type === 'move'),
        'Cold Clear represented a terminal result as an empty move.');

    const playerContext = vm.createContext({
        console,
        performance,
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
    const playerSource = fs.readFileSync(path.join(root, 'simulator/app/player-engine.js'), 'utf8');
    vm.runInContext(`${playerSource}\nglobalThis.PlayerUnderTest = Player;`, playerContext);
    const Player = playerContext.PlayerUnderTest;

    const terminal = makePlayer(Player);
    const accepted = terminal.player.handleAiNoLegalMove({ type: 'noLegalMove', requestId: 17 });
    assert(accepted, 'The current terminal response was not accepted.');
    assert(terminal.player.gameOver && terminal.opponent.gameClear,
        'A current noLegalMove response did not finish the match as a loss.');
    assert(!terminal.player.isAiThinking && !terminal.player.aiSearchInitialized,
        'Terminal handling retained active AI search state.');
    assert(terminal.messages.some(message => message.type === 'pause'),
        'Terminal handling did not pause the AI worker.');

    const oldResponse = makePlayer(Player);
    const ignored = oldResponse.player.handleAiNoLegalMove({ type: 'noLegalMove', requestId: 16 });
    assert(!ignored && !oldResponse.player.gameOver && !oldResponse.opponent.gameClear,
        'A stale noLegalMove response incorrectly ended the match.');
    assert(oldResponse.player.isAiThinking && oldResponse.player.aiSearchInitialized,
        'A stale terminal response invalidated the current search.');

    const unreachable = makePlayer(Player);
    unreachable.player.findShortestPath_forAI = () => null;
    unreachable.player.lockPiece = () => { throw new Error('Unreachable placement was locked.'); };
    await unreachable.player.executeAiMove({ requestId: 17, piece: 'T', x: 7, y: 35, rotation: 0 });
    assert(!unreachable.player.gameOver && !unreachable.opponent.gameClear,
        'An unreachable placement was treated as a terminal no-legal-move result.');
    assert(unreachable.messages.some(message => message.type === 'reset'),
        'An unreachable placement did not request a recoverable re-search.');

    const staleTarget = makePlayer(Player);
    staleTarget.player.findShortestPath_forAI = () => [];
    staleTarget.player.getShape = () => [[0, 0]];
    staleTarget.player.checkCollision = () => true;
    staleTarget.player.lockPiece = () => { throw new Error('Stale placement was locked.'); };
    await staleTarget.player.executeAiMove({ requestId: 17, piece: 'T', x: 7, y: 35, rotation: 0 });
    assert(!staleTarget.player.gameOver && !staleTarget.opponent.gameClear,
        'A stale placement was treated as a terminal no-legal-move result.');
    assert(staleTarget.messages.some(message => message.type === 'reset'),
        'A stale placement did not request a recoverable re-search.');

    assert(/type === 'noLegalMove'[\s\S]{0,120}handleAiNoLegalMove/.test(playerSource),
        'The Player worker-message dispatcher is not wired to terminal handling.');

    console.log(JSON.stringify({
        passed: true,
        workers: ['KASANE', 'Cold Clear'],
        cases: 6
    }, null, 2));
})().catch(error => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
