#!/usr/bin/env bun
/** Training-only AOT image selection. Never runs evaluation or trains a recorder.
 * bun tools/jit_aot/select.ts PROFILE MANIFEST SYMBOL_BINARY MODULE_ROOT NEW_OUTPUT
 *   [--coverage=0.90] [--max-text-ratio=0.50] [--format=elf|macho]
 * Budget is recorded native segment bytes, not whole ELF size/RSS. Omitted
 * translations retain the generator's unlinked/gadget fallback semantics.
 */
import {createHash} from 'node:crypto';
import {createReadStream, readFileSync, writeFileSync, mkdirSync, copyFileSync, constants, realpathSync} from 'node:fs';
import {createInterface} from 'node:readline';
import {resolve, dirname, join, isAbsolute} from 'node:path';
export type Translation = {off:number; bytes:number; entries:number[]};
function integer(n:number) { if (!Number.isSafeInteger(n) || n<0) throw Error('invalid/overflowing integer'); return n; }
export function profile(text:string): Map<string,Map<number,number>> {
 const result=new Map<string,Map<number,number>>();
 if (!text.startsWith('# dispatch profile; dropped=0;')) throw Error('not a complete dispatch training profile');
 for (const line of text.split('\n')) {
  if (!line || line.startsWith('#')) continue;
  // Producer: module-path hex-file-offset instruction-count dispatch-count.
  // Anonymous-PC rows have no image identity and cannot select AOT blocks.
  if (/^[0-9a-fA-F]+\s+\d+\s+\d+$/.test(line)) continue;
  const m=line.match(/^(\/.+?)\s+([0-9a-fA-F]+)\s+(\d+)\s+(\d+)$/);
  if (!m) throw Error('malformed profile row');
  integer(Number(m[3]));
  const off=integer(Number('0x'+m[2])), weight=integer(Number(m[4]));
  let table=result.get(m[1]); if (!table) result.set(m[1],table=new Map());
  table.set(off,integer((table.get(off)??0)+weight));
 }
 return result;
}
export function select(records:Translation[], weights:Map<number,number>, coverage:number, ratio:number) {
 if (!(coverage>0 && coverage<=1 && ratio>0 && ratio<=1) || !records.length) throw Error('invalid selection bounds');
 const totalBytes=records.reduce((n,t)=>integer(n+integer(t.bytes)),0);
 const budget=Math.floor(totalBytes*ratio);
 const reverse=new Map<number,Set<number>>();
 records.forEach((t,i)=>{
  integer(t.off); integer(t.bytes); if (!t.bytes) throw Error('empty translation');
  for(const off of new Set([t.off,...t.entries])) { integer(off); let ids=reverse.get(off); if(!ids) reverse.set(off,ids=new Set()); ids.add(i); }
 });
 const candidates=[...new Set(records.map(t=>t.off))].map(off=>{
  const ids=[...reverse.get(off)!], entries=new Set(ids.flatMap(i=>records[i].entries));
  const weight=[...entries].reduce((n,p)=>integer(n+(weights.get(p)??0)),0);
  const bytes=ids.reduce((n,i)=>integer(n+records[i].bytes),0);
  return {off,ids,weight,bytes};
 }).sort((a,b)=>b.weight/b.bytes-a.weight/a.bytes || b.weight-a.weight || a.off-b.off);
 const observed=new Set(records.flatMap(t=>t.entries));
 const totalWeight=[...observed].reduce((n,p)=>integer(n+(weights.get(p)??0)),0);
 const totalModuleWeight=[...weights.values()].reduce((n,w)=>integer(n+integer(w)),0);
 const kept=new Set<number>(), entries=new Set<number>(), offsets:number[]=[];
 let bytes=0, weight=0;
 for(const c of candidates) {
  if(offsets.length && totalWeight && weight>=totalWeight*coverage) break;
  if(offsets.length && !c.weight) continue;
  const add=c.ids.filter(i=>!kept.has(i));
  const cost=add.reduce((n,i)=>n+records[i].bytes,0);
  if(!add.length || bytes+cost>budget) continue;
  offsets.push(c.off); bytes+=cost;
  for(const i of add) {kept.add(i);for(const p of records[i].entries) if(!entries.has(p)) {entries.add(p);weight+=weights.get(p)??0;}}
 }
 if(!offsets.length) throw Error('native-byte budget cannot fit a translation');
 // Exactly mirror gen.py --keep closure: an offset can retain a translation
 // through any segment, not just its block's first instruction.
 const selectedOffsets=new Set(offsets);
 const closure=records.map((t,i)=>(selectedOffsets.has(t.off)||t.entries.some(p=>selectedOffsets.has(p)))?i:-1).filter(i=>i>=0);
 if(closure.length!==kept.size || closure.some(i=>!kept.has(i))) throw Error('selection closure mismatch');
 return {offsets:offsets.sort((a,b)=>a-b),translations:kept.size,totalTranslations:records.length,
  nativeBytes:bytes,totalNativeBytes:totalBytes,budgetBytes:budget,
  representedWeight:weight,totalRepresentableWeight:totalWeight,
  representedFraction:totalWeight?weight/totalWeight:0,totalModuleWeight,
  representedModuleFraction:totalModuleWeight?weight/totalModuleWeight:0};
}
async function sha(path:string) {const h=createHash('sha256');for await(const chunk of createReadStream(path))h.update(chunk);return h.digest('hex');}
type Identity={path:string;requestedPath:string;sha256:string};
export async function verifyInputs(files:Identity[]) {
 for(const f of files)if(realpathSync(f.requestedPath)!==f.path||await sha(f.path)!==f.sha256)throw Error('input changed during selection: '+f.requestedPath);
}
async function recording(path:string, expectedModule:string, abi:string):Promise<Translation[]> {
 let header:any;const records:Translation[]=[];
 for await(const line of createInterface({input:createReadStream(path),crlfDelay:Infinity})) {
  const t=JSON.parse(line); if(t.header){if(header)throw Error('duplicate header');header=t.header;continue;}
  if(!header||t.mod!==expectedModule||!Array.isArray(t.key)||t.key.length<7||!Array.isArray(t.segs))throw Error('invalid recording');
  const units=integer(t.key[4]);
  if(units>(t.key.length-7)/9)throw Error('truncated unit key');
  const entries:number[]=[];let bytes=0;
  for(const s of t.segs) {
   bytes=integer(bytes+integer(s.words.length)*4);
   const u=Array.from({length:integer(t.key[4])},(_,i)=>7+9*i).find(i=>t.key[i]===s.pos);
   if(u===undefined)throw Error('segment has no guest unit');
   const dpc=integer(t.key[u+3]+t.key[u+4]*4294967296);
   entries.push(integer(t.off+dpc));
  }
  records.push({off:integer(t.off),bytes,entries});
 }
 if(!header||header.region_exhausted||header.region_full||header.abi!==parseInt(abi,16))throw Error('incomplete/incompatible recording');
 return records;
}
export async function generate(args:string[]) {
 const positional=args.filter(x=>!x.startsWith('--'));
 if(positional.length!==5)throw Error('usage: select.ts PROFILE MANIFEST SYMBOL_BINARY MODULE_ROOT NEW_OUTPUT [--coverage=N] [--max-text-ratio=N] [--format=elf|macho]');
 const option=(name:string,fallback:string)=>args.find(x=>x.startsWith(name+'='))?.slice(name.length+1)??fallback;
 for(const arg of args.filter(x=>x.startsWith('--')))if(!/^--(coverage|max-text-ratio|format)=/.test(arg))throw Error('unknown option');
 const [p,m,b,r,out]=positional;for(const path of positional)if(!isAbsolute(path))throw Error('absolute paths required');
 const coverage=Number(option('--coverage','0.90')),ratio=Number(option('--max-text-ratio','0.50')),format=option('--format','elf');
 if(!['elf','macho'].includes(format)||!(coverage>0&&coverage<=1&&ratio>0&&ratio<=1))throw Error('invalid options');
 const sourceBytes=readFileSync(m),profileBytes=readFileSync(p);
 const source=JSON.parse(sourceBytes.toString()), weights=profile(profileBytes.toString());
 if(!/^[0-9a-f]{8}$/.test(source.abi)||!Array.isArray(source.images)||!source.images.length)throw Error('invalid source manifest');
 const fixed:Identity[]=[
  {path:realpathSync(p),requestedPath:p,sha256:createHash('sha256').update(profileBytes).digest('hex')},
  {path:realpathSync(m),requestedPath:m,sha256:createHash('sha256').update(sourceBytes).digest('hex')},
  {path:realpathSync(b),requestedPath:b,sha256:await sha(b)},
  {path:realpathSync(import.meta.path),requestedPath:import.meta.path,sha256:await sha(import.meta.path)}];
 await verifyInputs(fixed);
 mkdirSync(out); // exclusive: no overwritten evidence or recordings
 const images=[], selections=[];
 for(const image of source.images) {
  if(!/^[A-Za-z_][A-Za-z_0-9]*$/.test(image.name)||!image.module.startsWith('/')||image.module.split('/').includes('..'))throw Error('unsafe image');
  const raw=join(dirname(m),image.name+'.jsonl'),moduleFile=join(r,image.module);
  const inputs=[{path:realpathSync(raw),requestedPath:raw,sha256:image.recordingSha256},
   {path:realpathSync(moduleFile),requestedPath:moduleFile,sha256:image.sha256}];
  await verifyInputs(inputs);fixed.push(...inputs);
  const records=await recording(raw,image.module,source.abi), chosen=select(records,weights.get(image.module)??new Map(),coverage,ratio);
  const keep=join(out,image.name+'.keep'), assembly=join(out,'aot_'+image.name+'.S');
  writeFileSync(keep,chosen.offsets.map(n=>'0x'+n.toString(16)).join('\n')+'\n');
  const command=['python3',resolve(import.meta.dir,'gen.py'),raw,b,r,assembly,'--name',image.name,'--format',format,'--keep',keep];
  const result=Bun.spawnSync(command,{stdout:'pipe',stderr:'pipe',timeout:90000});
  writeFileSync(join(out,image.name+'.generate.log'),result.stdout.toString()+result.stderr.toString());
  if(result.exitCode!==0)throw Error('generator failed: '+image.name);
  copyFileSync(raw,join(out,image.name+'.jsonl'),constants.COPYFILE_FICLONE);
  if(await sha(join(out,image.name+'.jsonl'))!==image.recordingSha256)throw Error('copied recording hash mismatch');
  images.push({...image,image:assembly,imageSha256:await sha(assembly)});
  selections.push({name:image.name,module:image.module,...chosen,keepSha256:await sha(keep)});
 }
 const workload=join(dirname(m),'workload.sh');
 const workloadIdentity={path:realpathSync(workload),requestedPath:workload,sha256:source.workloadSha256};
 await verifyInputs([workloadIdentity]);fixed.push(workloadIdentity);
 copyFileSync(workload,join(out,'workload.sh'),constants.COPYFILE_FICLONE);
 if(await sha(join(out,'workload.sh'))!==source.workloadSha256)throw Error('copied workload hash mismatch');
 await verifyInputs(fixed);
 const result={...source,format,images,selection:{schema:1,coverage,maxTextRatio:ratio,
  profile:fixed[0],sourceManifest:fixed[1],generatorSymbols:fixed[2],toolSha256:fixed[3].sha256,selections,
  limits:'Training weights are sampled dispatch representation, not guaranteed AOT hits. Budget counts native segment bytes, not RSS. Sparse context indices and exact-image/fallback checks remain unchanged. No evaluation inputs used.'}};
 writeFileSync(join(out,'manifest.json'),JSON.stringify(result,null,2)+'\n');
 console.log('selected-aot-ok: '+out);
}
if(import.meta.main)await generate(process.argv.slice(2));
