/* The extra visible row must match drawing, editing, play and image imports. */
const assert = require('node:assert/strict');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base = process.argv[2] || 'http://127.0.0.1:8765/';
let checks=0;
const equal=(a,b,message)=>{assert.deepEqual(a,b,message);checks++;};
(async()=>{
    const browser=await chromium.launch({headless:true});
    try {
        const context=await browser.newContext({viewport:{width:1280,height:800},serviceWorkers:'block'});
        const page=await context.newPage(),errors=[];
        page.on('pageerror',e=>errors.push(e.message));
        await page.goto(base,{waitUntil:'networkidle'});
        const sim=page.frame({url:/index\.html\?.*workspace=1/});
        equal(await sim.evaluate(()=>[BOARD_VISIBLE_HEIGHT,BOARD_HEIGHT,editorData.p1.viewY]),[21,40,19]);
        equal(await sim.locator('#field-editor-canvas-p1').evaluate(el=>[el.width,el.height]),[500,1050]);
        const canvas=sim.locator('#field-editor-canvas-p1');
        let bounds=await canvas.boundingBox();
        await page.mouse.click(bounds.x+bounds.width/20,bounds.y+bounds.height/42);
        await page.mouse.click(bounds.x+bounds.width*0.95,bounds.y+bounds.height*20.5/21);
        equal(await sim.evaluate(()=>[editorData.p1.board[19][0],editorData.p1.board[39][9],editorData.p1.board[20][0]]),['I','I',null],'first and last screen rows map correctly');
        const exported=await sim.evaluate(()=>getGameStateForExport());
        equal(exported.p1.b.length,400,'transport still has all 40 rows');
        await sim.locator('#startGameBtn').click();
        await sim.waitForFunction(()=>gameState==='PLAYING');
        equal(await sim.evaluate(()=>[players[0].viewY,players[0].board.length]),[19,40]);
        await sim.waitForFunction(()=>{
            const b=gameSettings.layout.p1.board;
            const pixel=ctx.getImageData((b.x+BLOCK_SIZE/2)*RESOLUTION_SCALE,(b.y+BLOCK_SIZE/2)*RESOLUTION_SCALE,1,1).data;
            return pixel[0]===78&&pixel[1]===199&&pixel[2]===205;
        });checks++;
        await sim.evaluate(()=>{gameSettings.touchControlsEnabled=true;gameSettings.touchControlType='draw';});
        const point=await sim.evaluate(()=>{
            const r=mainCanvas.getBoundingClientRect(), b=gameSettings.layout.p1.board;
            return {x:r.x+(b.x+BLOCK_SIZE*1.5)*RESOLUTION_SCALE*r.width/mainCanvas.width,
                y:r.y+(b.y+BLOCK_SIZE*0.5)*RESOLUTION_SCALE*r.height/mainCanvas.height};
        });
        await page.mouse.move(point.x,point.y);await page.mouse.down();
        equal(await sim.evaluate(()=>players[0].drawnBlocks.has('1,19')),true,'play drawing targets row 21');
        await page.mouse.up();
        await sim.locator('#backToEditorBtn').click();
        // A source image still represents 20 rows. Sample its corner markers
        // through the real importer and check their absolute board rows.
        const imported=await sim.evaluate(async()=>{
            const source=document.createElement('canvas');source.width=200;source.height=400;
            const paint=source.getContext('2d');paint.fillStyle='#000';paint.fillRect(0,0,200,400);
            paint.fillStyle='#00ffff';paint.fillRect(0,0,20,20);paint.fillRect(180,380,20,20);
            const img=new Image();img.src=source.toDataURL();await img.decode();
            scanState.image=img;scanState.targetPlayerId='p1';
            mainCanvas.width=200;mainCanvas.height=400;
            scanState.bottomLeft={x:0,y:400};scanState.topRight={x:200,y:0};
            processAndLoadBoard();
            return [editorData.p1.board[19][0],editorData.p1.board[20][0],editorData.p1.board[39][9]];
        });
        equal(imported,[null,'I','I'],'20-row image imports are bottom-aligned');
        equal(errors,[]);
        console.log(JSON.stringify({passed:true,checks}));
    } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
