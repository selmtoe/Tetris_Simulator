/* Compare pruning/memoization to an independent breadth-first enumeration. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const workerRoot = path.join(__dirname, '../simulator/workers');
const context = vm.createContext({performance, console, setTimeout, TextEncoder, Uint16Array, Int8Array, Uint8Array});
context.self = context;
context.importScripts = (...files) => files.forEach(file => vm.runInContext(fs.readFileSync(path.join(workerRoot,file.split('?')[0]),'utf8'),context));
context.importScripts('route-search-worker.js');
const {Board,findMoves} = context.ColdClearSimulatorCore;
const solve = data => { context.input=data; return vm.runInContext('(() => { const iterator=renSearch(routeSnapshot(input)); let step; do { step=iterator.next(); } while (!step.done); return step.value; })()',context); };
function breadthFirst(data) {
    const sequence=[data.currentPiece,...data.nextQueue];
    let layer=[{board:Board.fromSimulator(data.board),index:0,hold:data.holdPiece,canHold:data.canHold}], depth=0;
    while(layer.length) {
        const next=[];
        for(const state of layer) {
            if(!sequence[state.index])continue;
            const options=[{piece:sequence[state.index],index:state.index+1,hold:state.hold}];
            if(state.canHold&&!data.holdDisabled) {
                if(state.hold)options.push({piece:state.hold,index:state.index+1,hold:sequence[state.index]});
                else if(sequence[state.index+1])options.push({piece:sequence[state.index+1],index:state.index+2,hold:sequence[state.index]});
            }
            for(const option of options)for(const move of findMoves(state.board,option.piece)) {
                const placed=state.board.lock(move,false,0);
                if(placed.lock.lines&&!placed.lock.lockedOut)next.push({board:placed.board,index:option.index,hold:option.hold,canHold:!data.holdDisabled});
            }
        }
        if(!next.length)break;
        depth++;layer=next;
    }
    return depth;
}
let checks=0;
let seed=1347;
const random=n=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed%n;};
const pieces=[...'IOTSZJL'];
for(let fixture=0;fixture<48;fixture++) {
    const board=Array.from({length:40},()=>Array(10).fill(null));
    const left=random(7), width=1+random(4),height=2+random(4);
    for(let y=40-height;y<40;y++)for(let x=0;x<10;x++)if(x<left||x>=left+width)board[y][x]='G';
    const data={board,currentPiece:pieces[random(7)],nextQueue:[pieces[random(7)],pieces[random(7)]],holdPiece:fixture%2?pieces[random(7)]:null,canHold:fixture%3!==0,holdDisabled:fixture%7===0,ren:-1};
    const actual=solve(data);
    assert.equal(actual.complete,true);
    assert.equal(actual.depth,breadthFirst(data),`fixture ${fixture} must match unpruned enumeration`);
    checks+=2;
}
// Known long queues are not truncated to the old nine-piece limit.
const board=Array.from({length:40},(_,y)=>Array.from({length:10},(_,x)=>y>=20&&x>=2?'G':null));
const long={board,currentPiece:'O',nextQueue:Array(30).fill('O'),holdPiece:null,canHold:true,ren:4};
const answer=solve(long);
assert.equal(answer.depth,10);assert.equal(answer.ren,14);assert.equal(answer.knownCount,31);checks+=3;
const fourWide=Array.from({length:40},(_,y)=>Array.from({length:10},(_,x)=>y>=20&&x<6?'G':null));
fourWide[39][6]=fourWide[39][7]=fourWide[39][8]='G';
const sequence=[...'TILJSZOTILJSZOTILJSZOTILJSZO'];
const extended=solve({board:fourWide,currentPiece:sequence[0],nextQueue:sequence.slice(1),holdPiece:null,canHold:true});
assert.equal(extended.complete,true);assert.equal(extended.depth,20);assert.equal(extended.ren,19);checks+=3;
console.log(JSON.stringify({passed:true,checks}));
