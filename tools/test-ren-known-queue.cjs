const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.join(__dirname,'..'),context=vm.createContext({window:{},document:{addEventListener(){}}});
vm.runInContext(fs.readFileSync(path.join(root,'F/app/87-analysis.js'),'utf8'),context);
const extend=context.window.PositionAnalysis.extendKnownQueue;
const state=(current,queue,hold=null,operation=current)=>({current,queue:[...queue],hold,operation:operation?{type:operation}:null});
let checks=0;const eq=(a,b,label)=>{assert.deepEqual(JSON.parse(JSON.stringify(a)),b,label);checks++;};
const initial=state('T','IOLSJ'),rolling=[state('I','OLSJZ'),state('O','LSJZT'),state('L','SJZTI')];
eq(extend(initial,rolling),[...'IOLSJZTI'],'rolling NEXT keeps pieces beyond the initial visible queue');
eq(extend(state('T','IOLSJ',null,null),[state('T','IOLSJ',null,null),...rolling]),[...'IOLSJ'],'unrelated skipped locks cannot invent a stream');
eq(extend(state('T','IOLSJ',null,null),[state('T','IOLSJ'),...rolling]),[...'IOLSJZTI'],'merged P2-only pages do not consume P1 NEXT');
eq(extend(state('T','IOLSJ',null,'I'),[state('O','LSJZT','T')]),[...'IOLSJZT'],'empty HOLD before lock consumes two previews');
eq(extend(state('T','IOLSJ','Z','Z'),[state('I','OLSJZ','T')]),[...'IOLSJZ'],'occupied HOLD before lock preserves the current in HOLD');
eq(extend(state('T','IOLSJ'),[state('O','LSJZT','I')]),[...'IOLSJZT'],'resolved empty HOLD after a lock consumes the additional preview');
eq(extend(state('T','IOLSJ','Z'),[state('Z','OLSJZ','I')]),[...'IOLSJZ'],'resolved occupied HOLD after a lock consumes only one preview');
eq(extend(initial,[state('I','OZZZZ')]),[...'IOLSJ'],'contradictory recognition stops extension');
eq(extend(state('O','OOOOO'),Array.from({length:120},()=>state('O','OOOOO'))).length,125,'repeated pieces are resolved from events without a ten-piece cap');
eq(extend(initial,[], 'TIOLSJZTIOTSZJL'),[...'IOLSJZTIOTSZJL'],'unambiguous full recording supplies the remaining generator stream');
eq(extend(state('O','OOOOO'),[], 'OOOOOOOOOOOO'),[...'OOOOO'],'ambiguous full stream is not positioned by guesswork');
const repeated={...state('T','IOLSJ'),page:{board:[['T']]}};
eq(extend(repeated,[{...repeated},...rolling]),[...'IOLSJZTI'],'repeated pending operations on merged pages are consumed once');

const workerContext=vm.createContext({performance,console,setTimeout,TextEncoder,Uint16Array,Int8Array,Uint8Array});workerContext.self=workerContext;
workerContext.importScripts=(...files)=>files.forEach(file=>vm.runInContext(fs.readFileSync(path.join(root,'simulator/workers',file),'utf8'),workerContext));
workerContext.importScripts('route-search-worker.js');
const solve=queue=>{workerContext.data={board:Array.from({length:40},(_,y)=>Array.from({length:10},(_,x)=>y>=16&&x!==4&&x!==5?'G':null)),currentPiece:'O',nextQueue:queue,canHold:true};return vm.runInContext('(()=>{const i=renSearch(routeSnapshot(data));let s;do{s=i.next();}while(!s.done);return s.value;})()',workerContext);};
eq(solve([...'OOOOO']).ren,5,'old five-preview query stops at five REN');
eq(solve(extend(state('O','OOOOO'),Array.from({length:15},()=>state('O','OOOOO')))).ren,11,'the same board reaches eleven REN when all recorded previews are supplied');

// Real video recovery exports can be passed separately; never write to Lab.
(async()=>{
 if(process.argv[2]){
  const records=(await(await fetch(process.argv[2])).json()).records;
  const codec=require(path.join(root,'shared/tetris-event-codec.js'));
  const examples=[];
  for(const match of records.filter(r=>r.kind==='match'&&r.simulator?.combined).slice(0,3)){
   const hash=new URL(match.simulator.combined).hash.slice(1);let data=JSON.parse(Buffer.from(decodeURIComponent(hash),'base64').toString());
   if(codec.isEventReplay(data))data=codec.decodeCollection(data);
   const c=data.cases?.[data.currentCase||0];if(!c)continue;
   for(const pid of ['p1','p2']){
    const states=c.pages.map(p=>p[pid]).filter(Boolean).map(p=>{const s={...state(p.active,p.next,p.hold,p.operation?.type||null),page:p};if(!s.current)s.current=s.queue.shift();return s;});
    const count=extend(states[0],states.slice(1),c.initial?.[pid]?.sequence).length;examples.push({match:match.id,player:pid,pages:states.length,visible:states[0].queue.length,known:count});
   }
  }
  console.log(JSON.stringify({realVideoExamples:examples}));
 }
 console.log(JSON.stringify({passed:true,checks}));
})().catch(e=>{console.error(e);process.exitCode=1;});
