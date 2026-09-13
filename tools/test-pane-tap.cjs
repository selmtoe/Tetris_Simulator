const assert = require('node:assert/strict');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:8767/';
let checks=0;
const equal=(a,b,message)=>{assert.deepEqual(a,b,message);checks++;};
const empty=()=>Array.from({length:40},()=>Array(10).fill(null));
const board=empty();board[39][0]='T';
const fixture={v:3,m:'1P',cases:[{name:'tap fixture',kind:'snapshot',gameMode:'1P',pages:[{p1:{board,next:'TILJSZO',hold:'O'},p2:{board:empty(),next:'',hold:''}}]}]};
const url=new URL('F/index.html',base);url.hash=Buffer.from(JSON.stringify(fixture)).toString('base64');
const settle=p=>p.waitForTimeout(300);
const ratio=async p=>Number(await p.locator('#workspace-divider').getAttribute('aria-valuenow'));
async function fold(page, direction) {
    const grip=await page.locator('#workspace-divider').boundingBox();
    await page.mouse.move(grip.x+grip.width/2,400);await page.mouse.down();
    await page.mouse.move(page.viewportSize().width*(direction==='left'?.05:.95),400,{steps:12});await page.mouse.up();await settle(page);
}
(async()=>{const browser=await chromium.launch({headless:true});try{
    for(const width of [390,720,1280]){
        const context=await browser.newContext({viewport:{width,height:844},hasTouch:true,isMobile:width===390,serviceWorkers:'block'});
        const page=await context.newPage();await page.goto(url.href,{waitUntil:'networkidle'});await page.waitForFunction(()=>document.body.dataset.workspaceReady==='true');
        const viewer=page.frame({url:/\/F\/index\.html\?workspace/});
        const replay=await viewer.evaluate(()=>JSON.stringify(getCollectionDataForExport()));
        const history=await page.evaluate(()=>history.length);
        await page.locator('#pane-edge').tap();await settle(page);
        equal(await page.locator('body').getAttribute('data-pane-view'),'both',width+': tapping left grip opens split');
        equal(await ratio(page),20,width+': left pane opens small');
        equal(await viewer.evaluate(()=>JSON.stringify(getCollectionDataForExport())),replay,'tap preserves replay');
        await fold(page,'right');equal(await page.locator('#pane-edge').getAttribute('data-side'),'right','viewer stowed at right');
        await page.locator('#pane-edge').tap();await settle(page);
        equal(await ratio(page),80,width+': tapping right grip opens viewer small');
        equal(await page.locator('body').getAttribute('data-pane-view'),'both','right tap stays split');
        equal(await page.evaluate(()=>history.length),history,'tap does not navigate');
        await fold(page,'left');await page.locator('#pane-edge').focus();await page.keyboard.press('Enter');await settle(page);
        equal(await ratio(page),20,'keyboard opens same minimum size');
        await fold(page,'right');await page.locator('#pane-edge').focus();await page.keyboard.press('Space');await settle(page);
        equal(await ratio(page),80,'Space opens opposite side');
        equal(await viewer.evaluate(()=>JSON.stringify(getCollectionDataForExport())),replay,'all changes preserve replay');
        await context.close();
    }
    console.log(JSON.stringify({passed:true,checks}));
}finally{await browser.close();}})().catch(error=>{console.error(error);process.exitCode=1;});
