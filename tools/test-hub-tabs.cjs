/* New links are fresh workspaces; reload only resumes that tab/history entry. */
const assert=require('node:assert/strict');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const base=process.argv[2]||'http://127.0.0.1:8765/';
let checks=0;
function equal(a,b,message){assert.deepEqual(a,b,message);checks++;}
const sim=page=>page.frame({url:/index\.html\?.*workspace=1/});
const viewer=page=>page.frame({url:/\/F\/index\.html\?workspace=1/});
const identity=page=>page.evaluate(()=>history.state.tetrisWorkspaceId);
async function prepared(page) {
    await page.waitForFunction(()=>document.body.dataset.workspaceReady==='true');
    await page.waitForFunction(()=>document.querySelector('#iframe-sim').contentWindow?.document.getElementById('p1-next-icons')?.childElementCount>0);
    await sim(page).waitForFunction(()=>editorData.p1.board && typeof window.createRecordedReplayCollection==='function');
}
async function state(page){return sim(page).evaluate(()=>getGameStateForExport());}
async function setMarker(page,column,piece){
    await sim(page).evaluate(({column,piece})=>{
        const value=getGameStateForExport();
        value.p1.b='_'.repeat(390+column)+piece+'_'.repeat(9-column);
        applyGameState(value);
    },{column,piece});
}
async function saved(page,column,piece){
    for(let attempt=0;attempt<100;attempt++) {
    const matches=await page.evaluate(async ({column,piece})=>{
        const db=await new Promise((resolve,reject)=>{
            const r=indexedDB.open('tetris-workspace-recovery',1);
            r.onsuccess=()=>resolve(r.result);r.onerror=()=>reject(r.error);
        });
        if(!db.objectStoreNames.contains('workspaces')){db.close();return false;}
        const record=await new Promise(resolve=>{
            const r=db.transaction('workspaces').objectStore('workspaces').get(history.state.tetrisWorkspaceId);
            r.onsuccess=()=>resolve(r.result);
        });db.close();
        return record?.state.sim?.p1.b[390+column]===piece;
    },{column,piece});
    if(matches)return;
    await new Promise(resolve=>setTimeout(resolve,100));
    }
    throw new Error('Recovery snapshot was not written');
}
async function reload(page){await page.reload({waitUntil:'networkidle'});await prepared(page);}
(async()=>{
    const browser=await chromium.launch({headless:true});
    try {
        const context=await browser.newContext({viewport:{width:1280,height:800},serviceWorkers:'block'});
        const errors=[];
        context.on('page',page=>page.on('pageerror',e=>errors.push(e.message)));
        const a=await context.newPage();await a.goto(base,{waitUntil:'networkidle'});await prepared(a);
        await setMarker(a,0,'Z');await saved(a,0,'Z');const aId=await identity(a);
        const b=await context.newPage();await b.goto(base,{waitUntil:'networkidle'});await prepared(b);
        equal(await b.locator('body').getAttribute('data-mode'),'simulator','normal new link opens only simulator');
        equal((await state(b)).p1.b,'_'.repeat(400),'new tab never takes another tab\'s draft');
        const bId=await identity(b);assert.notEqual(aId,bId);checks++;
        await setMarker(b,1,'J');await saved(b,1,'J');
        equal((await state(a)).p1.b.slice(-10),'Z_________','B cannot change live A');
        await reload(a);equal(await identity(a),aId,'reload retains ownership');
        equal((await state(a)).p1.b.slice(-10),'Z_________','A resumes only A');
        await reload(b);equal(await identity(b),bId);
        equal((await state(b)).p1.b.slice(-10),'_J________','B resumes only B');
        equal(await sim(a).locator('#workspace-new-simulator').count(),0,'no new menu item');

        // window.open without noopener copies sessionStorage on real browsers.
        // The launch must still be empty and independent from its opener.
        await a.evaluate(()=>sessionStorage.setItem('tab-test-opener','copied'));
        const opened=context.waitForEvent('page');
        await a.evaluate(url=>{window.open(url,'_blank');},base);
        const popup=await opened;await popup.waitForLoadState('networkidle');await prepared(popup);
        equal(await popup.evaluate(()=>sessionStorage.getItem('tab-test-opener')),'copied');
        equal((await state(popup)).p1.b,'_'.repeat(400),'copied session storage does not imply recovery');
        assert.notEqual(await identity(popup),aId);checks++;
        await popup.close();

        // Browser tab duplication can also copy history.state. A live writer
        // must never let the copied identity overwrite its recovery record.
        await context.addInitScript(id=>{
            if(location.search.includes('duplicate-probe')){
                history.replaceState({tetrisWorkspaceId:id},'');
                sessionStorage.setItem('tetrisWorkspaceTab',id);
            }
        },aId);
        const duplicate=await context.newPage();
        await duplicate.goto(new URL('hub/?duplicate-probe=1',base).href,{waitUntil:'networkidle'});await prepared(duplicate);
        assert.notEqual(await identity(duplicate),aId);checks++;
        await reload(duplicate);
        assert.notEqual(await identity(duplicate),aId);checks++;
        await setMarker(duplicate,2,'T');await saved(duplicate,2,'T');
        await reload(a);equal((await state(a)).p1.b.slice(-10),'Z_________','copied identity cannot overwrite A');
        await duplicate.close();

        const empty=()=>Array.from({length:40},()=>Array(10).fill(null));
        const pages=Array.from({length:3},(_,i)=>{const board=empty();board[39][i]='I';return {p1:{board,next:'TIOSZJL',hold:''}};});
        const fixture={v:3,m:'1P',cases:[{name:'リンクのリプレイ',kind:'snapshot',gameMode:'1P',pages}]};
        const link=new URL('F/index.html?page=2',base);link.hash=Buffer.from(JSON.stringify(fixture)).toString('base64');
        const replay=await context.newPage();await replay.goto(link.href,{waitUntil:'networkidle'});
        await replay.waitForFunction(()=>document.body.dataset.mode==='viewer');
        equal(await viewer(replay).evaluate(()=>[fumenPages.length,currentPageIndex]),[3,1],'explicit replay link wins');
        await replay.goto(base,{waitUntil:'networkidle'});await prepared(replay);
        equal(await replay.locator('body').getAttribute('data-mode'),'simulator','opening normal link from viewer starts simulator');
        equal((await state(replay)).p1.b,'_'.repeat(400));
        await setMarker(replay,3,'O');await saved(replay,3,'O');
        await replay.goto(new URL('hub/',base).href,{waitUntil:'networkidle'});await prepared(replay);
        equal((await state(replay)).p1.b,'_'.repeat(400),'ordinary same-URL navigation is a new launch');
        await replay.close();

        const fresh=await context.newPage();await fresh.goto(new URL('hub/?fresh=1',base).href,{waitUntil:'networkidle'});await prepared(fresh);
        await fresh.waitForFunction(()=>!new URL(location).searchParams.has('fresh'));
        await setMarker(fresh,4,'L');await saved(fresh,4,'L');const freshId=await identity(fresh);
        await reload(fresh);equal(await identity(fresh),freshId);
        equal((await state(fresh)).p1.b.slice(-10),'____L_____','one-time fresh flag does not disable future reload recovery');
        await fresh.close();

        const kept=await a.evaluate(async id=>{
            const {writeRecovery,readRecovery}=await import('./js/recovery.js');
            for(let i=0;i<12;i++) await writeRecovery('tab-retention-'+i,{version:1,test:true});
            return {own:(await readRecovery(id))?.state.sim.p1.b,missing:await readRecovery('unknown-tab')};
        },aId);
        equal(kept.own.slice(-10),'Z_________','other tabs never evict an older active workspace');
        equal(kept.missing,null,'missing ID does not fall back to the newest record');
        equal(errors,[],'no browser errors');
        await context.close();
        console.log(JSON.stringify({passed:true,checks}));
    } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
