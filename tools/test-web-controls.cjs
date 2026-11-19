/* Public controls, link updates, legacy import, and the overlay interaction. */
const assert = require('node:assert/strict');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:8765/';
let checks = 0;
function equal(actual, expected, message) { assert.deepEqual(actual, expected, message); checks++; }
function ok(value, message) { assert.ok(value, message); checks++; }
async function settle(frame) {
    await frame.evaluate(async () => {
        await document.fonts.ready;
        await Promise.all(document.getAnimations().filter(a => Number.isFinite(a.effect?.getComputedTiming().endTime)).map(a => a.finished.catch(() => {})));
    });
}
async function boardRect(frame) {
    return frame.locator('#viewerCanvas').evaluate(el => {
        const r = el.getBoundingClientRect(); return [r.x, r.y, r.width, r.height];
    });
}
(async () => {
    const browser = await chromium.launch({headless:true});
    try {
        const context = await browser.newContext({viewport:{width:1280,height:800}, serviceWorkers:'block'});
        const page = await context.newPage(), errors = [];
        page.on('pageerror', e => errors.push(e.message));
        page.on('response', r => { if(r.status() >= 400 && r.url().startsWith(base)) errors.push(`${r.status()} ${r.url()}`); });
        page.on('dialog', d => d.dismiss());
        await page.goto(base, {waitUntil:'networkidle'});
        const sim = page.frame({url:/index\.html\?.*workspace=1/});
        const viewer = page.frame({url:/\/F\/index\.html\?workspace=1/});
        equal(await sim.evaluate(() => Object.keys(AI_MODEL_CATALOG)), ['cold-clear'], 'one AI');
        equal(await sim.evaluate(() => gameSettings.aiThinkTime), 50, '50 ms default');
        await sim.locator('#settingsBtn').click();
        ok(await sim.locator('[data-tab="layout-settings"]').isHidden(), 'drawing is debug-only');
        await sim.locator('[data-tab="ai-settings"]').click();
        equal(await sim.locator('.ai-settings-model-hint').textContent(), 'Cold Clear', 'model setting');
        await sim.locator('#settings-close').click();
        for(let i=0;i<10;i++) await sim.locator('.mode-selection h2').dispatchEvent('click');
        await sim.locator('#settingsBtn').click();
        ok(await sim.locator('[data-tab="layout-settings"]').isVisible(), 'debug enables drawing');
        await sim.locator('[data-tab="layout-settings"]').click();
        ok(await sim.locator('#tab-content-layout-settings').isVisible(), 'drawing tab works');
        await sim.locator('#settings-close').click();
        await sim.locator('[data-ai-player="p1"] .ai-model-badge').click();
        equal(await sim.locator('.ai-model-option-name').allTextContents(), ['Cold Clear'], 'picker contains one model');
        await sim.locator('.ai-model-option').click();
        for(let i=0;i<10;i++) await sim.locator('.mode-selection h2').dispatchEvent('click');
        await sim.locator('#settingsBtn').click();
        ok(await sim.locator('[data-tab="layout-settings"]').isHidden(), 'drawing hides again');
        await sim.locator('#settings-close').click();

        await sim.locator('#shareBtn').click();
        equal(await sim.locator('#generate-advanced-link-btn').count(), 0, 'no regenerate button');
        await sim.locator('#advanced-link-btn').click();
        async function flags() {
            const url = new URL(await sim.locator('#share-link-input').inputValue());
            const data = JSON.parse(Buffer.from(url.hash.slice(1), 'base64').toString());
            return [!!data.ss, !!data.nh, !!data.hb];
        }
        equal(await flags(), [false,false,false]);
        await sim.locator('#start-sim-checkbox').check(); equal(await flags(), [true,false,false]);
        await sim.locator('#no-hold-checkbox').check(); equal(await flags(), [true,true,false]);
        await sim.locator('#hide-back-btn-checkbox').check(); equal(await flags(), [true,true,true]);
        await sim.locator('#start-sim-checkbox').uncheck(); equal(await flags(), [false,true,false]);
        ok(await sim.locator('#hide-back-btn-checkbox').isDisabled(), 'dependent option disabled');
        await sim.locator('#share-close').click();
        await sim.locator('#shareBtn').click(); equal(await flags(), [false,true,false], 'reopening preserves selected options');
        await sim.locator('#workspace-open-replay').click();
        const empty = () => Array.from({length:40}, () => Array(10).fill(null));
        const before={p1:{board:empty(),next:'OIJSZTL',hold:''}};
        const after=structuredClone(before);
        for(const [x,y] of [[4,38],[5,38],[4,39],[5,39]]) after.p1.board[y][x]='O';
        after.p1.next='IJSZTLO';
        const fixture={v:3,m:'1P',cases:[{name:'互換リプレイ',kind:'snapshot',gameMode:'1P',pages:[before,after]}]};
        await page.locator('#replay-file').setInputFiles({name:'old.tetrisevent.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify(fixture))});
        await page.waitForFunction(() => document.body.dataset.mode === 'viewer');

        for(const width of [1280,390,320,1280]) {
            await page.setViewportSize({width,height:800});
            await page.mouse.move(0,0);
            const persistent = width < 400; // The short, centered canvas leaves space above it here.
            await viewer.waitForFunction(expected=>document.getElementById('viewer-controls').dataset.presentation===expected, persistent?'persistent':'compact');
            await settle(viewer);
            equal(await viewer.locator('#viewer-page-indicator').getAttribute('aria-expanded'), String(persistent));
            equal(await viewer.locator('#viewer-share-btn').isVisible(), persistent, 'auto presentation');
            const rect=await boardRect(viewer);
            if (!persistent) await viewer.locator('#viewer-page-indicator').hover();
            await settle(viewer);
            equal(await boardRect(viewer), rect, 'expansion does not move the board');
            const rows=await viewer.locator('.viewer-top-row button:visible').evaluateAll(els => els.map(el => {
                const r=el.getBoundingClientRect(); return {y:r.y,left:r.left,right:r.right,text:el.textContent};
            }));
            equal(rows.map(r=>r.text), ['シミュレータ','共有','出力','AI採点']);
            ok(rows.every(r => r.y === rows[0].y), 'actions stay on one row');
            ok(rows[0].left >= 0 && rows.at(-1).right <= width, 'all four actions fit');
            const pageBottom=await viewer.locator('#viewer-page-indicator').evaluate(el=>el.getBoundingClientRect().bottom);
            ok(rows[0].y > pageBottom, 'page has its own row');
            if (persistent) {
                const menuBottom=await viewer.locator('.viewer-controls-shell').evaluate(el=>el.getBoundingClientRect().bottom);
                ok(menuBottom + 8 <= rect[1], 'persistent menu clears the entire canvas');
                ok(await viewer.locator('#viewer-page-indicator').isDisabled(), 'persistent page is a label');
                continue;
            }
            await page.mouse.move(0,0);
            await viewer.waitForFunction(()=>document.getElementById('viewer-controls-panel').inert);
            await settle(viewer);
            equal(await boardRect(viewer), rect, 'collapse does not move the board');
        }
        await viewer.locator('#viewer-page-indicator').press('Enter');
        equal(await viewer.locator('#viewer-page-indicator').getAttribute('aria-expanded'),'true','keyboard opens');
        await viewer.locator('#viewer-page-indicator').press('ArrowDown');
        equal(await viewer.evaluate(()=>document.activeElement.id),'viewer-simulator-btn','keyboard enters actions');
        await page.keyboard.press('Escape');
        equal(await viewer.locator('#viewer-page-indicator').getAttribute('aria-expanded'),'false','Escape closes');
        equal(await viewer.evaluate(()=>currentPageIndex),0,'menu keys do not seek');
        await page.emulateMedia({reducedMotion:'reduce'});
        await viewer.locator('#viewer-page-indicator').click();
        equal(await viewer.locator('.viewer-controls-shell').evaluate(el=>getComputedStyle(el).transitionDuration),'0s','reduced motion');
        await viewer.locator('#viewer-share-btn').click();
        equal(await viewer.locator('#export-event-file-btn').count(),0,'legacy files are read-only');
        await viewer.waitForFunction(()=>document.getElementById('share-link-input').value.includes('#'));
        const link=await viewer.locator('#share-link-input').inputValue();
        ok(link.includes('#'), 'share is a link');
        const linked=await context.newPage();
        await linked.goto(link,{waitUntil:'networkidle'});
        const linkedViewer=linked.frame({url:/\/F\/index\.html\?workspace=1/});
        equal(await linkedViewer.evaluate(()=>fumenPages.length),2,'shared link reopens all pages');
        await linked.close();
        await viewer.locator('#share-close').click();
        await page.setViewportSize({width:1280,height:800});
        await viewer.locator('#viewer-page-indicator').click();
        await viewer.locator('#viewer-ai-score-btn').click();
        await viewer.locator('#ai-score-run').click();
        await viewer.waitForFunction(()=>document.getElementById('ai-score-status').textContent.includes('完了'),null,{timeout:30000});
        checks++;
        equal(errors,[],'no missing AI dependencies or browser errors');
        await context.close();

        const touch=await browser.newContext({viewport:{width:390,height:400},isMobile:true,hasTouch:true,serviceWorkers:'block'});
        await touch.addInitScript(()=>localStorage.setItem('tetrisGameSettings',JSON.stringify({aiThinkTime:77,aiModels:{p1:'kasane-basic',p2:'kasane-base'}})));
        const touchPage=await touch.newPage();
        await touchPage.goto(link,{waitUntil:'networkidle'});
        const touchSim=touchPage.frame({url:/index\.html\?.*workspace=1/});
        equal(await touchSim.evaluate(()=>[playerAiModelId('p1'),playerAiModelId('p2'),gameSettings.aiThinkTime]),['cold-clear','cold-clear',77],'old models fall back, custom timing preserved');
        const touchViewer=touchPage.frame({url:/\/F\/index\.html\?workspace=1/});
        await touchViewer.waitForFunction(()=>document.getElementById('viewer-controls').dataset.presentation==='compact');
        await touchViewer.locator('#viewer-page-indicator').tap();
        equal(await touchViewer.locator('#viewer-page-indicator').getAttribute('aria-expanded'),'true','tap opens');
        await touchViewer.locator('#viewer-page-indicator').tap();
        equal(await touchViewer.locator('#viewer-page-indicator').getAttribute('aria-expanded'),'false','second tap closes');
        await touchViewer.locator('#viewer-page-indicator').tap();
        await touchPage.touchscreen.tap(10,390);
        equal(await touchViewer.locator('#viewer-page-indicator').getAttribute('aria-expanded'),'false','outside tap closes');
        await touchPage.setViewportSize({width:390,height:844});
        await touchViewer.waitForFunction(()=>document.getElementById('viewer-controls').dataset.presentation==='persistent');
        equal(await touchViewer.locator('#viewer-page-indicator').getAttribute('aria-expanded'),'true','resize reopens when there is room');
        await touch.close();
        console.log(JSON.stringify({passed:true,checks}));
    } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
