/* Optional motion must preserve settled geometry and disappear completely
   when its URL flag is removed or the OS requests reduced motion. */
const assert=require('node:assert/strict');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const base=process.argv[2]||'http://127.0.0.1:8765/';
let checks=0;
async function settled(frame) {
    await frame.evaluate(async()=>{
        await document.fonts.ready;
        await Promise.all(document.getAnimations().filter(a=>Number.isFinite(a.effect?.getComputedTiming().endTime)).map(a=>a.finished.catch(()=>{})));
        await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));
    });
}
async function geometry(frame) {
    await settled(frame);
    return frame.evaluate(()=>({
        board:document.getElementById('field-editor-canvas-p1').toDataURL(),
        controls:[...document.querySelectorAll('#editor-container, #editor-container button, #editor-container .mino-icon, #field-editor-canvas-p1')]
            .filter(el=>el.offsetWidth&&el.offsetHeight).map(el=>{
                const r=el.getBoundingClientRect(),s=getComputedStyle(el);
                return {id:el.id,rect:[r.x,r.y,r.width,r.height],color:s.color,background:s.backgroundColor,font:s.font};
            })
    }));
}
(async()=>{
    const browser=await chromium.launch({headless:true});
    try {
        for(const width of [1280,390]) for(const theme of ['light','dark']) {
            let baseline;
            for(const enabled of [false,true]) {
                const context=await browser.newContext({viewport:{width,height:800},serviceWorkers:'block'});
                await context.addInitScript(theme=>localStorage.setItem('lab-appearance-mode',theme),theme);
                const page=await context.newPage(),errors=[];
                page.on('pageerror',error=>errors.push(error.message));
                await page.goto(new URL(enabled?'?fresh=1&motion=1':'?fresh=1',base).href,{waitUntil:'networkidle'});
                const sim=page.frame({url:/index\.html\?.*workspace=1/});
                const viewer=page.frame({url:/\/F\/index\.html\?workspace=1/});
                const measurement=await geometry(sim);
                if(enabled) {assert.deepEqual(measurement,baseline);checks++;}
                else baseline=measurement;
                assert.equal(await sim.evaluate(()=>document.documentElement.dataset.uiMotion),enabled?'on':undefined);checks++;
                assert.equal(await viewer.evaluate(()=>document.documentElement.dataset.uiMotion),enabled?'on':undefined);checks++;
                // Observe native Web Animations at creation without slowing the
                // UI: the recording also survives a heavily loaded CI runner.
                await sim.evaluate(()=>{
                    window.motionEvidence=[];
                    const animate=Element.prototype.animate;
                    Element.prototype.animate=function(frames,options){
                        window.motionEvidence.push({id:this.id,className:this.className,frames,duration:options.duration});
                        return animate.call(this,frames,options);
                    };
                });
                await sim.locator('#settingsBtn').click();
                await settled(sim);
                const evidence=await sim.evaluate(()=>window.motionEvidence);
                assert.equal(evidence.length,enabled?1:0);checks++;
                if(enabled) {assert.equal(evidence[0].duration,180);assert.equal(evidence[0].frames[1].opacity,1);checks+=2;}
                await sim.locator('#settings-close').click();
                await sim.locator('#shareBtn').click();
                assert.ok(!await sim.locator('#share-link-input').inputValue().then(text=>text.includes('motion=')));checks++;
                await sim.locator('#share-close').click();
                if(enabled) {
                    await page.emulateMedia({reducedMotion:'reduce'});
                    await sim.waitForFunction(()=>document.documentElement.dataset.uiMotion==='off');checks++;
                    await sim.evaluate(()=>{window.motionEvidence=[];});
                    await sim.locator('#settingsBtn').click();
                    await settled(sim);
                    assert.equal(await sim.evaluate(()=>window.motionEvidence.length),0);checks++;
                }
                assert.deepEqual(errors,[]);checks++;
                await context.close();
            }
        }
        console.log(JSON.stringify({passed:true,checks,base}));
    } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
