#!/usr/bin/env bun
/** Linux-only heldout replay. No training, source builds, or guest staging here.
 * Usage: bun heldout.ts CONFIG.json NEW_OUTPUT_DIRECTORY
 * All files/commands are explicit absolute paths; rootfs must be disposable.
 */
import {createHash} from 'node:crypto';
import {readFileSync, mkdirSync, writeFileSync, appendFileSync, chmodSync, realpathSync} from 'node:fs';
import {isAbsolute, join} from 'node:path';
export const ABI = '3babd760';
export const CODE_VERSION = 15;
export type Mode = 'gadget' | 'native-runtime-emitter' | 'linked-no-emitter' | 'host-native';
type FileHash = {path: string; requestedPath: string; sha256: string};
type Variant = {mode: Mode; binary: string; buildOptions?: string; args: string[]};
type Config = {abi: string; codeVersion: number; release: string; trainingManifest: string;
 trainingFiles: string[]; moduleRoot: string; evaluationFiles: string[];
 runs: number; timeoutSeconds: number; allowEmitter?: boolean;
 cases: {name: string; oracle: string; variants: [Variant, Variant]}[]};
export function digest(bytes: string | Uint8Array): string { return createHash('sha256').update(bytes).digest('hex'); }
export function fileHash(path: string): FileHash {
 if (!isAbsolute(path)) throw Error('absolute path required: ' + path);
 const canonical = realpathSync(path);
 return {path: canonical, requestedPath: path, sha256: digest(readFileSync(canonical))};
}
export function cleanEnv(mode: Mode, inherited = process.env): Record<string,string> {
 const env = Object.fromEntries(Object.entries(inherited).filter(([k,v]) => v !== undefined &&
  !/^ISH_/.test(k))) as Record<string,string>;
 Object.assign(env, {ISH_JIT: mode === 'linked-no-emitter' || mode === 'native-runtime-emitter' ? '1' : '0',
  ISH_AOT_FAMILY: '0', ISH_JIT_STATS: '1'});
 return env;
}
export function assertOracle(actual: string, expected: string) {
 // Exact bytes, including final newline; never learn an oracle from the measured guest.
 if (actual !== expected) throw Error('output oracle mismatch');
}
export function assertContract(abi: string, codeVersion: number) {
 if (abi !== ABI || codeVersion !== CODE_VERSION) throw Error('stale ABI/code version');
}
export function verifyFiles(files: FileHash[]) {
 for (const f of files) {
  if (realpathSync(f.requestedPath ?? f.path) !== f.path || fileHash(f.path).sha256 !== f.sha256)
   throw Error('manifest hash mismatch: ' + f.path);
 }
}
export function assertDiagnostics(mode: Mode, stderr: string, images: number) {
 if (mode === 'linked-no-emitter' && (!/AOT [1-9][0-9]* /.test(stderr) ||
  !stderr.includes(`(${images} images)`))) throw Error('missing linked AOT hits');
 if (mode === 'linked-no-emitter' && !/segments 0, units 0, code 0 KB/.test(stderr)) throw Error('runtime emission detected');
 if (/images:.*[1-9][0-9]* rejected/.test(stderr)) throw Error('rejected images');
}
export function freeze(input: Config) {
 const config: Config = JSON.parse(JSON.stringify(input));
 assertContract(config.abi, config.codeVersion);
 if (config.release !== '2.6.0') throw Error('release mismatch');
 if (!Number.isInteger(config.runs) || config.runs < 1 || config.runs > 10 ||
  !(config.timeoutSeconds > 0 && config.timeoutSeconds <= 60)) throw Error('unbounded run');
 const trainingBytes = readFileSync(config.trainingManifest);
 const training = JSON.parse(trainingBytes.toString());
 assertContract(training.abi, config.codeVersion);
 const trainingFiles = [fileHash(config.trainingManifest), ...config.trainingFiles.map(fileHash)];
 if (!trainingFiles.some(f => f.sha256 === training.workloadSha256)) throw Error('training workload mismatch');
 const modules: FileHash[] = [], images: FileHash[] = [];
 for (const image of training.images) {
  const module = fileHash(join(config.moduleRoot, image.module));
  const asm = fileHash(image.image);
  if (module.sha256 !== image.sha256 || asm.sha256 !== image.imageSha256) throw Error('module/image manifest mismatch');
  const recording = fileHash(join(config.trainingManifest.slice(0,config.trainingManifest.lastIndexOf('/')), image.name + '.jsonl'));
  if (recording.sha256 !== image.recordingSha256) throw Error('recording manifest mismatch');
  // Generated descriptor contains the actual numeric ABI, not just metadata.
  if (!readFileSync(asm.path,'utf8').includes(String(parseInt(ABI,16)))) throw Error('assembly ABI mismatch');
  modules.push(module); images.push(asm); trainingFiles.push(recording);
 }
 const binaries: FileHash[] = [], builds: FileHash[] = [];
 for (const c of config.cases) {
  if (!/^[a-z0-9-]+$/.test(c.name) || !c.oracle || c.variants.length !== 2) throw Error('invalid case');
  for (const v of c.variants) {
   if (!['gadget','linked-no-emitter','native-runtime-emitter','host-native'].includes(v.mode)) throw Error('unknown mode');
   if (v.mode === 'native-runtime-emitter' && !config.allowEmitter) throw Error('emitter requires explicit permission');
   const binary = fileHash(v.binary);
   binaries.push(binary);
   v.binary = binary.path; // Execute the frozen target, not an untracked alias.
   if (v.mode !== 'host-native') {
    if (!v.buildOptions) throw Error('build contract required');
    builds.push(fileHash(v.buildOptions));
    const opts = JSON.parse(readFileSync(v.buildOptions,'utf8'));
    const value = (key: string) => opts.find((o:any) => o.name === key)?.value;
    if (v.mode === 'linked-no-emitter' && (value('jit_emit') !== false || value('jit') !== true ||
      JSON.stringify([...value('cli_aot')].map(p => realpathSync(p)).sort()) !== JSON.stringify(images.map(i=>i.path).sort()))) throw Error('linked build mismatch');
    if (v.mode === 'native-runtime-emitter' && (value('jit_emit') !== true || value('jit') !== true)) throw Error('emitter build mismatch');
   }
  }
 }
 const evaluationFiles = config.evaluationFiles.map(fileHash);
 const aliases = new Map([...trainingFiles,...modules,...images,...evaluationFiles].map(f=>[f.requestedPath,f.path]));
 aliases.set(config.moduleRoot, realpathSync(config.moduleRoot));
 for (const c of config.cases) for (const v of c.variants)
  v.args = v.args.map(arg=>aliases.get(arg) ?? arg);
 if (evaluationFiles.some(e=>trainingFiles.some(t=>t.sha256===e.sha256))) throw Error('training/evaluation overlap');
 // Detached immutable snapshot; no mutable input/config references retained.
 return JSON.parse(JSON.stringify({schema:1, abi:ABI, codeVersion:CODE_VERSION, release:config.release,
  config, trainingFiles, modules, images, binaries, builds, evaluationFiles,
  accounting:fileHash('/usr/bin/time'), timeout:fileHash('/usr/bin/timeout'), harness:fileHash(import.meta.path),
  method:'fresh processes; alternating paired order; no discarded warmup; wall includes startup; GNU time wait4 peak RSS KiB, minor/major faults; no speed/device claim'}));
}
export async function run(configPath: string, output: string) {
 if (!isAbsolute(output)) throw Error('absolute output required');
 const config: Config = JSON.parse(readFileSync(configPath,'utf8'));
 const manifest = freeze(config); // Training sealed before any evaluation process executes.
 mkdirSync(output); // Exclusive output: never overwrite another evidence set.
 const frozen = JSON.stringify(manifest,null,2)+'\n';
 const path = join(output,'manifest.json'); writeFileSync(path,frozen); chmodSync(path,0o444);
 const hash = digest(frozen); writeFileSync(join(output,'manifest.sha256'),hash+'\n');
 const all = [...manifest.trainingFiles,...manifest.modules,...manifest.images,...manifest.binaries,
  ...manifest.builds,...manifest.evaluationFiles,manifest.accounting,manifest.timeout,manifest.harness];
 const start = Date.now();
 try {
  for (const c of manifest.config.cases) for (let pair=0;pair<config.runs;pair++) {
   const order = pair%2 ? [1,0] : [0,1];
   for (let position=0;position<2;position++) {
    if (Date.now()-start > 240000) throw Error('total evaluation budget exhausted');
    verifyFiles(all);
    const v = c.variants[order[position]], id = `${c.name}-${pair}-${position}`;
    const metrics = join(output,id+'.time');
    // GNU timeout owns a process group; killing only GNU time would orphan its
    // guest child. The outer Bun timeout is a last-resort bound with extra grace.
    const command = [manifest.timeout.path,'-k','2','-s','TERM',String(config.timeoutSeconds)+'s',
     manifest.accounting.path,'-f','%e %U %S %M %R %F %x','-o',metrics,v.binary,...v.args];
    const env = cleanEnv(v.mode);
    const child = Bun.spawnSync(command,{env,stdout:'pipe',stderr:'pipe',timeout:(config.timeoutSeconds+4)*1000,killSignal:'SIGKILL'});
    const stdout = child.stdout.toString(), stderr = child.stderr.toString();
    writeFileSync(join(output,id+'.stdout'),stdout); writeFileSync(join(output,id+'.stderr'),stderr);
    const numbers = readFileSync(metrics,'utf8').trim().split('\n').at(-1)!.split(/\s+/).map(Number);
    const [wallSeconds,userSeconds,systemSeconds,maxRssKiB,minorFaults,majorFaults,exitStatus] = numbers;
    const row = {case:c.name,pair,position,mode:v.mode,command,env:Object.fromEntries(Object.entries(env).filter(([k])=>k.startsWith('ISH_'))),
     wallSeconds,userSeconds,systemSeconds,maxRssKiB,minorFaults,majorFaults,exitStatus,stdout,stderr,manifestSha256:hash};
    appendFileSync(join(output,'raw.jsonl'),JSON.stringify(row)+'\n'); // Keep failed samples too.
    if (child.exitCode !== 0 || exitStatus !== 0 || numbers.length!==7 || numbers.some(x=>!Number.isFinite(x))) throw Error(id+': process/accounting failed');
    assertOracle(stdout,c.oracle); assertDiagnostics(v.mode,stderr,manifest.images.length);
   }
  }
  verifyFiles(all);
  if (digest(readFileSync(path)) !== hash) throw Error('frozen manifest mutated');
  writeFileSync(join(output,'PASS'),'all outputs, identities and accounting verified\n');
  console.log('heldout-ok: '+output);
 } catch (e) { writeFileSync(join(output,'FAIL'),String(e)+'\n'); throw e; }
}
if (import.meta.main) {
 const [config,out] = process.argv.slice(2);
 if (!config || !out) throw Error('usage: heldout.ts ABS_CONFIG NEW_ABS_OUTPUT');
 await run(config,out);
}
