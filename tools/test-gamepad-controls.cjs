/* Real settings -> saved bindings -> gameplay, with browser Gamepad API fixtures. */
const assert = require('node:assert/strict');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:8767/';
let checks = 0;
const eq = (a, b, message) => { assert.deepEqual(a, b, message); checks++; };
(async () => {
    const browser = await chromium.launch({headless:true});
    try {
        const context = await browser.newContext({serviceWorkers:'block',viewport:{width:1360,height:900}});
        await context.addInitScript(() => {
            window.testPads = [];
            Object.defineProperty(navigator, 'getGamepads', {configurable:true,value:() => window.testPads});
            if (!sessionStorage.getItem('gamepad-fixture')) {
                localStorage.setItem('tetrisKeyBindings', JSON.stringify({p1:{
                    left:{type:'pad_button',value:14,label:'Pad2-Btn14'},
                    rotateCW:{type:'pad_button',value:0,label:'Pad2-Btn0'},
                    hardDrop:{type:'pad_button',value:12,label:'Pad2-Btn12'}
                }}));
                sessionStorage.setItem('gamepad-fixture','1');
            }
        });
        const page = await context.newPage(), errors = [];
        page.on('pageerror', e => errors.push(e.message));
        await page.goto(base, {waitUntil:'networkidle'});
        let sim = page.frame({url:/index\.html\?.*workspace=1/});
        const pads = async values => sim.evaluate(values => {
            window.testPads = values.map((value,index) => value ? {
                index,id:value.id||'Test Controller',connected:true,
                buttons:Array.from({length:18},(_,button) => ({pressed:(value.buttons||[]).includes(button)})),
                axes:value.axes || [0,0]
            } : null);
            pollGamepads();
        }, values);
        await sim.locator('#mode-1p').click();
        await sim.locator('#p1-ai-toggle').uncheck();
        await sim.evaluate(() => { gameSettings.dropInterval = 9999999; editorData.p1.nextQueue = [...'TILJSZOTILJSZOTILJSZO']; });
        await pads([null,null,{}]);
        await sim.locator('#startGameBtn').click();
        await sim.waitForFunction(() => gameState === 'PLAYING' && !players[0].isSpawning);
        const rotation = await sim.evaluate(() => players[0].player.rotation);
        await pads([null,null,{buttons:[0]}]);
        eq(await sim.evaluate(() => players[0].player.rotation), (rotation+1)%4, 'saved Pad2 rotates P1');
        await pads([null,null,{buttons:[0]}]);
        eq(await sim.evaluate(() => players[0].player.rotation), (rotation+1)%4, 'held rotation does not retrigger');
        await pads([null,null,{buttons:[14]}]);
        eq(await sim.evaluate(() => players[0].isActionPressed('left')),true,'DAS reads same assigned pad');
        await pads([]);
        eq(await sim.evaluate(() => players[0].isActionPressed('left')),false,'shortened disconnected list releases input');
        await pads([null,{}]);
        await pads([null,{buttons:[0]}]);
        eq(await sim.evaluate(() => players[0].player.rotation),(rotation+2)%4,'legacy profile reconnects at a new index');
        await pads([null,{}]);
        await sim.locator('#gameSettingsBtn').click();
        await sim.locator('[data-tab="p1-keys"]').click();
        const binding = name => sim.locator('#p1-key-config-list .key-config-item').filter({hasText:name}).locator('button');
        const beforeSettings = await sim.evaluate(() => players[0].player.rotation);
        await pads([null,{buttons:[0]}]);
        eq(await sim.evaluate(() => players[0].player.rotation),beforeSettings,'settings do not control the live board');
        await binding('右回転').click();
        await pads([null,{buttons:[0]}]);
        eq(await binding('右回転').textContent(),'入力待機中...','held pad cannot steal a keyboard binding');
        await binding('右回転').press('k');
        eq(await binding('右回転').textContent(),'k','keyboard replaces a gamepad binding');
        for (const key of ['Space','Enter','ArrowLeft']) {
            await binding('右回転').click();
            await binding('右回転').press(key);
            eq(await sim.evaluate(() => isBindingKey),false,key+' finishes recording instead of clicking the control again');
        }
        await binding('右回転').click();
        await sim.evaluate(() => document.activeElement.dispatchEvent(new KeyboardEvent('keydown',{key:'Process',code:'KeyK',isComposing:true,bubbles:true,cancelable:true})));
        eq(await sim.evaluate(() => keyBindings.p1.rotateCW.code),'KeyK','IME records physical key');
        await binding('ホールド').click();
        await pads([null,{}]); await pads([null,{buttons:[7]}]);
        eq(await sim.evaluate(() => [keyBindings.p1.hold.padIndex,keyBindings.p1.hold.padId]),[1,'Test Controller'],'new binding keeps device identity');
        await pads([null,{}]);
        await sim.locator('#settings-close').click();
        await sim.locator('#game-controls').press('k');
        eq(await sim.evaluate(() => players[0].player.rotation),(beforeSettings+1)%4,'keyboard mapping controls gameplay');
        await pads([null,null,null,{buttons:[7]}]);
        eq(await sim.evaluate(() => players[0].holdPiece),'T','registered pad follows identity after reconnection');
        await pads([]);
        await sim.evaluate(() => {Object.defineProperty(navigator,'getGamepads',{configurable:true,value:()=>{throw new DOMException('blocked','SecurityError');}});pollGamepads();});
        eq(await sim.evaluate(() => Object.keys(gamepads).length),0,'unavailable API releases inputs without breaking render loop');
        await sim.evaluate(() => Object.defineProperty(navigator,'getGamepads',{configurable:true,value:()=>window.testPads}));
        await sim.locator('#backToEditorBtn').click();
        await sim.locator('#mode-2p').click(); await sim.locator('#p2-ai-toggle').uncheck();
        await sim.evaluate(() => {
            keyBindings.p1.rotateCW={type:'pad_button',value:0,padId:'First',padIndex:2,label:'Pad2-Btn0'};
            keyBindings.p2.rotateCW={type:'pad_button',value:0,padId:'Second',padIndex:4,label:'Pad4-Btn0'};
            editorData.p1.nextQueue=[...'TILJSZO'];editorData.p2.nextQueue=[...'TILJSZO'];
        });
        await pads([null,null,{id:'First'},null,{id:'Second'}]);
        await sim.locator('#startGameBtn').click();
        await pads([null,null,{id:'First',buttons:[0]},null,{id:'Second'}]);
        eq(await sim.evaluate(() => players.map(p=>p.player.rotation)),[1,0],'P1 mapping does not drive P2');
        await pads([null,null,{id:'First'},null,{id:'Second',buttons:[0]}]);
        eq(await sim.evaluate(() => players.map(p=>p.player.rotation)),[1,1],'P2 uses its own recorded controller');
        await pads([null,null,{id:'First'}]);
        eq(await sim.evaluate(() => gamepadIndexForBinding(keyBindings.p2.rotateCW,1)),null,'missing identified P2 pad does not take P1 pad');
        await sim.evaluate(() => {keyBindings.p1.softDrop={type:'pad_axis',value:'10+',padId:'First',padIndex:2};});
        await pads([null,null,{id:'First',axes:Array.from({length:11},(_,i)=>i===10?1:0)}]);
        eq(await sim.evaluate(() => players[0].isActionPressed('softDrop')),true,'multi-digit axes are parsed completely');
        eq(errors,[],'no browser runtime errors');
        console.log(JSON.stringify({passed:true,checks}));
    } finally { await browser.close(); }
})().catch(error=>{console.error(error);process.exitCode=1;});
