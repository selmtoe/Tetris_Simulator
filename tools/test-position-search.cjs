/* Real deployed workers and UI, in an isolated browser profile. */
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:8767/';
let checks = 0;
const equal = (actual, expected, message) => { assert.deepEqual(actual, expected, message); checks++; };
const empty = () => Array.from({length:40}, () => Array(10).fill(null));
const snapshot = (board, pieces, extra = {}) => ({ board, currentPiece: pieces[0], nextQueue: pieces.slice(1), holdPiece:null, canHold:true, ren:-1, ...extra });
function replay(initial, result) {
    let board = initial.board.map(row => [...row]);
    let current = initial.currentPiece, queue = [...initial.nextQueue], hold = initial.holdPiece, cleared = 0;
    for (const step of result.plan || []) {
        if (step.hold || step.piece !== current) { const previous = current; current = hold || queue.shift(); hold = previous; }
        equal(current, step.piece, 'route respects NEXT and HOLD');
        equal(new Set(step.cells.map(cell => `${cell.x},${cell.y}`)).size, 4, 'four unique mino cells');
        for (const {x,y} of step.cells) { equal(Boolean(board[y]?.[x]), false, 'no overlap'); board[y][x] = step.piece; }
        const remaining = board.filter(row => !row.every(Boolean));
        if (result.kind === 'ren') equal(remaining.length < 40, true, 'every REN move clears');
        cleared += 40 - remaining.length;
        while (remaining.length < 40) remaining.unshift(Array(10).fill(null));
        board = remaining; current = queue.shift();
    }
    return {board, cleared};
}
(async () => {
    const browser = await chromium.launch({headless:true});
    try {
        const page = await browser.newPage({viewport:{width:1280,height:850},serviceWorkers:'block'}), errors=[];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(base, {waitUntil:'networkidle'});
        const sim = page.frame({url:/index\.html\?.*workspace=1/});
        const query = (kind, data) => sim.evaluate(({kind,data}) => new Promise((resolve,reject) => {
            const worker = new Worker('./simulator/workers/route-search-worker.js');
            const timeout = setTimeout(() => { worker.terminate(); reject(new Error('search test timeout')); }, 30000);
            worker.onerror = event => { clearTimeout(timeout); worker.terminate(); reject(new Error(event.message)); };
            worker.onmessage = event => {
                if (event.data.type === 'progress') return;
                clearTimeout(timeout); worker.terminate(); resolve(event.data);
            };
            worker.postMessage({type:'search', requestId:1, kind, ...data});
        }), {kind,data});
        equal(await sim.evaluate(() => gameSettings.maxNext), 10, 'ten NEXT default');
        equal(await sim.evaluate(() => gameSettings.layout.p1.next.length), 10, 'ten NEXT positions');
        const pc = snapshot(empty(), [...'TILJSZOTIOJ']);
        const pcResult = await query('pc', pc);
        equal(pcResult.depth, 10, 'four-line PC requires ten placements');
        equal(pcResult.lines, 4, 'four-line PC target');
        equal(pcResult.status, 'found', 'PC route found');
        equal(replay(pc, pcResult).board.every(row => row.every(cell => !cell)), true, 'PC is actually empty');
        const well = empty();
        for (let y=34;y<40;y++) well[y] = Array.from({length:10}, (_,x) => x<2 ? null : 'G');
        const ren = snapshot(well, [...'OOOOOOOOOOO']);
        const renResult = await query('ren', ren);
        equal(renResult.complete, true, 'REN maximum certified');
        equal(renResult.depth, 3, 'six-row two-wide well supports three O clears');
        equal(renResult.ren, 2, 'first clear is zero REN');
        replay(ren, renResult);
        const held = snapshot(well, [...'IOOO'], {holdPiece:'O'});
        const heldResult = await query('ren', held);
        equal(heldResult.depth, 3, 'hold finds the full route');
        equal(heldResult.plan[0].hold, true, 'hold used for first O'); replay(held, heldResult);
        const locked = await query('ren', {...held, canHold:false});
        equal(locked.depth, 0, 'locked HOLD cannot supply first O');
        const noRen = await query('ren', snapshot(empty(), [...'IOTSZJL']));
        equal(noRen.status, 'not_found', 'no fabricated clearing route on empty board');
        const ai = snapshot(empty(), [...'TILJSZOTIOJ']);
        const aiResult = await query('ai', ai);
        equal(aiResult.status, 'found', 'real Cold Clear WASM route'); replay(ai, aiResult);
        await sim.evaluate(() => {
            window.searchReplies = [];
            const NativeWorker = window.Worker;
            window.Worker = class extends NativeWorker {
                constructor(...args) {
                    super(...args);
                    if (String(args[0]).includes('route-search-worker')) this.addEventListener('message', event => {
                        if (event.data.type !== 'progress') window.searchReplies.push(event.data);
                    });
                }
            };
        });
        await sim.locator('#startGameBtn').click();
        await sim.waitForFunction(() => gameState === 'PLAYING');
        equal(await sim.evaluate(() => players[0].nextQueue.length), 10, 'ten NEXT supplied in play');
        await sim.locator('#searchMenuBtn').hover();
        equal(await sim.locator('#sim-search-menu').getAttribute('class'), 'search-menu is-open', 'hover expands search');
        await sim.locator('#aiSearchBtn').click();
        await sim.waitForFunction(() => Boolean(players[0].pcGuide), {timeout:15000});
        equal(await sim.locator('#search-result').count(), 0, 'search has no text notification');
        equal(await sim.locator('#searchMenuBtn').textContent(), '探索', 'search button keeps its label');
        // Execute the guide through the real Player lock/spawn hooks. AI ends
        // on a non-empty field; completion must not use the old PC-only check.
        await sim.evaluate(() => { gameSettings.lineClearDelay=0; gameSettings.spawnDelay=0; });
        const guideDepth = await sim.evaluate(() => searchReplies.at(-1).plan.length);
        for (let index=0; index<guideDepth; index++) {
            const hasGuide = await sim.evaluate(() => {
                const player=players[0], guide=player.pcGuide;
                if (!guide) return false;
                if (player.player.pieceType !== guide.pieceType) player.hold();
                const key = cells => cells.map(cell => `${cell.x},${cell.y}`).sort().join('|');
                for (let rotation=0;rotation<4;rotation++) {
                    const shape=player.getShape(guide.pieceType,rotation);
                    for (const target of guide.cells) {
                        const x=target.x-shape[0][0], y=target.y-shape[0][1];
                        if (key(shape.map(([dx,dy]) => ({x:x+dx,y:y+dy}))) !== key(guide.cells)) continue;
                        player.player.x=x; player.player.y=y; player.player.rotation=rotation;
                        player.hardDrop(); return true;
                    }
                }
                throw new Error('Guide geometry cannot be played');
            });
            equal(hasGuide, true, 'AI guide continues to every planned placement');
            await sim.waitForFunction(() => !players[0].isSpawning && !players[0].isClearingLine);
        }
        equal(await sim.evaluate(() => players[0].pcGuide), null, 'AI guide finishes after the last placement');
        equal(await sim.locator('#search-result').count(), 0, 'completion adds no notification');
        await sim.locator('#backToEditorBtn').click();
        await sim.evaluate(() => {
            editorData.p1.board=Array.from({length:40},(_,y)=>Array.from({length:10},(_,x)=>y>=20&&x<6?'G':null));
            editorData.p1.board[39][6]=editorData.p1.board[39][7]=editorData.p1.board[39][8]='G';
            editorData.p1.nextQueue=[...'TILJSZOTILJSZOTILJSZOTILJSZO'];
        });
        await sim.locator('#startGameBtn').click();
        await sim.locator('#searchMenuBtn').click();
        await sim.locator('#renSearchBtn').click();
        await sim.waitForFunction(() => players[0].pcGuide && document.getElementById('searchMenuBtn').getAttribute('aria-busy') === 'false');
        equal(await sim.evaluate(() => players[0].nextQueue.length),10,'long-queue REN retains ten visible NEXT');
        equal(await sim.evaluate(() => [searchReplies.at(-1).ren, searchReplies.at(-1).depth]),[19,20],'REN reads beyond ten visible previews without generating random pieces');

        // Two-player games still search P1, independently of P2's moves.
        await sim.locator('#backToEditorBtn').click();
        await sim.locator('#mode-2p').click();
        await sim.locator('#p2-ai-toggle').uncheck();
        await sim.evaluate(() => {
            editorData.p1.board = Array.from({length:40},()=>Array(10).fill(null));
            editorData.p1.nextQueue = [...'TILJSZOTIOJ'];
            gameSettings.drawMoveDelay = 0;
        });
        await sim.locator('#startGameBtn').click();
        equal(await sim.evaluate(() => gameMode), '2P', 'real two-player game is running');
        await sim.locator('#searchMenuBtn').click();
        await sim.locator('#pcSearchBtn').click();
        await sim.waitForFunction(() => Boolean(players[0].pcGuide));
        equal(await sim.evaluate(() => searchReplies.at(-1).kind), 'pc', 'PC search works in 2P');
        equal(await sim.evaluate(() => players[1].pcGuide), null, 'P2 gets no guide');
        const p1Guide = await sim.evaluate(() => JSON.stringify(players[0].pcGuide));
        await sim.evaluate(() => players[1].hardDrop());
        equal(await sim.evaluate(() => JSON.stringify(players[0].pcGuide)), p1Guide, 'P2 lock leaves P1 guide intact');
        await sim.locator('#searchMenuBtn').click();
        await sim.locator('#aiSearchBtn').click();
        await sim.waitForFunction(() => players[0].pcGuide && searchReplies.at(-1).kind === 'ai');
        for (const debug of [false, true]) {
            const draw = await sim.evaluate(async debug => {
                gameSettings.debugEnabled = debug;
                const player = players[0], guide = player.pcGuide;
                const before = JSON.stringify(player.board);
                const overlay = document.getElementById('ai-tree-debug-display');
                overlay.style.display = 'none'; overlay.textContent = '';
                player.drawnBlocks = new Map(guide.cells.map(({x,y}) => [`${x},${y}`, true]));
                await player.processDrawing();
                return {moved: before !== JSON.stringify(player.board), visible: getComputedStyle(overlay).display !== 'none', text: overlay.textContent};
            }, debug);
            equal(draw.moved, true, 'Draw executes the AI suggestion during 2P');
            equal(draw.visible, debug, 'Draw path is visible only in debug mode');
            equal(draw.text.includes('Draw Path:'), debug, 'normal Draw never writes diagnostic text');
        }
        await sim.locator('#backToEditorBtn').click();
        await sim.evaluate(board => {
            gameSettings.debugEnabled = false;
            editorData.p1.board = board;
            editorData.p1.nextQueue = [...'OOOOOOOOOOO'];
        }, well);
        await sim.locator('#startGameBtn').click();
        await sim.locator('#searchMenuBtn').click();
        await sim.locator('#renSearchBtn').click();
        await sim.waitForFunction(() => players[0].pcGuide && searchReplies.at(-1).kind === 'ren');
        equal(await sim.evaluate(() => searchReplies.at(-1).ren), 2, 'REN search works in 2P');
        equal(await sim.evaluate(() => players[1].pcGuide), null, 'REN only guides P1');
        equal(await sim.locator('#search-result').count(), 0, '2P search adds no notification');
        for (const width of [1280,390]) {
            const context=await browser.newContext({viewport:{width,height:844},hasTouch:true,serviceWorkers:'block'});
            const viewerPage=await context.newPage(); viewerPage.on('pageerror', error=>errors.push(error.message));
            const fixture={v:3,m:'1P',cases:[{name:'PC search fixture',kind:'snapshot',gameMode:'1P',pages:[{p1:{board:empty(),next:'TILJSZOTIOJ',hold:''},p2:{board:empty(),next:'',hold:''}}]}]};
            const url=new URL('F/index.html',base); url.hash=Buffer.from(JSON.stringify(fixture)).toString('base64');
            await viewerPage.goto(url.href,{waitUntil:'networkidle'});
            await viewerPage.waitForFunction(()=>document.body.dataset.workspaceReady==='true');
            const view=viewerPage.frame({url:/\/F\/index\.html\?workspace/});
            const original=await view.evaluate(()=>JSON.stringify(getCollectionDataForExport()));
            if (await view.locator('#viewer-controls').getAttribute('data-presentation')==='compact') await view.locator('#viewer-page-indicator').tap();
            await view.locator('#viewer-analysis-btn').tap();
            await view.locator('#viewer-pc-search-btn').tap();
            await view.waitForFunction(()=>document.querySelector('.route-status').textContent.includes('4ラインPC'));
            equal(await view.locator('#route-dialog').evaluate(element=>element.open),true,'viewer PC route opens');
            equal(await view.locator('#route-dialog input').getAttribute('max'),'10','viewer displays all ten PC steps');
            equal(await view.locator('[data-route-stop]').isVisible(),false,'completed query hides stop');
            await view.locator('#route-dialog input').fill('10');
            equal(await view.locator('.route-position').textContent(),'完了','route has final board');
            await view.locator('[data-route-close]').tap();
            equal(await view.evaluate(()=>JSON.stringify(getCollectionDataForExport())),original,'preview leaves replay unchanged');
            if (await view.locator('#viewer-controls').getAttribute('data-presentation')==='compact') {
                const expanded=await view.locator('#viewer-page-indicator').getAttribute('aria-expanded');
                if(expanded!=='true') await view.locator('#viewer-page-indicator').tap();
            }
            await view.locator('#viewer-analysis-btn').tap();
            await view.locator('#viewer-ai-score-btn').tap();
            equal(await view.locator('#ai-score-title').textContent(),'AI分析 （P1のみ）','existing AI scoring opens from Analysis');
            await view.locator('#ai-score-close').tap();
            await view.evaluate(() => {
                const board=Array.from({length:40},(_,y)=>Array.from({length:10},(_,x)=>y>=20&&x<6?'G':null));
                board[39][6]=board[39][7]=board[39][8]='G';
                return TetrisWorkspace.import({v:3,m:'1P',cases:[{name:'REN',kind:'snapshot',gameMode:'1P',pages:[{p1:{board,next:'TILJSZOTILJSZOTILJSZOTILJSZO',hold:''},p2:{board:Array.from({length:40},()=>Array(10).fill(null)),next:'',hold:''}}]}]});
            });
            if (await view.locator('#viewer-page-indicator').getAttribute('aria-expanded')!=='true') await view.locator('#viewer-page-indicator').tap();
            await view.locator('#viewer-analysis-btn').tap(); await view.locator('#viewer-ren-search-btn').tap();
            await view.waitForFunction(()=>document.querySelector('.route-status').textContent.includes('最大 19 REN'));
            equal(await view.locator('#route-dialog input').getAttribute('max'),'20','viewer REN shows the full known route');
            await view.locator('#route-dialog input').fill('10');
            await view.locator('[data-route-practice]').tap();
            await viewerPage.waitForFunction(()=>document.body.dataset.paneView==='both');
            equal(await viewerPage.locator('body').getAttribute('data-pane-view'),'both','route practice preserves split workflow');
            const practiceFrame=viewerPage.frame({url:/index\.html\?.*workspace=1/});
            equal(await practiceFrame.evaluate(()=>editorData.p1.board.flat().filter(Boolean).length),63,'practice receives the board after ten clears');
            await context.close();
        }
        const two=await browser.newPage({viewport:{width:1280,height:850},serviceWorkers:'block'});
        two.on('pageerror',error=>errors.push(error.message));
        const twoFixture={v:3,m:'2P',cases:[{name:'2P switching',kind:'snapshot',gameMode:'2P',pages:[{p1:{board:empty(),next:'I',hold:''},p2:{board:well,next:'OOO',hold:''}}]}]};
        const twoUrl=new URL('F/index.html',base); twoUrl.hash=Buffer.from(JSON.stringify(twoFixture)).toString('base64');
        await two.goto(twoUrl.href,{waitUntil:'networkidle'});
        const twoView=two.frame({url:/\/F\/index\.html\?workspace/});
        if(await twoView.locator('#viewer-page-indicator').getAttribute('aria-expanded')!=='true')await twoView.locator('#viewer-page-indicator').click();
        await twoView.locator('#viewer-analysis-btn').click(); await twoView.locator('#viewer-ren-search-btn').click();
        for(let iteration=0;iteration<8;iteration++) {
            await twoView.locator('#route-dialog select').selectOption('p1');
            await twoView.locator('#route-dialog select').selectOption('p2');
            await twoView.waitForFunction(()=>document.querySelector('.route-status').textContent.includes('最大 2 REN'));
            equal(await twoView.locator('#route-dialog input').getAttribute('max'),'3','superseded loading errors never cancel the new query');
        }
        if (process.env.SEARCH_TEST_SCREENSHOT) await two.screenshot({path:process.env.SEARCH_TEST_SCREENSHOT});
        await two.close();
        equal(errors, [], 'no browser errors');
        console.log(JSON.stringify({passed:true,checks}));
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode=1; });
