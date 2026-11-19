/* Reproduce an installed native cache, then upgrade without clearing storage.
   Runs only on an isolated ephemeral HTTP server and browser profile. */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const http=require('node:http');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const root=path.resolve(__dirname,'..'),built=path.join(root,'dist/pages');
let legacy=true,checks=0;
const oldAssets=['simulator/app/runtime-config.js?v=app-v21','simulator/app/editor.js','simulator/app/player-engine.js?v=app-v27','F/app/10-state.js'];
const oldPaths=new Set(oldAssets.map(value=>value.split('?')[0]));
const oldWorker=`
const OLD='tetris-simulator-legacy-ui';
self.addEventListener('install',event=>event.waitUntil(caches.open(OLD).then(cache=>cache.addAll(${JSON.stringify(oldAssets)})).then(()=>self.skipWaiting())));
self.addEventListener('activate',event=>event.waitUntil(self.clients.claim()));
self.addEventListener('fetch',event=>event.respondWith(caches.match(event.request).then(hit=>hit||fetch(event.request))));`;
const requests=[];
const types={'.html':'text/html','.js':'text/javascript','.css':'text/css','.json':'application/json','.wasm':'application/wasm','.png':'image/png'};
const server=http.createServer((req,res)=>{
    const url=new URL(req.url,'http://localhost');
    let relative=decodeURIComponent(url.pathname).replace(/^\//,'');
    if(!relative||relative.endsWith('/')) relative+='index.html';
    requests.push({legacy,url:req.url});
    if(relative==='sw.js'&&legacy) {res.writeHead(200,{'Content-Type':'text/javascript','Cache-Control':'no-store'});res.end(oldWorker);return;}
    const file=path.resolve(legacy&&oldPaths.has(relative)?root:built,relative);
    if(!file.startsWith(root+path.sep)||!fs.existsSync(file)||!fs.statSync(file).isFile()) {res.writeHead(404);res.end();return;}
    let data=fs.readFileSync(file);
    if(legacy&&relative.endsWith('.html')) data=Buffer.from(data.toString().replace(/&amp;build=[a-f0-9]+|\?build=[a-f0-9]+/g,''));
    res.writeHead(200,{'Content-Type':types[path.extname(file)]||'application/octet-stream','Cache-Control':'no-store'});res.end(data);
});
(async()=>{
    await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
    const base=`http://127.0.0.1:${server.address().port}/`;
    const browser=await chromium.launch({headless:true});
    try {
        const context=await browser.newContext({viewport:{width:1000,height:800}});
        const page=await context.newPage();
        const errors=[];page.on('pageerror',error=>errors.push(error.message));
        await page.goto(base+'index.html?standalone=1',{waitUntil:'networkidle'});
        await page.evaluate(()=>navigator.serviceWorker.ready);
        await page.waitForFunction(()=>navigator.serviceWorker.controller);
        const palette=()=>document.querySelector('#p1-palette .color-swatch:nth-child(2)').style.backgroundColor;
        assert.equal(await page.evaluate(palette),'rgb(0, 240, 240)');checks++;
        // A preference survives the upgrade; no clear-site-data workaround.
        await page.evaluate(()=>localStorage.setItem('lab-appearance-mode','dark'));
        legacy=false;
        await page.goto(base+'hub/?entry=simulator',{waitUntil:'networkidle'});
        const sim=page.frame({url:/index\.html\?.*workspace=1/});
        assert.equal(await sim.evaluate(palette),'rgb(78, 199, 205)');checks++;
        assert.equal(await sim.evaluate(()=>document.documentElement.dataset.labTheme),'dark');checks++;
        const swScripts=await page.evaluate(async()=>{
            const regs=await navigator.serviceWorker.getRegistrations();
            await Promise.all(regs.map(reg=>reg.update()));
            return regs.map(reg=>reg.scope);
        });
        assert.ok(swScripts.some(scope=>scope===base));checks++;
        assert.ok(swScripts.some(scope=>scope===base+'hub/'));checks++;
        const stale=await page.evaluate(async()=>{
            const cache=await caches.open('tetris-simulator-legacy-ui');
            return (await cache.match(new URL('../simulator/app/runtime-config.js?v=app-v21',location.href)))?.text();
        });
        assert.ok(stale?.includes("'I': '#00f0f0'")||stale?.includes('#00f0f0'));checks++;
        assert.ok(requests.some(r=>!r.legacy&&r.url.includes('runtime-config.js?v=app-v21&build=')));checks++;
        await page.reload({waitUntil:'networkidle'});
        assert.equal(await page.frame({url:/index\.html\?.*workspace=1/}).evaluate(palette),'rgb(78, 199, 205)');checks++;
        assert.equal(await page.evaluate(()=>localStorage.getItem('lab-appearance-mode')),'dark');checks++;
        assert.deepEqual(errors,[]);checks++;
        console.log(JSON.stringify({passed:true,checks,legacyCacheRetained:true}));
    } finally {await browser.close();server.close();}
})().catch(error=>{console.error(error);server.close();process.exitCode=1;});
