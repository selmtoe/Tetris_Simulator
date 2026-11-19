/* Isolated trial checks: link files, pane gestures and origin-aware returns. */
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const {pathToFileURL} = require('node:url');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:8767/';
const output = process.env.HUB_TEST_OUTPUT || 'C:/Users/hirom/.codex/tmp/hub-divider-checks';
const board = () => Array.from({length:40}, () => Array(10).fill(null));
const fixture = {v:3,m:'1P',cases:[{name:'仕切りのテスト <記録>',kind:'snapshot',gameMode:'1P',pages:Array.from({length:4}, (_,i) => {
    const b=board();for(let x=0;x<=i;x++)b[39][x]='G';return {p1:{board:b,next:'TILJSZO',hold:'O'},p2:{board:board(),next:'',hold:''}};
})}]};
const link=new URL('F/index.html?page=2',base);link.hash=Buffer.from(JSON.stringify(fixture)).toString('base64');
let checks=0;
const equal=(a,b,m)=>{assert.deepEqual(a,b,m);checks++;};
const ok=(a,m)=>{assert.ok(a,m);checks++;};
const sim=p=>p.frame({url:/index\.html\?.*workspace=1/});
const viewer=p=>p.frame({url:/\/F\/index\.html\?workspace=1/});
const state=p=>sim(p).evaluate(()=>getGameStateForExport());
async function mode(p,value){await p.waitForFunction(v=>document.body.dataset.mode===v,value);checks++;}
async function ready(p){await p.waitForFunction(()=>document.body?.dataset.workspaceReady==='true');}
async function expand(v){await v.locator('#viewer-page-indicator').hover();if(await v.locator('#viewer-page-indicator').getAttribute('aria-expanded')!=='true')await v.locator('#viewer-page-indicator').click();}
async function settle(p){await p.waitForTimeout(320);}
async function pull(p, distance=220){const r=await p.locator('#pane-edge').boundingBox();const left=r.x<50;const x=r.x+r.width/2,y=r.y+r.height/2;await p.mouse.move(x,y);await p.mouse.down();await p.mouse.move(x+(left?distance:-distance),y,{steps:14});await p.mouse.up();await settle(p);}
async function drop(frame,text,name){await frame.evaluate(({text,name})=>{const dt=new DataTransfer();dt.items.add(new File([text],name,{type:'text/html'}));document.body.dispatchEvent(new DragEvent('drop',{bubbles:true,cancelable:true,dataTransfer:dt}));},{text,name});}
async function saved(p, expected){
    for(let i=0;i<60;i++){
        const value=await p.evaluate(async()=>{const db=await new Promise(resolve=>{const r=indexedDB.open('tetris-workspace-recovery',1);r.onsuccess=()=>resolve(r.result);});const value=await new Promise(resolve=>{const r=db.transaction('workspaces').objectStore('workspaces').get(history.state.tetrisWorkspaceId);r.onsuccess=()=>resolve(r.result?.state);});db.close();return value;});
        if(value?.flow.mode===expected.mode && (expected.focus===undefined||value.flow.focusPane===expected.focus) && (!expected.recording||value.interruptedRecord?.data))return;
        await p.waitForTimeout(150);
    }throw new Error('Recovery did not catch up.');
}
(async()=>{
 const browser=await chromium.launch({headless:true});
 try{
    await fs.mkdir(output,{recursive:true});
    const context=await browser.newContext({viewport:{width:1280,height:800}});
    const errors=[];context.on('page',p=>p.on('pageerror',e=>errors.push(e.message)));
    const page=await context.newPage();await page.goto(base,{waitUntil:'networkidle'});await ready(page);
    await mode(page,'simulator');
    equal(await sim(page).evaluate(()=>[innerWidth,innerHeight]),[1280,800],'normal full size');
    ok(await page.locator('#pane-edge').isHidden(),'no handle in ordinary preparation');
    await sim(page).locator('#shareBtn').click();
    ok(await sim(page).locator('#advanced-link-options').isVisible(),'settings initially visible');
    equal(await sim(page).locator('#advanced-link-btn,#workspace-reference,#workspace-open-replay,#import-from-data-btn').count(),0,'no unrelated Share actions');
    const before=await sim(page).locator('#share-link-input').inputValue();
    await sim(page).locator('#start-sim-checkbox').check();
    const after=await sim(page).locator('#share-link-input').inputValue();ok(after!==before,'link updates immediately');
    await sim(page).locator('#start-sim-checkbox').uncheck();
    const d1=page.waitForEvent('download');await sim(page).locator('#save-link-btn').click();const download=await d1;
    const file=path.join(output,'saved-simulator.html');await download.saveAs(file);
    const savedSim=await fs.readFile(file,'utf8');
    equal(await sim(page).evaluate(text=>TetrisLinkFile.extract(text),savedSim),await sim(page).locator('#share-link-input').inputValue(),'saved link equals Copy');
    const launcher=await context.newPage();await launcher.goto(pathToFileURL(file).href);await launcher.getByRole('link').click();await ready(launcher);await mode(launcher,'simulator');await launcher.close();
    await page.screenshot({path:path.join(output,'share.png')});
    await sim(page).locator('#share-close').click();
    await sim(page).locator('#startGameBtn').click();await mode(page,'playing');
    await page.keyboard.press('Space');await sim(page).locator('#backToEditorBtn').click();await mode(page,'simulator');
    await sim(page).locator('#startGameBtn').click();await mode(page,'playing');await page.keyboard.press('Space');
    await sim(page).locator('#exportFumenBtn').click();await mode(page,'viewer');await settle(page);
    equal(await viewer(page).locator('#viewer-return-btn').count(),0,'no Return button');await pull(page);await mode(page,'split');await settle(page);
    ok(await page.locator('#viewer-pane').isVisible(),'recording remains visible beside simulator');
    await drop(sim(page),JSON.stringify(fixture),'old.tetrisevent.json');await mode(page,'viewer');await settle(page);
    equal(await viewer(page).evaluate(()=>fumenPages.length),4,'legacy file reads through drop');
    ok(await page.locator('#pane-edge').isVisible(),'import allows simulator to be pulled in');
    await expand(viewer(page));await viewer(page).locator('#viewer-page-slider').fill('1');await viewer(page).locator('#viewer-page-slider').dispatchEvent('input');
    await viewer(page).locator('#viewer-share-btn').click();
    ok(!(await viewer(page).locator('#share-modal').innerText()).includes('リプレイと練習'),'section removed');
    equal(await viewer(page).locator('#records-button,#home-button,#wide-button,#resume-button,#import-event-file-btn').count(),0,'no old navigation');
    const d2=page.waitForEvent('download');await viewer(page).locator('#save-link-btn').click();const replayDownload=await d2;
    const replayFile=path.join(output,'saved-replay.html');await replayDownload.saveAs(replayFile);const savedReplay=await fs.readFile(replayFile,'utf8');
    equal(await viewer(page).evaluate(text=>TetrisLinkFile.extract(text),savedReplay),await viewer(page).locator('#share-link-input').inputValue(),'replay file and clipboard match');
    const replayLauncher=await context.newPage();await replayLauncher.goto(pathToFileURL(replayFile).href);await replayLauncher.getByRole('link').click();await ready(replayLauncher);await mode(replayLauncher,'viewer');equal(await viewer(replayLauncher).evaluate(()=>fumenPages.length),4,'saved replay opens');await replayLauncher.close();
    await viewer(page).locator('#share-close').click();await expand(viewer(page));await viewer(page).locator('#viewer-simulator-btn').click();await mode(page,'split');await settle(page);
    const draft=await state(page);ok(draft.p1.b.endsWith('GG________'),'selected source applied');
    const identity=await sim(page).evaluate(()=>window.__trialIdentity=crypto.randomUUID());
    await expand(viewer(page));await viewer(page).locator('#viewer-page-slider').fill('3');await viewer(page).locator('#viewer-page-slider').dispatchEvent('input');
    equal(await page.locator('#divider-actions,#pane-show-viewer,#pane-show-simulator').count(),0,'no popup menu');
    await page.locator('#workspace-divider').press('Control+ArrowRight');await settle(page);
    equal(await page.locator('body').getAttribute('data-mode'),'split','fold is presentation only');
    ok(await page.locator('#simulator-pane').isHidden(),'viewer focused');
    equal(await state(page),draft,'fold retains preparation');
    await pull(page,15);equal(await page.locator('body').getAttribute('data-pane-view'),'viewer','short pull does not unfold');
    await pull(page);equal(await viewer(page).evaluate(()=>currentPageIndex),3,'unfold retains independent page');
    equal(await sim(page).evaluate(()=>window.__trialIdentity),identity,'pane is pulled without reloading the simulator');
    await page.screenshot({path:path.join(output,'split.png')});
    async function dragTo(percent,cancel=false){const r=await page.locator('#divider-grip').boundingBox();await page.mouse.move(r.x+r.width/2,r.y+r.height/2);await page.mouse.down();await page.mouse.move(1280*percent,400,{steps:14});if(cancel)await page.locator('#workspace-divider').dispatchEvent('pointercancel',{pointerId:1});await page.mouse.up();await settle(page);}
    const grip=await page.locator('#divider-grip').boundingBox();await page.mouse.move(grip.x+grip.width/2,grip.y+grip.height/2);await page.mouse.down();await page.mouse.move(100,400,{steps:12});await settle(page);
    equal(await page.locator('#iframe-sim').evaluate(el=>getComputedStyle(el).filter),'blur(3px)','pane about to stow is blurred');
    equal(await page.locator('#iframe-editor-custom').evaluate(el=>getComputedStyle(el).filter),'none','remaining viewer is clear');
    ok(await page.locator('#divider-hint').isVisible(),'release hint appears');await page.screenshot({path:path.join(output,'stow-preview.png')});
    await page.mouse.move(640,400,{steps:12});await settle(page);equal(await page.locator('#iframe-sim').evaluate(el=>getComputedStyle(el).filter),'none','moving back removes blur');await page.mouse.up();await settle(page);
    await dragTo(.92);equal(await page.locator('body').getAttribute('data-pane-view'),'simulator','drag right expands simulator');
    ok(await page.locator('#viewer-pane').isHidden(),'viewer folded');
    await saved(page,{mode:'split',focus:'simulator'});await page.reload({waitUntil:'networkidle'});await ready(page);await settle(page);
    equal(await page.locator('body').getAttribute('data-pane-view'),'simulator','focus survives reload');equal(await state(page),draft,'draft survives reload');
    await sim(page).locator('#startGameBtn').click();await mode(page,'playing');await page.keyboard.press('Space');
    await sim(page).locator('#backToEditorBtn').click();await mode(page,'split');await settle(page);
    equal(await page.locator('body').getAttribute('data-pane-view'),'both','folded practice still returns to practice');
    await dragTo(.08);equal(await page.locator('body').getAttribute('data-pane-view'),'viewer','drag left expands viewer');
    await pull(page);await dragTo(.08,true);
    equal(await page.locator('body').getAttribute('data-pane-view'),'both','cancelled drag does not fold');
    await page.locator('#workspace-divider').press('Home');await page.locator('#workspace-divider').press('ArrowRight');equal(await page.locator('#workspace-divider').getAttribute('aria-valuenow'),'55','keyboard width');
    await page.locator('#workspace-divider').press('Control+ArrowLeft');await settle(page);
    await sim(page).locator('#startGameBtn').click();await mode(page,'playing');await page.keyboard.press('Space');
    await sim(page).locator('#exportFumenBtn').click();await mode(page,'viewer');await settle(page);
    const latestRecord=await viewer(page).evaluate(()=>JSON.stringify(getCollectionDataForExport()));
    await pull(page);await mode(page,'split');await settle(page);
    equal(await viewer(page).evaluate(()=>JSON.stringify(getCollectionDataForExport())),latestRecord,'pulling a pane never replaces the visible recording');
    equal(await state(page),draft,'record reveal retains draft');
    await page.setViewportSize({width:390,height:844});await settle(page);ok(await page.locator('#viewer-pane').isVisible()&&await page.locator('#simulator-pane').isVisible(),'narrow view retains split');
    await page.locator('#workspace-divider').press('Control+ArrowRight');await settle(page);
    await pull(page,130);ok(await page.locator('#simulator-pane').isVisible()&&await page.locator('#viewer-pane').isVisible(),'narrow left pull restores split');
    await page.screenshot({path:path.join(output,'mobile-reference.png')});
    await page.locator('#workspace-divider').press('Control+ArrowLeft');await settle(page);
    await pull(page,130);ok(await page.locator('#viewer-pane').isVisible()&&await page.locator('#simulator-pane').isVisible(),'narrow right pull restores split');
    ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'no narrow overflow');
    await page.setViewportSize({width:1280,height:800});await page.emulateMedia({reducedMotion:'reduce'});
    await page.locator('#workspace-divider').press('Control+ArrowRight');equal(await page.locator('body').getAttribute('data-pane-motion'),'false','reduced motion respected');
    await drop(viewer(page),savedSim,'saved.html');await mode(page,'simulator');await settle(page);
    ok(await page.locator('#pane-edge').isHidden(),'loading simulator link exits old practice');
    await drop(sim(page),savedReplay,'saved.html');await mode(page,'viewer');await settle(page);equal(await viewer(page).evaluate(()=>fumenPages.length),4,'HTML drop loads replay');
    await page.goto(base,{waitUntil:'networkidle'});await ready(page);await mode(page,'simulator');
    await sim(page).locator('#p1-ai-toggle').check();await sim(page).locator('#startGameBtn').click();await mode(page,'playing');
    await sim(page).waitForFunction(()=>window.createRecordedReplayCollection?.()!=null,null,{timeout:20000});
    await saved(page,{mode:'playing',recording:true});await page.reload({waitUntil:'networkidle'});await ready(page);
    await mode(page,'simulator');ok(await page.locator('#interrupted-notice').isVisible(),'interrupted record appears outside Share');
    await page.locator('#open-interrupted').click();await mode(page,'viewer');await settle(page);await pull(page);await mode(page,'split');
    const touchContext=await browser.newContext({viewport:{width:390,height:844},hasTouch:true,isMobile:true});
    const touch=await touchContext.newPage();touch.on('pageerror',e=>errors.push(e.message));
    await touch.goto(link.href,{waitUntil:'networkidle'});await ready(touch);await expand(viewer(touch));
    await viewer(touch).locator('#viewer-simulator-btn').click();await mode(touch,'split');await settle(touch);
    const cdp=await touchContext.newCDPSession(touch);
    async function swipeEdge(){
        const r=await touch.locator('#pane-edge').boundingBox();const start=r.x+r.width/2,y=r.y+r.height/2,delta=r.x<50?150:-150;
        await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{x:start,y}]});
        for(let i=1;i<=10;i++)await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{x:start+delta*i/10,y}]});
        await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});await settle(touch);
    }
    await touch.locator('#workspace-divider').press('Control+ArrowRight');await settle(touch);
    await swipeEdge();equal(await touch.locator('body').getAttribute('data-pane-view'),'both','touch swipe pulls simulator into split');
    await touch.locator('#workspace-divider').press('Control+ArrowLeft');await settle(touch);
    await swipeEdge();equal(await touch.locator('body').getAttribute('data-pane-view'),'both','touch swipe pulls viewer into split');
    await touchContext.close();
    equal(errors,[],'no page errors');
    console.log(JSON.stringify({passed:true,checks,output}));
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
