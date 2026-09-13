const assert=require('node:assert/strict');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const base=process.argv[2]||'http://127.0.0.1:8767/';
const failures=[];let checks=0;
const check=(ok,message)=>{checks++;if(!ok)failures.push(message);};
const sim=p=>p.frame({url:/index\.html\?.*workspace=1/});
const viewer=p=>p.frame({url:/\/F\/index\.html\?workspace=1/});
const settle=p=>p.waitForTimeout(300);
const ready=p=>p.waitForFunction(()=>document.body.dataset.workspaceReady==='true');
const board=()=>Array.from({length:40},()=>Array(10).fill(null));
const b=board();b[39][0]='T';
const fixture={v:3,m:'1P',cases:[{name:'drag regression',kind:'snapshot',gameMode:'1P',pages:[{p1:{board:b,next:'TILJSZO',hold:'O'},p2:{board:board(),next:'',hold:''}}]}]};
async function drag(p,selector,fraction,release=true){const r=await p.locator(selector).boundingBox();if(!r){check(false,selector+' missing');return false;}await p.mouse.move(r.x+r.width/2,r.y+r.height/2);await p.mouse.down();await p.mouse.move(p.viewportSize().width*fraction,r.y+r.height/2,{steps:12});await settle(p);if(release){await p.mouse.up();await settle(p);}return true;}
async function both(p,message){const visible=await p.locator('#simulator-pane').isVisible()&&await p.locator('#viewer-pane').isVisible();check(visible&&await p.locator('body').getAttribute('data-pane-view')==='both',message);return visible;}
(async()=>{const browser=await chromium.launch({headless:true});try{
 for(const width of [1280,720,390]){
    const context=await browser.newContext({viewport:{width,height:844},serviceWorkers:'block'}),p=await context.newPage();
    const errors=[];p.on('pageerror',e=>errors.push(e.message));
    await p.goto(base,{waitUntil:'networkidle'});await ready(p);
    await sim(p).locator('#startGameBtn').click();await p.waitForFunction(()=>document.body.dataset.mode==='playing');await p.keyboard.press('Space');
    await sim(p).locator('#exportFumenBtn').click();await p.waitForFunction(()=>document.body.dataset.mode==='viewer');await settle(p);
    const replay=await viewer(p).evaluate(()=>JSON.stringify(getCollectionDataForExport()));
    await drag(p,'#pane-edge',.5);const divided=await both(p,width+': pulling normal recording to middle stays split');
    if(divided){
        check(await viewer(p).evaluate(()=>JSON.stringify(getCollectionDataForExport()))===replay,width+': reveal leaves current replay intact');
        await drag(p,'#workspace-divider',.92);check(await p.locator('#pane-edge').isVisible()&&await p.locator('#pane-edge').getAttribute('data-side')==='right',width+': simulator full has right grip');
        await drag(p,'#pane-edge',.5);await both(p,width+': right grip restores split');
        await drag(p,'#workspace-divider',.08);check(await p.locator('#pane-edge').getAttribute('data-side')==='left',width+': viewer full has left grip');
        await drag(p,'#pane-edge',.92,false);
        check(await p.locator('#iframe-editor-custom').evaluate(el=>getComputedStyle(el).filter)==='blur(3px)',width+': edge pull also previews stowing viewer');
        check(await p.locator('#iframe-sim').evaluate(el=>getComputedStyle(el).filter)==='none',width+': simulator stays clear');
        await p.mouse.move(width*.42,422,{steps:12});await settle(p);
        check(await p.locator('#iframe-editor-custom').evaluate(el=>getComputedStyle(el).filter)==='none',width+': returning from edge clears blur');
        await p.mouse.up();await settle(p);await both(p,width+': release outside stow zone remains split');
        const ratio=Number(await p.locator('#workspace-divider').getAttribute('aria-valuenow'));
        check(Math.abs(ratio-42)<=1,width+': released width is retained');
        await drag(p,'#workspace-divider',.24);await both(p,width+': nonblurred 24 percent is split');
        check(Number(await p.locator('#workspace-divider').getAttribute('aria-valuenow'))===24,width+': 24 percent is not clamped to 30');
        await p.waitForTimeout(2200);await p.reload({waitUntil:'networkidle'});await ready(p);await settle(p);
        await both(p,width+': normal replay split survives reload');
        await sim(p).locator('#startGameBtn').click();await p.waitForFunction(()=>document.body.dataset.mode==='playing');await sim(p).locator('#backToEditorBtn').click();await settle(p);
        check(await p.locator('body').getAttribute('data-mode')==='simulator',width+': normal play keeps normal return origin');
        check(await p.locator('#pane-edge').isVisible(),width+': right grip remains after ordinary play return');
    }
    const url=new URL('F/index.html',base);url.hash=Buffer.from(JSON.stringify(fixture)).toString('base64');await p.goto(url.href,{waitUntil:'networkidle'});await ready(p);
    await viewer(p).locator('#viewer-page-indicator').hover();if(await viewer(p).locator('#viewer-page-indicator').getAttribute('aria-expanded')!=='true')await viewer(p).locator('#viewer-page-indicator').click();
    await viewer(p).locator('#viewer-simulator-btn').click();await settle(p);
    await both(p,width+': replay practice starts split');
    check(errors.length===0,width+': no runtime errors');await context.close();
 }
 console.log(JSON.stringify({passed:!failures.length,checks,failures}));assert.deepEqual(failures,[]);
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
