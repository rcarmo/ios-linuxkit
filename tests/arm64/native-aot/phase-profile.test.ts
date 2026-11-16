import {test,expect} from 'bun:test';
import {elfLayout,samples,phaseAt,clockOffset,snapshots,analyse,main} from './phase-profile';
function elf(){const b=Buffer.alloc(4096);Buffer.from('7f454c460201','hex').copy(b);b.writeUInt16LE(183,18);b.writeBigUInt64LE(64n,32);b.writeUInt16LE(56,54);b.writeUInt16LE(1,56);b.writeUInt32LE(1,64);b.writeUInt32LE(5,68);b.writeBigUInt64LE(4096n,96);b.writeUInt32LE(0x3babd760,0x200+76);b.writeBigUInt64LE(0x600n,0x200+80);b.writeBigUInt64LE(0x800n,0x200+88);return b;}
const syms='0000000000000200 D ish_aot_module_python\n';
function phase(name:string,start:number,end:number,count:number){return{schema:1,runtime:'python',version:'3.14.7',clock:'monotonic',phase:name,workCount:count,startNs:String(start),endNs:String(end),seconds:(end-start)/1e9};}
const phases=[phase('warmup',100,200,1),phase('steady-state',300,500,64)];
function counters(){return['warmup.before','warmup.after','steady-state.before','steady-state.after'].map((label,i)=>'AOT_PROFILE_SNAPSHOT '+JSON.stringify({schema:1,label,beforeNs:String([50,210,250,510][i]),afterNs:String([50,210,250,510][i]+1),hostNs:String([50,210,250,510][i]),state:`blocks translated or looked up: ${10+i}, AOT installs: ${i} (moved: 0, with pc-relative immediates fixed: 0, slot taken elsewhere: 0), reused: 0\n`})).join('\n');}
const stderr=phases.map(p=>'BREAK_EVEN_PHASE '+JSON.stringify(p)).join('\n')+'\n'+counters();
test('ELF module ranges use absolute fields, RX bounds and declared ABI',()=>{
 const e=elfLayout(elf(),syms);expect(e.ranges).toEqual([{name:'python',lo:1536,hi:2048}]);expect(e.fileOffsetToVA(0x680)).toBe(0x680);
 expect(e.fileOffsetToVA(5000)).toBeNull();
 const b=elf();b.writeUInt32LE(1,0x200+76);expect(()=>elfLayout(b,syms)).toThrow('ABI');
 const c=elf();c.writeBigUInt64LE(6000n,0x200+88);expect(()=>elfLayout(c,syms)).toThrow('bounds');
 expect(()=>elfLayout(Buffer.alloc(16),syms)).toThrow('ELF64');
});
test('ELF file offsets are not virtual addresses; RELATIVE relocations resolve descriptor pointers',()=>{
 const b=elf();b.writeBigUInt64LE(0x10000n,80);
 b.writeBigUInt64LE(0n,0x200+80);b.writeBigUInt64LE(0n,0x200+88);
 b.writeBigUInt64LE(0xa00n,40);b.writeUInt16LE(64,58);b.writeUInt16LE(1,60);
 b.writeUInt32LE(4,0xa00+4);b.writeBigUInt64LE(0xb00n,0xa00+24);
 b.writeBigUInt64LE(48n,0xa00+32);b.writeBigUInt64LE(24n,0xa00+56);
 for(const [i,where,value] of [[0,0x10250,0x10600],[1,0x10258,0x10800]]){
  const p=0xb00+i*24;b.writeBigUInt64LE(BigInt(where),p);b.writeBigUInt64LE(1027n,p+8);b.writeBigInt64LE(BigInt(value),p+16);
 }
 const e=elfLayout(b,'0000000000010200 D ish_aot_module_python\n');
 expect(e.fileOffsetToVA(0x680)).toBe(0x10680);expect(e.ranges).toEqual([{name:'python',lo:0x10600,hi:0x10800}]);
 b.writeBigUInt64LE(1n,0xa00+56);expect(()=>elfLayout(b,'')).toThrow('RELA');
});
test('perf dso offsets, nanoseconds and period weighting are parsed explicitly',()=>{
 const p=samples('  1.000000003: 42 ffffff [unknown] (/f+0x680)\n');
 expect(p[0].ns).toBe(1000000003n);expect(p[0].period).toBe(42);expect(p[0].off).toBe(0x680);
 expect(samples('0.1: 1 ab [unknown] ([unknown])')[0].off).toBeNull();
 for(const s of['corrupt','0.1: 9007199254740992 ab x (/f+0x1)'])expect(()=>samples(s)).toThrow();
});
test('phases use shared monotonic ns and half-open bounds',()=>{
 expect(phaseAt(99n,phases)).toBe('startup');expect(phaseAt(100n,phases)).toBe('warmup');expect(phaseAt(200n,phases)).toBe('outside-work');expect(phaseAt(301n,phases)).toBe('steady-state');
});
test('clock anchors intersect bounded reads rather than assuming equal clocks',()=>{
 const rows=[0,100,200,300].map(n=>({beforeNs:String(n),afterNs:String(n+10),hostNs:String(n+1000)}));
 expect(clockOffset(rows)).toEqual({lo:990n,hi:1000n});
 expect(phaseAt(1095n,phases,990n,1000n)).toBe('boundary-uncertain');
 expect(phaseAt(1100n,phases,990n,1000n)).toBe('warmup');
 expect(phaseAt(1195n,phases,990n,1000n)).toBe('boundary-uncertain');
 expect(phaseAt(1350n,phases,990n,1000n)).toBe('steady-state');
 expect(()=>clockOffset(rows.slice(1))).toThrow('anchors');
 expect(()=>clockOffset(rows.map((r,i)=>({...r,hostNs:String(1000+i*200)})))).toThrow('constant offset');
});
test('counter snapshots reject missing/duplicate/malformed sets',()=>{
 expect(snapshots(counters()).map(x=>x.blocks)).toEqual([10,11,12,13]);expect(snapshots('')).toEqual([]);
 expect(()=>snapshots(counters().split('\n').slice(1).join('\n'))).toThrow();
 expect(()=>snapshots(counters().replace('warmup.after','warmup.before'))).toThrow();
 for(const field of ['beforeNs','afterNs','hostNs']) {
  expect(()=>snapshots(counters().replace(new RegExp(`"${field}":"[0-9]+"`),`"${field}":null`))).toThrow();
 }
 expect(()=>snapshots(counters().replace('"hostNs":"210"','"hostNs":"0"'))).toThrow();
 expect(()=>snapshots(counters().replace('"beforeNs":"210"','"beforeNs":"0"'))).toThrow();
});
test('analysis separates native image, lookup and dispatch by weighted cycles',()=>{
 const text='0.000000350: 100 a [unknown] (/f+0x680)\n0.000000360: 50 b aot_find (/f+0x850)\n0.000000370: 50 c cpu_run_to_interrupt (/f+0x900)\n';
 const a=analyse(text,stderr,'/f',elf(),syms,'python','3.14.7',64,true);
 expect(a.buckets.find(x=>x.kind==='native-image:python').cycleFraction).toBe(.5);
 expect(a.buckets.find(x=>x.kind==='lookup-install').cycleFraction).toBe(.25);
 expect(a.nativeHotPCs['steady-state'][String(0x680)]).toBe(100);
 expect(()=>analyse(text.replaceAll('0000003','0000009'),stderr,'/f',elf(),syms,'python','3.14.7',64,true)).toThrow('steady');
 expect(()=>analyse(text,stderr.split('AOT_PROFILE_SNAPSHOT')[0],'/f',elf(),syms,'python','3.14.7',64,true)).toThrow('counters');
});
test('hot PCs are phase-separated and exclude uncertain boundary samples',()=>{
 const records=counters().split('\n').map(l=>JSON.parse(l.slice(21))).map(r=>({...r,afterNs:String(BigInt(r.beforeNs)+10n)}));
 const err=phases.map(p=>'BREAK_EVEN_PHASE '+JSON.stringify(p)).join('\n')+'\n'+records.map(r=>'AOT_PROFILE_SNAPSHOT '+JSON.stringify(r)).join('\n');
 const text='0.000000050: 10 a [unknown] (/f+0x680)\n0.000000095: 100 a [unknown] (/f+0x680)\n0.000000350: 20 a [unknown] (/f+0x680)\n';
 const a=analyse(text,err,'/f',elf(),syms,'python','3.14.7',64,true);
 expect(a.nativeHotPCs.startup[String(0x680)]).toBe(10);
 expect(a.nativeHotPCs['steady-state'][String(0x680)]).toBe(20);
 expect(a.nativeHotPCs['boundary-uncertain']).toBeUndefined();
});
test('CLI refuses unbounded count before reading inputs',async()=>{await expect(main(['/missing','/out','65'])).rejects.toThrow('1..64');});
