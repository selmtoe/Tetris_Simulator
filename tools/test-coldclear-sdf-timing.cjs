const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const source=fs.readFileSync(process.env.PLAYER_ENGINE_SOURCE||require('node:path').join(__dirname,'../simulator/app/player-engine.js'),'utf8');
const timeouts=[],messages=[];
const context=vm.createContext({performance:{now:()=>0},setTimeout:(f,ms)=>{timeouts.push(ms);queueMicrotask(f);},console,BOARD_WIDTH:10,BOARD_HEIGHT:40,gameState:'PLAYING',gameSettings:{aiMoveDelay:50,aiSdfDelay:30,debugEnabled:false},document:{getElementById:()=>null}});
vm.runInContext(source+'\nthis.PlayerClass=Player;',context);
async function run(extra={}){
 timeouts.length=messages.length=0;
 const player=Object.create(context.PlayerClass.prototype);
 Object.assign(player,{aiModel:'cold-clear',gameOver:false,isAiThinking:true,aiRequestId:1,canHold:true,player:{pieceType:'T',x:4,y:20,rotation:0},board:Array.from({length:40},()=>Array(10).fill(null)),aiWorker:{postMessage:m=>messages.push(m)},findShortestPath_forAI:()=>['↓','↓','R','↑'],getShape:()=>[],getGhostY:()=>39,tryRotate_forAI:()=>({x:4,y:22,r:1}),checkCollision:()=>false,lockPiece(){this.locked=true;}});
 await player.executeAiMove({requestId:1,piece:'T',x:4,y:39,rotation:1,controllerInputs:4,...extra});
 assert.equal(player.locked,true);assert.equal(messages.at(-1).type,'commit');return [...timeouts];
}
(async()=>{
 assert.deepEqual(await run(),[30,30,50,50],'Cold Clear uses SDF30 without adding 40ms fake waiting');
 assert.deepEqual(await run({controllerInputs:24}),[30,30,50,50],'a different Rust path length must not slow the actual browser path');
 assert.deepEqual(await run({decisionWaitMs:12}),[12,30,30,50,50],'the configured decision window is still respected');
 assert.deepEqual(await run({waitMs:25,controllerMs:200}),[65,30,30,50,50],'an explicit timed tactical forecast remains supported');
 console.log(JSON.stringify({passed:true,checks:12}));
})().catch(e=>{console.error(e);process.exitCode=1;});
