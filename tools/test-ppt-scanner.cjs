// Real extension APIs in a disposable browser profile; no user's tabs or profile.
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const extension = path.resolve(__dirname, '../extensions/ppt-scanner');
const target = 'https://selmtoe.github.io/Tetris_Simulator/';
let checks = 0;
function equal(actual, expected, message) { assert.deepEqual(actual, expected, message); checks++; }
const receiverHTML = '<script>window.received=[];window.receiveExtensionImage=async image=>{window.received.push(image);};</script>';
(async () => {
    const profile = await fs.mkdtemp(path.join(os.tmpdir(), 'ppt-scanner-test-'));
    let context;
    try {
        context = await chromium.launchPersistentContext(profile, {
            channel: 'chromium', headless: true,
            args: [`--disable-extensions-except=${extension}`, `--load-extension=${extension}`],
            viewport: {width: 1280, height: 800}
        });
        context.setDefaultTimeout(15000);
        context.on('console', message => { if (message.type() === 'error') console.log('Browser:', message.text()); });
        console.log('Extension browser launched');
        const worker = context.serviceWorkers()[0] || await context.waitForEvent('serviceworker');
        console.log('Extension worker ready');
        // Playwright cannot route the first navigation of a tab created by
        // an extension before it attaches. Pause only that test bootstrap;
        // subsequent navigation and script injection still use real APIs.
        await worker.evaluate(() => {
            const create = chrome.tabs.create.bind(chrome.tabs);
            globalThis.restoreTabCreate = () => { chrome.tabs.create = create; };
            chrome.tabs.create = async options => {
                const tab = await create({...options, url:'about:blank'});
                await new Promise(resolve => setTimeout(resolve, 250));
                return chrome.tabs.update(tab.id, {url:options.url});
            };
        });
        let deliveryMode = 'hub';
        await context.route(target + '**', async route => {
            const url = new URL(route.request().url());
            let body;
            if (url.pathname.includes('/hub/')) {
                body = deliveryMode === 'standalone' ? receiverHTML : `<iframe id="iframe-sim" src="../index.html?workspace=1"></iframe><iframe src="https://unrelated.test/"></iframe>`;
            } else {
                if (deliveryMode === 'delayed') await new Promise(resolve => setTimeout(resolve, 1200));
                body = receiverHTML;
            }
            await route.fulfill({contentType: 'text/html', body});
        });
        await context.route('https://unrelated.test/**', route => route.fulfill({body:'unrelated frame'}));
        await context.route('https://www.youtube.com/**', route => route.fulfill({contentType:'text/html', body:'<body></body>'}));
        await context.route('http://localhost:19999/**', route => route.fulfill({contentType:'text/html', body:'<iframe src="https://www.youtube.com/embed/test"></iframe>'}));
        const source = await context.newPage();
        await source.goto('https://www.youtube.com/watch?v=scanner-fixture');
        const makeVideo = async frame => {
            await frame.evaluate(() => {
            const canvas = document.createElement('canvas'); canvas.width = 1920; canvas.height = 1080;
            const ctx = canvas.getContext('2d'); ctx.fillStyle = '#34bbcf'; ctx.fillRect(0, 0, 1920, 1080);
            const video = document.createElement('video'); video.muted = true; video.style.width = '640px';
            video.srcObject = canvas.captureStream(10); document.body.append(video); video.play();
            setInterval(() => ctx.fillRect(0, 0, 1920, 1080), 100);
            });
            await frame.waitForFunction(() => document.querySelector('video').readyState >= 2);
        };
        const sourceTabId = await worker.evaluate(async () => (await chrome.tabs.query({url:'https://www.youtube.com/watch*'}))[0].id);
        const noVideo = await worker.evaluate(id => startCapture(id), sourceTabId);
        console.log('Missing-video check completed');
        equal(noVideo.ok, false, 'missing video is an error');
        equal(await source.locator('#ppt-scanner-notice').textContent(), noVideo.error, 'error is visible on source page');
        equal(context.pages().filter(p => p.url().startsWith(target)).length, 0, 'no empty simulator tab on capture error');
        await source.evaluate(() => { const video = document.createElement('video'); video.style.cssText = 'width:640px;height:360px'; document.body.append(video); });
        const notReady = await worker.evaluate(id => startCapture(id), sourceTabId);
        equal(notReady.error.includes('一度再生'), true, 'unready video gives actionable feedback');
        await source.locator('video').evaluate(el => el.remove());
        await makeVideo(source);
        console.log('Video fixture ready');
        for (const mode of ['hub', 'standalone', 'delayed']) {
            console.log('Checking delivery:', mode);
            deliveryMode = mode;
            const before = context.pages().length;
            const results = await worker.evaluate(async id => Promise.all([startCapture(id), startCapture(id)]), sourceTabId);
            if (!results[0].ok) console.log('Delivery failure:', results[0], context.pages().map(p => ({url:p.url(), frames:p.frames().map(f=>f.url())})));
            equal(results[0].ok, true, mode + ': delivery succeeds');
            equal(results[0].targetTabId, results[1].targetTabId, mode + ': repeated clicks share the same operation');
            equal(context.pages().length, before + 1, mode + ': one new tab');
            const page = context.pages().findLast(p => p.url().startsWith(target));
            const receiver = mode === 'standalone' ? page.mainFrame() : page.frame({url:/index\.html\?workspace/});
            const count = await receiver.evaluate(() => received.length);
            equal(count, 1, mode + ': image received once');
            equal(await receiver.evaluate(() => received[0].startsWith('data:image/jpeg;base64,')), true, 'real video JPEG delivered');
            equal(await page.locator('#ppt-scanner-notice').isVisible(), true, 'delivery notification visible');
            await page.close();
        }
        // The Lab bridge asks the worker to capture all frames exactly once.
        deliveryMode = 'hub';
        const lab = await context.newPage(); await lab.goto('http://localhost:19999/');
        const embedded = lab.frame({url:/youtube\.com\/embed/}); await makeVideo(embedded);
        await lab.evaluate(() => {
            window.scanResult = null;
            window.addEventListener('message', event => { if(event.data?.type === 'PPT_SCAN_RESULT') window.scanResult = event.data; });
            window.postMessage({type:'PPT_TRIGGER_SCAN'}, location.origin);
        });
        await lab.waitForFunction(() => window.scanResult !== null);
        equal(await lab.evaluate(() => scanResult.ok), true, 'Lab embed bridge works');
        equal(context.pages().filter(p => p.url().startsWith(target)).length, 1, 'Lab embed opens only one simulator');
        if (process.env.PPT_SCANNER_FIXTURE) {
            // Optional full integration: a local video frame through the actual
            // public Hub and its real ONNX model. The image stays in the browser.
            for (const page of context.pages().filter(p => p.url().startsWith(target))) await page.close();
            await context.unroute(target + '**');
            await worker.evaluate(() => restoreTabCreate());
            await context.addInitScript(() => window.addEventListener('load', () => {
                if (typeof runHighPrecisionAnalysis !== 'function') return;
                const analyze = runHighPrecisionAnalysis;
                runHighPrecisionAnalysis = async (...args) => {
                    await analyze(...args);
                    document.documentElement.dataset.scannerDone = 'true';
                };
            }));
            const imageData = 'data:image/png;base64,' + (await fs.readFile(process.env.PPT_SCANNER_FIXTURE)).toString('base64');
            await source.locator('video').evaluate(el => { el.srcObject.getTracks().forEach(t=>t.stop());el.remove(); });
            await source.evaluate(async imageData => {
                const image = new Image(); image.src = imageData; await image.decode();
                const canvas = document.createElement('canvas'); canvas.width=image.width; canvas.height=image.height;
                const ctx = canvas.getContext('2d'); ctx.drawImage(image,0,0);
                const video = document.createElement('video'); video.style.width='640px'; video.muted=true;
                video.srcObject=canvas.captureStream(10);document.body.append(video);video.play();
                setInterval(()=>ctx.drawImage(image,0,0),100);
            }, imageData);
            await source.waitForFunction(()=>document.querySelector('video').readyState>=2);
            context.on('page', page => page.on('dialog', dialog => { console.log('App dialog:',dialog.message());dialog.dismiss(); }));
            const imported = await worker.evaluate(id => startCapture(id), sourceTabId);
            equal(imported.ok,true,'live production Hub accepts captured video');
            const page = context.pages().find(p => p.url().startsWith(target));
            const sim = page.frame({url:/index\.html\?.*workspace=1/});
            assert.ok(sim,'live simulator frame exists');checks++;
            await sim.waitForFunction(()=>document.documentElement.dataset.scannerDone==='true',null,{timeout:60000});
            const recognized=await sim.evaluate(()=>({mode:gameMode,p1:editorData.p1.board.flat().filter(Boolean).length,p2:editorData.p2.board.flat().filter(Boolean).length,p1Hold:editorData.p1.hold,p1Next:editorData.p1.nextQueue,p2Next:editorData.p2.nextQueue}));
            console.log('Live recognition:',JSON.stringify(recognized));
            equal(recognized.mode,'2P','real recognition applies both-player mode');
            assert.ok(recognized.p1>0,'real ONNX recognition produces board cells');checks++;
        }
        // A stale content script is unnecessary for toolbar capture: a video
        // that existed before extension invocation has been used above.
        console.log(JSON.stringify({passed:true, checks}));
    } finally {
        await context?.close();
        await fs.rm(profile, {recursive:true, force:true});
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
