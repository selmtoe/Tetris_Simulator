// Exercise the deployed browser Worker and its retained-tree lifecycle.
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
globalThis.self = {};
require('../simulator/workers/cold-clear-core.js');
const core = self.ColdClearSimulatorCore;
const base = process.argv[2] || 'http://127.0.0.1:8769/';
(async () => {
    const browser = await chromium.launch({ headless: true });
    try {
        const page = await browser.newPage({ serviceWorkers: 'block' });
        await page.goto(new URL('F/index.html?workspace=1', base).href);
        await page.evaluate(() => {
            window.testBot = new Worker(new URL('../simulator/workers/cold-clear-wasm-worker.js', location.href));
            window.testMessages = [];
            testBot.onmessage = event => testMessages.push(event.data);
        });
        let state = 33441;
        const random = () => { state ^= state << 13; state ^= state >>> 17; state ^= state << 5; return (state >>> 0) / 4294967296; };
        const sequence = [];
        for (let i=0; i<15; i++) {
            const bag = [...'IOTLSJZ'];
            for (let j=6; j>0; j--) { const k = Math.floor(random() * (j+1)); [bag[j], bag[k]] = [bag[k], bag[j]]; }
            sequence.push(...bag);
        }
        let board = new core.Board(), index=0, hold=null, combo=0, b2b=false, holds=0;
        for (let turn=0; turn<50; turn++) {
            const snapshot = { type:'analyze', requestId:turn, board:Array.from(board.rows, row=>Array.from({length:10},(_,x)=>row&(1<<x)?'G':null)), currentPiece:sequence[index], nextQueue:sequence.slice(index+1,index+11), holdPiece:hold, canHold:true, isB2B:b2b, ren:combo-1, nodeLimit:120000, thinkTimeMs:50, background:true, weights:{} };
            const move = await page.evaluate(data => new Promise((resolve, reject) => {
                const timer = setTimeout(() => reject(Error('Cold Clear worker timeout')), 10000);
                const listener = event => {
                    const result = event.data;
                    if (result.type === 'error' || result.type === 'noLegalMove') {
                        clearTimeout(timer); testBot.removeEventListener('message', listener); reject(Error(JSON.stringify(result)));
                    } else if (result.type === 'move' && result.requestId === data.requestId) {
                        clearTimeout(timer); testBot.removeEventListener('message', listener); resolve(result);
                    }
                };
                testBot.addEventListener('message', listener); testBot.postMessage(data);
            }), snapshot);
            assert.equal(move.piece, move.hold ? (hold || sequence[index+1]) : sequence[index], 'retained tree respects real HOLD/NEXT');
            const placement = { type:move.piece, x:move.x+(move.piece==='I'?1:0), y:move.y, rotation:move.rotation, tspin:move.tspin==='full'?2:move.tspin==='mini'?1:0 };
            assert.ok(board.valid(placement), 'retained tree placement does not collide');
            assert.ok(!board.valid({...placement,y:placement.y+1}), 'placement is grounded');
            const consumed = move.hold && !hold ? 2 : 1;
            if (move.hold) { hold=sequence[index]; holds++; }
            const locked=board.lock(placement,b2b,combo); board=locked.board; b2b=locked.b2b; combo=locked.combo;
            assert.ok(!locked.lock.lockedOut);
            const extra=sequence.slice(index+11,index+11+consumed); index+=consumed;
            await page.evaluate(pieces => {
                testBot.postMessage({type:'commit'});
                for (const piece of pieces) testBot.postMessage({type:'addNextPiece',piece});
            }, extra);
        }
        const messages = await page.evaluate(() => { testBot.terminate(); return testMessages; });
        assert.ok(holds>0, 'exercise HOLD transitions');
        assert.ok(messages.some(m=>m.type==='debug' && m.message.includes('REUSED')), 'retain the search across pieces');
        const score = await page.evaluate(() => new Promise((resolve,reject) => {
            const worker = new Worker('./app/84-ai-scoring-worker.js');
            const board = Array.from({length:40},()=>Array(10).fill(null));
            const timer=setTimeout(()=>{worker.terminate();reject(Error('viewer timeout'));},10000);
            worker.onmessage=event=>{
                if(!['done','error'].includes(event.data.type))return;
                clearTimeout(timer);worker.terminate();
                event.data.type==='error'?reject(Error(event.data.message)):resolve(event.data.results[0]);
            };
            worker.postMessage({type:'score',runId:1,replay:true,pages:[{p1:{board,active:'T',hold:'',next:'IOLJSZTIOL',operation:{type:'T',rotation:0,x:4,y:39}}}],operationPages:[0],nodeBudget:120000,detailNodeBudget:120000,thresholdScore:999999,planLength:12});
        }));
        assert.equal(score.searchEngine,'wasm');
        assert.equal(score.status,'scored');
        assert.ok(score.aiPlan.length>=5, 'built viewer receives the actual WASM continuation');
        console.log(JSON.stringify({passed:true,moves:50,holds,viewerPlan:score.aiPlan.length}));
    } finally { await browser.close(); }
})().catch(error=>{console.error(error);process.exitCode=1;});
