#!/usr/bin/env bun
/** @script Durable AOT artifact kit: prepare, restore, record, generate, build, run, verify.
 * Run `bun tools/jit_aot/kit.ts help`. Linux host tools; Apple conversion is gated,
 * not Apple execution validation. Completed directories are immutable inputs.
 */
import { createHash } from 'node:crypto';
import { createReadStream } from 'node:fs';
import { cpSync, chmodSync, existsSync, lstatSync, mkdirSync, readdirSync, readFileSync, renameSync, statSync } from 'node:fs';
import { dirname, join, resolve, relative, isAbsolute } from 'node:path';
import { createInterface } from 'node:readline';

export const project = resolve(import.meta.dir, '../..');
export const names = ['musl','busybox','python','zlib'];
export function moduleNames(modules: { name: string }[]) {
 if(!Array.isArray(modules))throw Error('missing module set');
 const selected=modules.map(module=>module.name);
 if(!Array.isArray(selected)||new Set(selected).size!==selected.length||names.some(name=>!selected.includes(name))||selected.some(name=>![...names,'bun'].includes(name)))
  throw Error('need the four baseline modules and optionally Bun; unknown/duplicate/incomplete module set');
 return [...names,...(selected.includes('bun')?['bun']:[])];
}
export function cleanEnv(extra: Record<string,string> = {}) {
 return {...Object.fromEntries(Object.entries(process.env).filter(([k,v])=>v!==undefined && !/^(ISH_JIT|ISH_AOT)/.test(k))) as Record<string,string>, ...extra};
}
export function safePath(root: string, p: string) {
 if (!p || isAbsolute(p) || p.split(/[\\/]/).some(s=>s==='..'||s==='.'||s==='') || p.includes('\\')) throw Error(`unsafe artifact path: ${p}`);
 return join(root,p);
}
export async function sha(p:string) { const h=createHash('sha256'); for await(const c of createReadStream(p))h.update(c); return h.digest('hex'); }
export async function inventory(root:string) {
 const files:Record<string,{sha256:string,size:number,mode:number}>={};
 async function walk(dir:string) {for(const n of readdirSync(dir).sort()) {const p=join(dir,n),s=lstatSync(p),r=relative(root,p);if(s.isSymbolicLink())throw Error(`symlink in sealed payload: ${r}`);if(s.isDirectory())await walk(p);else if(s.isFile())files[r]={sha256:await sha(p),size:s.size,mode:s.mode&0o777};else throw Error(`special file: ${r}`);}}
 await walk(root);return files;
}
async function json(p:string, x:unknown){await Bun.write(p,JSON.stringify(x,null,2)+'\n');}
export async function seal(stage:string,out:string,details:any) {
 const files=await inventory(stage);await json(join(stage,'manifest.json'),{schema:'ish-aot-kit/v1',...details,files});
 if(existsSync(out))throw Error(`refusing existing output: ${out}`);renameSync(stage,out);
}
export async function verify(root:string) {
 const m=JSON.parse(readFileSync(join(root,'manifest.json'),'utf8'));
 if(m.schema!=='ish-aot-kit/v1'||!m.files||!Object.keys(m.files).length)throw Error('invalid kit manifest');
 const actual=await inventory(root);delete actual['manifest.json'];
 for(const [p,f] of Object.entries(m.files) as [string,any][]){safePath(root,p);const a=actual[p];if(!a||a.sha256!==f.sha256||a.size!==f.size||a.mode!==f.mode)throw Error(`artifact mismatch: ${p}`);}
 if(Object.keys(actual).length!==Object.keys(m.files).length)throw Error('unlisted payload files');
 return m;
}
function stageFor(out:string){if(existsSync(out))throw Error(`refusing existing output: ${out}`);const s=out+`.partial-${process.pid}`;mkdirSync(s,{recursive:false});return s;}
export function checked(args:string[], log?:string, opts:{cwd?:string,env?:Record<string,string>,timeout?:number}={}) {
 const r=Bun.spawnSync(args,{cwd:opts.cwd||project,env:cleanEnv(opts.env),stdout:'pipe',stderr:'pipe',maxBuffer:128*1024*1024,timeout:opts.timeout||300000});
 const text=r.stdout.toString()+r.stderr.toString();if(log){mkdirSync(dirname(log),{recursive:true});require('node:fs').writeFileSync(log,text);}
 if(r.exitCode!==0||r.signalCode)throw Error(`${args[0]} failed: exit ${r.exitCode} signal ${r.signalCode}; ${log||text.slice(-2000)}`);
 return r.stdout.toString();
}
async function sourceInfo(out:string) {
 const revision=checked(['git','rev-parse','HEAD']).trim();
 const status=checked(['git','status','--porcelain']);
 if(status)throw Error('source checkout must be committed and clean before publishing a kit');
 checked(['git','archive','--format=tar.gz','-o',join(out,'source.tar.gz'),'HEAD']);
 await Bun.write(join(out,'submodules.txt'),checked(['git','submodule','status']));
 const versions:Record<string,string>={};for(const [tool,args] of Object.entries({clang:['--version'],meson:['--version'],ninja:['--version'],bun:['--version'],python3:['--version'],tar:['--version']}))versions[tool]=checked([tool,...args]).split('\n')[0];
 return {revision,submodules:readFileSync(join(out,'submodules.txt'),'utf8'),toolchain:versions,host:checked(['uname','-a']).trim(),sourceScope:'git archive contains tracked source; submodule commits recorded, not vendored; Linux ish build uses system SQLite/libarchive'};
}
async function checkRecordings(dir:string, root:string) {
 const m=JSON.parse(readFileSync(join(dir,'manifest.json'),'utf8'));
 if(m.format!=='elf')throw Error('need completed targeted ELF recordings');
 moduleNames(m.images);
 for(const i of m.images) {
  if(!/^\/[A-Za-z0-9/_.-]+$/.test(i.module)||i.module.split('/').includes('..'))throw Error('unsafe module');
  for(const [p,h] of [[join(dir,i.name+'.jsonl'),i.recordingSha256],[join(root,'data',i.module),i.sha256],[join(dir,`aot_${i.name}.S`),i.imageSha256]])if(await sha(p)!==h)throw Error(`recording identity mismatch: ${p}`);
  let header:any,count=0;for await(const line of createInterface({input:createReadStream(join(dir,i.name+'.jsonl')),crlfDelay:Infinity})){const t=JSON.parse(line);if(t.header){if(header||count)throw Error('duplicate/late recording header');header=t.header;}else{if(t.mod!==i.module)throw Error('wrong recording module');count++;}}
  if(header?.abi?.toString(16).padStart(8,'0')!==m.abi||count!==i.translations||!count)throw Error('ABI/count mismatch');
 }
 if(await sha(join(dir,'workload.sh'))!==m.workloadSha256)throw Error('workload mismatch');
 return m;
}
// Hash backing data and SQLite WAL together. Caller must ensure no live guest.
// This detects observed mutation; it is not a lock against an uncooperative writer.
async function treeSignature(root:string){return createHash('sha256').update(JSON.stringify(await inventory(root))).digest('hex');}
async function prepare(root:string,record:string,build:string,out:string,offline:string) {
 if(offline!=='--quiescent')throw Error('stop all guests first; explicitly pass --quiescent');
 const before=await treeSignature(root),m=await checkRecordings(record,root),s=stageFor(out);
 const provenance=await sourceInfo(s);
 mkdirSync(join(s,'bin'));for(const n of ['fakefsify','unfakefsify']){cpSync(join(build,'tools',n),join(s,'bin',n),{dereference:true});chmodSync(join(s,'bin',n),0o755);}
 if(await sha(m.ish)!==m.ishSha256)throw Error('original recorder hash mismatch');
 cpSync(m.ish,join(s,'bin/recorder'));cpSync(join(build,'ish'),join(s,'bin/gadget'));chmodSync(join(s,'bin/recorder'),0o755);chmodSync(join(s,'bin/gadget'),0o755);
 cpSync(record,join(s,'recordings'),{recursive:true});
 // Preserve raw snapshot including WAL, then export only a private clone.
 checked(['tar','-czf',join(s,'fakefs-snapshot.tar.gz'),'-C',root,'.'],join(s,'snapshot.log'));
 const scratch=join(s,'export-scratch');mkdirSync(scratch);checked(['tar','-xzf',join(s,'fakefs-snapshot.tar.gz'),'-C',scratch]);
 checked([join(s,'bin/unfakefsify'),scratch,join(s,'rootfs.tar.gz')],join(s,'export.log'));
 if(readFileSync(join(s,'export.log'),'utf8').includes('skipping '))throw Error('export skipped stale paths; inspect snapshot');
 require('node:fs').rmSync(scratch,{recursive:true});
 mkdirSync(join(s,'packages'));
 for(const [a,b] of [['lib/apk/db/installed','installed'],['etc/apk/world','world'],['etc/apk/repositories','repositories'],['etc/alpine-release','alpine-release']])cpSync(join(root,'data',a),join(s,'packages',b));
 cpSync(join(root,'data/etc/apk/keys'),join(s,'packages/keys'),{recursive:true});
 if(await treeSignature(root)!==before)throw Error('source guest changed during snapshot');
 await seal(s,out,{kind:'seed',provenance,originalGuestSignature:before,recordingABI:m.abi,inventory:m.inventory,modules:m.images.map((i:any)=>({name:i.name,path:i.module,sha256:i.sha256,translations:i.translations})),compatibility:{linux:'recordings previously verified; rebuild/run required',apple:'unverified candidate; target ABI/symbol/ISA and device gates required'},quiescence:'operator confirmed; complete backing-file content/mode hashes equal before/after',packages:'installed database + keys + repositories retained; exact guest files in rootfs.tar.gz; APK archives not retained'});
}
async function restore(seed:string,out:string) {
 const m=await verify(seed);if(m.kind!=='seed')throw Error('restore needs seed');
 const s=stageFor(out);
 // fakefs_import requires a nonexistent directory.
 const root=join(s,'root');checked([join(seed,'bin/fakefsify'),join(seed,'rootfs.tar.gz'),root],join(s,'restore.log'));
 for(const i of m.modules)if(await sha(join(root,'data',i.path))!==i.sha256)throw Error(`restored module mismatch ${i.name}`);
 renameSync(s,out);return join(out,'root');
}
export function definedSymbols(nm:string){const all=new Set(nm.split('\n').map(x=>x.trim().split(/\s+/)).filter(x=>x.length===3&&/^[0-9a-fA-F]+$/.test(x[0])&&/^[A-TV-Z]$/.test(x[1])).map(x=>x[2]));const map=new Map<string,string>();for(const n of all)map.set(n.startsWith('_')?n.slice(1):n,n);for(const n of all)if(!n.startsWith('_'))map.set(n,n);return map;}
function symbols(binary:string){return definedSymbols(checked(['nm','-g',binary]));}
export function requireAppleBinary(bytes:Uint8Array,platform:string){const d=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);if(bytes.length<32||d.getUint32(0,true)!==0xfeedfacf||d.getUint32(4,true)!==0x0100000c||!['ios','ios-simulator','macos'].includes(platform))throw Error('requires thin Mach-O ARM64 target and explicit Apple platform; ELF/fat binaries rejected');}
export function matchContract(header:any, contract:any, binaryHash:string) {
 for(const k of ['abi','prologue_words','entry_off','n_pinned'])if(contract[k]!==header[k])throw Error(`target contract mismatch: ${k}`);
 if(contract.binarySha256!==binaryHash||contract.arch!=='aarch64'||contract.endian!=='little'||contract.pointerBits!==64)throw Error('target binary/architecture mismatch');
 if(!contract.evidence)throw Error('target contract requires provenance of runtime/layout observation');
}
export async function extractModules(archive:string,root:string,modules:{path:string,sha256:string}[]) {
 for(const i of modules){if(!/^\/[A-Za-z0-9/_.-]+$/.test(i.path)||i.path.split('/').includes('..'))throw Error('unsafe module path');
 const dest=join(root,i.path);mkdirSync(dirname(dest),{recursive:true});
 // Extract each exact regular module to stdout, not filesystem paths/symlinks.
 // Uses host tar on Linux/macOS; never executes the retained Linux importer.
 const r=Bun.spawnSync(['tar','-xOf',archive,'.'+i.path],{env:cleanEnv(),stdout:'pipe',stderr:'pipe',maxBuffer:128*1024*1024,timeout:120000});
 if(r.exitCode!==0||r.signalCode)throw Error(`module extraction failed: ${i.path}`);
 await Bun.write(dest,r.stdout);if(await sha(dest)!==i.sha256)throw Error(`module extraction hash mismatch: ${i.path}`);
 }
}
async function generate(seed:string,out:string,format:string,symbolBinary?:string,contractFile?:string) {
 const m=await verify(seed);if(m.kind!=='seed'||!['elf','macho'].includes(format))throw Error('generate needs seed and elf|macho');
 if(format==='macho'&&(!symbolBinary||!contractFile))throw Error('Mach-O requires actual Apple symbol binary and observed target contract');
 const target=symbolBinary?resolve(symbolBinary):join(seed,'bin/recorder');const syms=symbols(target);const s=stageFor(out);
 const root=join(s,'modules');await extractModules(join(seed,'rootfs.tar.gz'),root,m.modules);
 const c=contractFile?JSON.parse(readFileSync(contractFile,'utf8')):null;
 if(format==='macho')requireAppleBinary(readFileSync(target).subarray(0,32),c.platform);
 const checks=[];
 for(const name of moduleNames(m.modules)){
  const rec=join(seed,'recordings',name+'.jsonl');let header:any;const required=new Set<string>(['ish_aot_register']);
  for await(const line of createInterface({input:createReadStream(rec),crlfDelay:Infinity})) {const t=JSON.parse(line);if(t.header){header=t.header;continue;}for(const n of Object.values(t.keysym||{}))required.add(n as string);for(const seg of t.segs)for(const rel of seg.rel)if(rel[1]!=='exit')required.add(rel[2]);}
  if(c)matchContract(header,c,await sha(target));
  for(const n of required)if(n!=='@region'&&!syms.has(n))throw Error(`unresolved target symbol: ${n}`);
  checked(['python3',join(project,'tools/jit_aot/gen.py'),rec,target,root,join(s,`aot_${name}.S`),'--name',name,'--format',format,'--family',''],join(s,name+'.log'));
  checks.push({name,header,requiredSymbols:[...required].sort()});
 }
 await Bun.write(join(s,'workload.sh'),readFileSync(join(seed,'recordings/workload.sh')));
 require('node:fs').rmSync(root,{recursive:true});
 await seal(s,out,{kind:'generated',format,seedManifestSha256:await sha(join(seed,'manifest.json')),targetBinarySha256:await sha(target),contract:c,checks,appleValidation:format==='macho'?'generation only; SDK/link/sign/device gates remain':'not applicable'});
}
async function build(seed:string,images:string,out:string) {
 const sm=await verify(seed),im=await verify(images);if(sm.kind!=='seed'||im.kind!=='generated'||im.format!=='elf'||im.seedManifestSha256!==await sha(join(seed,'manifest.json')))throw Error('build input identity mismatch');
 const s=stageFor(out);const provenance=await sourceInfo(s);mkdirSync(join(s,'bin'));
 const selected=moduleNames(sm.modules);
 if(moduleNames(im.checks).join()!==selected.join())throw Error('generated image module set mismatch');
 const imageArg=selected.map(n=>join(images,`aot_${n}.S`)).join(',');
 for(const kind of ['gadget','release','debug']) {
  const d=join(s,'build-'+kind);const config=kind==='debug'?'debug':'release';
  checked(['meson','setup',d,'--buildtype='+config,'-Djit='+(kind==='gadget'?'false':'true'),...(kind==='gadget'?[]:['-Djit_emit=false','-Dcli_aot='+imageArg])],join(s,kind+'-setup.log'),{env:{CC:'clang'}});
  // Make accepts relative paths and delegates actual work to Ninja.
  checked(['make','build-arm64-linux','RELEASE_BUILD_DIR='+relative(project,d)],join(s,kind+'-build.log'),{env:{CC:'clang'},timeout:600000});
  cpSync(join(d,'ish'),join(s,'bin',kind));chmodSync(join(s,'bin',kind),0o755);
  mkdirSync(join(s,'options',kind),{recursive:true});for(const f of ['intro-buildoptions.json','intro-machines.json','intro-compilers.json'])cpSync(join(d,'meson-info',f),join(s,'options',kind,f));
 }
 // Build directories contain absolute paths and are deliberately not shipped.
 // Retain their logs/options, test them before deleting via a separate publish.
 await json(join(s,'pending.json'),{kind:'build',provenance,seedManifestSha256:await sha(join(seed,'manifest.json')),imagesManifestSha256:await sha(join(images,'manifest.json'))});
 console.log(`BUILT_PENDING=${s}\nValidate build directories, then publish ${s} ${out}`);
}
export async function publish(s:string,out:string) {
 // Refuse before removing build caches or pending.json, so a destination typo
 // leaves the validated staging directory available for a corrected retry.
 if(existsSync(out))throw Error(`refusing existing output: ${out}`);
 const p=JSON.parse(readFileSync(join(s,'pending.json'),'utf8'));if(p.kind!=='build')throw Error('not pending build');
 if(!existsSync(join(s,'evidence/acceptance.json')))throw Error('missing evidence/acceptance.json');
 const a=JSON.parse(readFileSync(join(s,'evidence/acceptance.json'),'utf8'));if(a.status!=='pass')throw Error('acceptance not passing');
 for(const k of ['gadget','release','debug'])require('node:fs').rmSync(join(s,'build-'+k),{recursive:true});
 require('node:fs').unlinkSync(join(s,'pending.json'));await seal(s,out,{...p,acceptance:a});
}
async function run(bundle:string,root:string,mode:string,args:string[]) {
 const m=await verify(bundle);if(m.kind!=='build'||!['release','debug','gadget','off'].includes(mode)||!args.length)throw Error('run needs build, writable restored ROOT, release|debug|gadget|off, COMMAND...');
 // Never default to a sealed source guest or alter the caller's environment.
 const bin=join(bundle,'bin',mode==='off'?'release':mode);
 const r=Bun.spawn([bin,'-f',root,...args],{env:cleanEnv({ISH_JIT:mode==='off'||mode==='gadget'?'0':'1',ISH_AOT_FAMILY:'0',ISH_JIT_STATS:'1'}),stdin:'inherit',stdout:'inherit',stderr:'inherit'});process.exit(await r.exited);
}
async function main(){const [cmd,...a]=process.argv.slice(2);const p=(i:number)=>{if(!a[i])throw Error('missing argument; use help');return resolve(a[i]);};
 switch(cmd){
 case 'prepare':await prepare(p(0),p(1),p(2),p(3),a[4]);break;
 case 'restore':console.log(await restore(p(0),p(1)));break;
 case 'record':{const s=stageFor(p(2));checked(['bun',join(project,'tools/jit_aot/targeted.ts'),p(0),p(1),join(s,'recordings'),'elf'],join(s,'record.log'),{timeout:1200000});renameSync(s,p(2));break;}
 case 'generate':await generate(p(0),p(1),a[2]||'elf',a[3],a[4]);break;
 case 'build':await build(p(0),p(1),p(2));break;
 case 'publish':await publish(p(0),p(1));break;
 case 'verify':{const m=await verify(p(0));console.log(`verified ${m.kind}: ${Object.keys(m.files).length} files`);break;}
 case 'run':await run(p(0),p(1),a[2],a.slice(3));break;
 default:console.log(`kit.ts prepare ROOTFS RECORDINGS GADGET_BUILD OUTPUT --quiescent
kit.ts restore SEED OUTPUT  # fresh OUTPUT/root; no source guest mutation
kit.ts record RECORDER WRITABLE_ROOT OUTPUT  # only when retraining required
kit.ts generate SEED OUTPUT elf
kit.ts generate SEED OUTPUT macho APPLE_SYMBOL_BINARY OBSERVED_CONTRACT.json
kit.ts build SEED ELF_IMAGES OUTPUT  # leaves .partial-PID for acceptance
kit.ts publish VALIDATED_PARTIAL OUTPUT
kit.ts verify KIT
kit.ts run BUILD RESTORED_ROOT release|debug|gadget|off COMMAND...\nAll output parents must exist; outputs must not exist. Failures retain partials.\nSee docs/NATIVE_AOT_ARTIFACT_KIT.md. No untrusted archives/manifests.`);if(cmd&&cmd!=='help')throw Error('unknown command');
 }
}
if(import.meta.main)await main();
