/* Build first, serve dist/pages, then run with PLAYWRIGHT_MODULE pointing to
   playwright if it is not installed locally. Uses isolated browser storage. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:8765/';
const output = process.env.HUB_TEST_OUTPUT;
const board = () => Array.from({length:40}, () => Array(10).fill(null));
const pages = Array.from({length:4}, (_, index) => {
    const b = board(); for(let x=0; x<=index; x++) b[39][x]='G';
    return {p1:{board:b,next:'TILJSZO',hold:'O'},p2:{board:board(),next:'IOTSZLJ',hold:''}};
});
const fixture = {v:3,m:'1P',currentCase:0,cases:[{name:'練習元の記録',kind:'snapshot',gameMode:'1P',pages}]};

(async () => {
 const browser = await chromium.launch({headless:true});
 const context = await browser.newContext({viewport:{width:1280,height:800},serviceWorkers:'block'});
 const page = await context.newPage();
 const errors=[]; page.on('pageerror', error=>errors.push(error.message));
 page.on('dialog', async dialog=>{errors.push(dialog.message()); await dialog.dismiss();});
 let checks=0;
 const check = (condition,message)=>{assert.ok(condition,message);checks++;};
 async function mode(expected) { await page.waitForFunction(value=>document.body.dataset.mode===value,expected); checks++; }
 async function navigationAction(id) {
   if (await page.locator('#viewer-pane').isHidden()) {
    const simulator=page.frame({url:/index\.html\?.*workspace=1/});
    await simulator.locator('#shareBtn').click();
    await simulator.locator('#workspace-reference').click();
    if (id==='#narrow-viewer') { await page.waitForFunction(()=>document.body.dataset.narrowPane==='viewer'); return; }
   }
   const viewer=page.frame({url:/\/F\/index\.html\?workspace=1/});
   await viewer.locator('#viewer-share-btn').click();
   await viewer.locator(id).click();
   if (id.startsWith('#narrow-')) await page.waitForFunction(pane=>document.body.dataset.narrowPane===pane,id.slice('#narrow-'.length));
 }
 async function screenshot(name) { if(output) {fs.mkdirSync(output,{recursive:true}); await page.screenshot({path:path.join(output,name+'.png')});} }
 try {
  await page.goto(base,{waitUntil:'networkidle'});
  const sim = page.frame({url:/index\.html\?.*workspace=1/});
  const viewer = page.frame({url:/\/F\/index\.html\?workspace=1/});
  check(sim && viewer, 'both native apps loaded');
  await mode('simulator');
  assert.deepEqual(await sim.evaluate(()=>[innerWidth,innerHeight]), [1280,800]); checks++;
  check(await page.locator('#workspace-bar').count()===0,'no surrounding toolbar reduces the native viewport');
  const normal = await sim.evaluate(()=>getGameStateForExport());
  const frameIdentity = await sim.evaluate(()=>{window.__workflowIdentity=crypto.randomUUID();return window.__workflowIdentity;});
  await sim.locator('#settingsBtn').click();
  await sim.locator('.lab-appearance-select').selectOption('dark');
  await viewer.waitForFunction(()=>document.documentElement.dataset.labTheme==='dark'); checks++;
  await sim.locator('.lab-appearance-select').selectOption('light');
  await viewer.waitForFunction(()=>document.documentElement.dataset.labTheme==='light'); checks++;
  await sim.locator('#settings-close').click();
  await screenshot('simulator');
  await sim.locator('#startGameBtn').click();
  await mode('playing');
  await page.keyboard.press('Space');
  await sim.locator('#backToEditorBtn').click();
  await mode('simulator');
  check(await page.locator('#viewer-pane').isHidden(),'normal return never opens viewer');
  check(await page.evaluate(()=>localStorage.getItem('tetrisHubAutoData'))===null,'Hub does not autosave recordings');

  await sim.locator('#shareBtn').click();
  await sim.locator('#workspace-open-replay').click();
  await page.locator('#replay-file').setInputFiles({name:'practice.tetrisevent.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify(fixture))});
  await mode('viewer');
  check(await viewer.locator('#back-to-editor-btn').isHidden(),'no generic screen button');
  check(await viewer.locator('#viewer-controls .lab-settings-open').count()===0,'no viewer appearance settings button');
  await viewer.locator('#viewer-page-slider').fill('2');
  await viewer.locator('#viewer-page-slider').dispatchEvent('input');
  check(await viewer.evaluate(()=>currentPageIndex)===2,'source seek applied');
  await screenshot('viewer');
  await viewer.locator('#viewer-simulator-btn').click();
  await mode('split');
  const practiceState = await sim.evaluate(()=>getGameStateForExport());
  check(practiceState.p1.b.endsWith('GGG_______'),'selected page transferred');
  const simBounds=await page.locator('#simulator-pane').boundingBox();
  const viewerBounds=await page.locator('#viewer-pane').boundingBox();
  check(simBounds.x < viewerBounds.x,'simulator left, viewer right');
  await screenshot('split');
  await viewer.locator('#viewer-page-slider').fill('3');
  await viewer.locator('#viewer-page-slider').dispatchEvent('input');
  assert.deepEqual(await sim.evaluate(()=>getGameStateForExport()),practiceState); checks++;
  await navigationAction('#wide-button'); await mode('viewer');
  await navigationAction('#resume-button'); await mode('split');
  check(await viewer.evaluate(()=>currentPageIndex)===3,'wide/resume retains independent viewer cursor');
  assert.deepEqual(await sim.evaluate(()=>getGameStateForExport()),practiceState); checks++;
  await sim.locator('#startGameBtn').click(); await mode('playing');
  check(await page.locator('#viewer-pane').isHidden(),'viewer hidden during game');
  await page.keyboard.press('Space');
  await sim.locator('#backToEditorBtn').click(); await mode('split');
  check(await sim.evaluate(()=>window.__workflowIdentity)===frameIdentity,'native simulator not reloaded during transitions');
  await navigationAction('#source-button'); await mode('viewer');
  check(await viewer.evaluate(()=>currentPageIndex)===2,'return to exact practice anchor');
  await navigationAction('#resume-button'); await mode('split');
  await sim.locator('#startGameBtn').click(); await mode('playing');
  await page.keyboard.press('Space');
  await sim.locator('#exportFumenBtn').click(); await mode('viewer');
  check(await viewer.locator('#source-button').evaluate(button=>button.style.display!=='none'),'practice recording has source return');
  check(await viewer.evaluate(()=>fumenPages.length)>0,'practice recording is playable');
  await navigationAction('#source-button');
  await viewer.waitForFunction(()=>currentPageIndex===2);
  check(await viewer.evaluate(()=>fumenPages.length)===4,'source remains immutable after practice recording');

  await navigationAction('#resume-button'); await mode('split');
  await page.setViewportSize({width:390,height:844});
  check(await page.locator('#viewer-pane').isHidden(),'mobile initially shows preparation');
  await navigationAction('#narrow-viewer');
  check(await page.locator('#simulator-pane').isHidden(),'mobile switches to reference');
  await screenshot('mobile-viewer');
  await navigationAction('#narrow-simulator');
  await navigationAction('#home-button'); await mode('simulator');
  assert.deepEqual(await sim.evaluate(()=>getGameStateForExport()),normal); checks++;
  await sim.locator('#startGameBtn').click(); await mode('playing');
  await sim.locator('#backToEditorBtn').click(); await mode('simulator');
  check(await page.locator('#viewer-pane').isHidden(),'old practice context cannot change normal return');
  check(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'mobile has no horizontal overflow');

  // Viewer links, page selection and repository-prefix hosting.
  const share = new URL('F/index.html',base); share.search='?page=3'; share.hash=Buffer.from(JSON.stringify(fixture)).toString('base64');
  await page.goto(share.href,{waitUntil:'networkidle'}); await mode('viewer');
  const linkedViewer=page.frame({url:/\/F\/index\.html\?workspace=1/});
  check(await linkedViewer.evaluate(()=>currentPageIndex)===2,'direct viewer link opens requested page');
  // Messages from unrelated windows cannot start games or replace a document.
  await page.evaluate(()=>window.postMessage({source:'sim',type:'workspaceStarted'},location.origin));
  check(await page.locator('body').getAttribute('data-mode')==='viewer','untrusted sender ignored');

  // Crash/reload recovery preserves two independent cursors and the edited
  // simulator draft. It never adds a user-managed save list.
  await page.setViewportSize({width:1280,height:800});
  await linkedViewer.locator('#viewer-simulator-btn').click(); await mode('split');
  let recoveredSim=page.frame({url:/index\.html\?.*workspace=1/});
  await linkedViewer.locator('#viewer-page-slider').fill('3');
  await linkedViewer.locator('#viewer-page-slider').dispatchEvent('input');
  const edited=await recoveredSim.evaluate(()=>{
    const state=getGameStateForExport();state.p1.b=state.p1.b.slice(0,-1)+'Z';applyGameState(state);return state;
  });
  await page.locator('#workspace-divider').press('ArrowRight');
  async function recoveryMatches(kind) {
    for (let attempt=0;attempt<60;attempt++) {
      const matches=await page.evaluate(async kind=>{
      const db=await new Promise((resolve,reject)=>{const r=indexedDB.open('tetris-workspace-recovery',1);r.onsuccess=()=>resolve(r.result);r.onerror=()=>reject(r.error);});
      const all=await new Promise(resolve=>{const r=db.transaction('workspaces').objectStore('workspaces').getAll();r.onsuccess=()=>resolve(r.result);});db.close();
      return all.some(({state:s})=>kind==='split' ? s.flow.mode==='split' && s.sim.p1.b.endsWith('Z') && s.currentDocument.context.pageIndex===3 : s.flow.mode==='playing' && s.interruptedRecord?.data);
      },kind);
      if(matches) return;
      await new Promise(resolve=>setTimeout(resolve,200));
    }
    throw new Error('Recovery snapshot was not persisted: '+kind);
  }
  await recoveryMatches('split');
  await page.reload({waitUntil:'networkidle'}); await mode('split');
  recoveredSim=page.frame({url:/index\.html\?.*workspace=1/});
  let recoveredViewer=page.frame({url:/\/F\/index\.html\?workspace=1/});
  assert.deepEqual(await recoveredSim.evaluate(()=>getGameStateForExport()),edited);checks++;
  check(await recoveredViewer.evaluate(()=>currentPageIndex)===3,'reload retains viewer cursor');
  check(await page.locator('#workspace-divider').getAttribute('aria-valuenow')==='55','reload retains split width');
  await navigationAction('#source-button');await mode('viewer');
  check(await recoveredViewer.evaluate(()=>currentPageIndex)===2,'reload retains original practice anchor');
  await navigationAction('#resume-button');await mode('split');
  await recoveredSim.locator('#startGameBtn').click();await mode('playing');
  await page.keyboard.press('Space');
  await recoveryMatches('playing');
  await page.reload({waitUntil:'networkidle'});await mode('split');
  recoveredSim=page.frame({url:/index\.html\?.*workspace=1/});
  check(await recoveredSim.evaluate(()=>gameState)==='EDITING','recovered game is stopped');
  await navigationAction('#interrupted-button');await mode('viewer');
  check(await page.frame({url:/\/F\/index\.html\?workspace=1/}).locator('#source-button').evaluate(button=>button.style.display!=='none'),'interrupted recording retains source');
  check(await page.locator('#save-replay-form').count()===0,'no user save form');
  check(await page.locator('#record-list').count()===0,'no replay library');
  await navigationAction('#home-button');await mode('simulator');
  const aiSim=page.frame({url:/index\.html\?.*workspace=1/});
  await aiSim.locator('#p1-ai-toggle').check();
  await aiSim.locator('#startGameBtn').click();await mode('playing');
  await aiSim.waitForFunction(()=>window.createRecordedReplayCollection?.()!=null,null,{timeout:20000});
  check(await aiSim.evaluate(()=>players[0].isAi),'browser WASM AI plays successfully');
  await recoveryMatches('playing');
  await page.reload({waitUntil:'networkidle'});await mode('simulator');
  const restoredAi=page.frame({url:/index\.html\?.*workspace=1/});
  check(await restoredAi.locator('#p1-ai-toggle').isChecked(),'AI selection survives recovery');
  assert.deepEqual(errors,[]);
  check(await page.evaluate(()=>localStorage.getItem('tetrisHubData'))===null,'Hub does not save a library');
  console.log(JSON.stringify({passed:true,checks,base,errors}));
 } finally { await browser.close(); }
})().catch(error=>{console.error(error);process.exitCode=1;});
