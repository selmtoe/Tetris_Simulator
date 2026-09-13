/* Compare rendered native controls with the same apps inside the workspace.
   Build/serve dist/pages first. APPROVED_UI_BASE_URL optionally uses a separate
   approved preview; otherwise the standalone public apps are the reference. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:8765/';
const reference = process.env.APPROVED_UI_BASE_URL || base;
const output = process.env.HUB_TEST_OUTPUT;
const board = () => Array.from({length:40}, () => Array(10).fill(null));
const pages = Array.from({length:4}, (_, index) => {
    const b=board(); for(let x=0;x<=index;x++) b[39][x]='G';
    return {p1:{board:b,next:'TILJSZO',hold:'O'},p2:{board:board(),next:'IOTSZLJ',hold:''}};
});
const fixture={v:3,m:'1P',cases:[{name:'練習元の記録',kind:'snapshot',gameMode:'1P',pages}]};
const simSelectors=['#editor-container','#field-editor-canvas-p1','.mode-selection','#startGameBtn','#shareBtn'];
const viewerSelectors=['#viewer-controls','#viewerCanvas','.viewer-top-row','#viewer-page-slider','#viewer-simulator-btn','#back-to-editor-btn','#viewer-share-btn'];
let checks=0;
async function settle(frame) {
    await frame.evaluate(async()=>{await document.fonts.ready;await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));});
    await frame.evaluate(()=>Promise.all(document.getAnimations().filter(animation=>Number.isFinite(animation.effect?.getComputedTiming().endTime)).map(animation=>animation.finished.catch(()=>{}))));
}
async function measure(frame,selectors) {
    await settle(frame);
    return frame.evaluate(selectors=>({
        viewport:[innerWidth,innerHeight],
        theme:document.documentElement.dataset.labTheme,
        buttons:[...document.querySelectorAll(selectors[0]+' button')].filter(el=>el.offsetWidth&&el.offsetHeight).map(el=>{
            const r=el.getBoundingClientRect();
            return {id:el.id,label:el.id==='back-to-editor-btn'?'screen-slot':el.textContent.trim(),rect:[r.x,r.y,r.width,r.height]};
        }),
        elements:selectors.map(selector=>{
            const el=document.querySelector(selector),r=el.getBoundingClientRect(),s=getComputedStyle(el);
            return {selector,rect:[r.x,r.y,r.width,r.height],font:s.font,color:s.color,background:s.backgroundColor,border:s.borderRadius};
        })
    }),selectors);
}
async function compare(label,expectedFrame,actualFrame,selectors,actualElement,referencePage) {
    await referencePage.mouse.move(0,0);
    await actualElement.page().mouse.move(0,0);
    const expected=await measure(expectedFrame,selectors),actual=await measure(actualFrame,selectors);
    if(output) {
        fs.mkdirSync(output,{recursive:true});
        await referencePage.screenshot({path:path.join(output,label+'-reference.png')});
        await actualElement.screenshot({path:path.join(output,label+'-workspace.png')});
        fs.writeFileSync(path.join(output,label+'.json'),JSON.stringify({expected,actual},null,2));
    }
    assert.deepEqual(actual.viewport,expected.viewport,label+' viewport');checks++;
    assert.equal(actual.theme,expected.theme,label+' theme');checks++;
    assert.equal(actual.buttons.length,expected.buttons.length,label+' visible controls');checks++;
    for(let i=0;i<expected.buttons.length;i++) {
        const a=actual.buttons[i],e=expected.buttons[i];
        assert.deepEqual({...a,rect:null},{...e,rect:null},label+' control '+e.id);
        for(let j=0;j<4;j++) assert.ok(Math.abs(a.rect[j]-e.rect[j])<0.05,`${label} ${e.id}: ${a.rect} != ${e.rect}`);
        checks++;
    }
    for(let i=0;i<expected.elements.length;i++) {
        const a=actual.elements[i],e=expected.elements[i];
        for(let j=0;j<4;j++) assert.ok(Math.abs(a.rect[j]-e.rect[j])<0.05,`${label} ${e.selector}: ${a.rect} != ${e.rect}`);
        assert.deepEqual({...a,rect:null},{...e,rect:null},label+' style '+e.selector);checks++;
    }
}
(async()=>{
    const browser=await chromium.launch({headless:true});
    try {
        for(const theme of ['light','dark']) for(const width of [1280,390]) {
            const context=await browser.newContext({viewport:{width,height:800},serviceWorkers:'block'});
            await context.addInitScript(theme=>localStorage.setItem('lab-appearance-mode',theme),theme);
            const hub=await context.newPage(),native=await context.newPage(),nativeViewer=await context.newPage();
            const errors=[];
            for(const page of [hub,native,nativeViewer]) page.on('pageerror',e=>errors.push(e.message));
            await Promise.all([
                hub.goto(new URL('?fresh=1',base).href,{waitUntil:'networkidle'}),
                native.goto(new URL('index.html?standalone=1',reference).href,{waitUntil:'networkidle'}),
                nativeViewer.goto(new URL('F/index.html?standalone=1',reference).href,{waitUntil:'networkidle'})
            ]);
            const sim=hub.frame({url:/index\.html\?.*workspace=1/});
            const viewer=hub.frame({url:/\/F\/index\.html\?workspace=1/});
            const label=theme+'-'+width;
            await compare(label+'-preparation',native.mainFrame(),sim,simSelectors,hub.locator('#iframe-sim'),native);
            assert.equal(await sim.locator('#exportFumenBtn').textContent(),'記録');checks++;
            await sim.locator('#shareBtn').click();
            await sim.locator('#workspace-open-replay').click();
            await hub.locator('#replay-file').setInputFiles({name:'reference.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify(fixture))});
            await hub.waitForFunction(()=>document.body.dataset.mode==='viewer');
            await viewer.evaluate(()=>loadPage(2));
            await nativeViewer.evaluate(fixture=>{applyCollectionData(fixture);document.getElementById('view-mode-btn').click();loadPage(2);},fixture);
            await compare(label+'-viewer',nativeViewer.mainFrame(),viewer,viewerSelectors,hub.locator('#iframe-editor-custom'),nativeViewer);
            assert.equal(await viewer.locator('#viewer-simulator-btn').textContent(),'ここから練習');checks++;
            await viewer.locator('#viewer-simulator-btn').click();
            await hub.waitForFunction(()=>document.body.dataset.mode==='split');
            const simWidth=await sim.evaluate(()=>innerWidth);
            await native.setViewportSize({width:simWidth,height:800});
            await native.evaluate(state=>applyGameState(state),await sim.evaluate(()=>getGameStateForExport()));
            // Clear click focus so screenshot comparisons reflect layout only.
            await viewer.evaluate(()=>document.activeElement?.blur());
            await compare(label+'-practice',native.mainFrame(),sim,simSelectors,hub.locator('#iframe-sim'),native);
            if(width>800) {
                await nativeViewer.setViewportSize({width:await viewer.evaluate(()=>innerWidth),height:800});
                await compare(label+'-reference',nativeViewer.mainFrame(),viewer,viewerSelectors,hub.locator('#iframe-editor-custom'),nativeViewer);
            }
            const editor=await context.newPage();
            editor.on('pageerror',e=>errors.push(e.message));
            await editor.goto(new URL('F/index.html?standalone=1',base).href,{waitUntil:'networkidle'});
            await editor.evaluate(fixture=>{applyCollectionData(fixture);loadPage(2);document.getElementById('back-to-editor-btn').click();},fixture);
            await nativeViewer.setViewportSize({width,height:800});
            await nativeViewer.locator('#back-to-editor-btn').click();
            await nativeViewer.evaluate(()=>document.activeElement?.blur());
            await compare(label+'-editor',nativeViewer.mainFrame(),editor.mainFrame(),['#editor-container','#case-selector','#view-mode-btn','#send-to-simulator'],editor.locator('body'),nativeViewer);
            assert.deepEqual(errors,[]);checks++;
            await context.close();
        }
        console.log(JSON.stringify({passed:true,checks,base,reference}));
    } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
