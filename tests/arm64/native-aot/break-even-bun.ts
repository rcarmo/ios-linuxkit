#!/usr/bin/env bun
// Evaluation only. Same work, fixed corpus and oracle as break-even.py.
import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
const [path, repeatArg, warmupArg, ...extra] = process.argv.slice(2);
if (!path || !/^\d+$/.test(repeatArg ?? '') || !/^\d+$/.test(warmupArg ?? '') || extra.length)
 throw Error('usage: break-even-bun.ts RECORDS REPEATS WARMUP');
const repeats = Number(repeatArg), warmup = Number(warmupArg);
if (!Number.isInteger(repeats) || repeats < 1 || repeats > 64 || !Number.isInteger(warmup) || warmup < 0 || warmup > 8)
 throw Error('unbounded work count');
const raw = readFileSync(path, 'utf8'), rows = JSON.parse(raw);
const expected = Array.from({length:64}, (_,i) => ({id:i,group:i%17,active:i%3!==0,
 value:(101+7919*i)%65521,label:'record-'+i.toString(16)}));
// Field order in the source JSON is not part of the semantic input contract.
if (!Array.isArray(rows) || rows.length < 64 || expected.some((e,i) =>
 Object.keys(rows[i]).length !== 5 || Object.entries(e).some(([k,v]) => rows[i][k] !== v)))
 throw Error('fixed 64-record corpus mismatch');
const corpusHash = createHash('sha256').update(JSON.stringify(expected)).digest('hex');
function work(): number {
 const chosen = JSON.parse(raw).slice(0,64).filter((r:any) => r.active)
  .sort((a:any,b:any) => a.group-b.group || a.id-b.id);
 const serialized = JSON.stringify(chosen.map((r:any) => Object.fromEntries(Object.entries(r).sort(([a],[b]) => a<b?-1:a>b?1:0))));
 const restored = JSON.parse(serialized);
 if (restored.length !== chosen.length || restored.some((r:any,i:number) =>
  Object.entries(chosen[i]).some(([k,v]) => r[k] !== v))) throw Error('roundtrip mismatch');
 return chosen.reduce((sum:number,r:any) => sum+r.value*(r.group+1),0);
}
function phase(name:string, count:number):number {
 const start = process.hrtime.bigint(); let total = 0;
 for (let i=0;i<count;i++) total += work();
 const end = process.hrtime.bigint();
 console.error('BREAK_EVEN_PHASE '+JSON.stringify({schema:1,runtime:'bun',version:Bun.version,
  clock:'monotonic',phase:name,workCount:count,startNs:String(start),endNs:String(end),seconds:Number(end-start)/1e9}));
 return total;
}
phase('warmup',warmup);
const total = phase('steady-state',repeats);
console.log(`BREAK_EVEN_JSON 64 42 ${repeats} ${total} ${corpusHash}`);
