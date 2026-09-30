#!/usr/bin/env bun
/** Record four version-aware Alpine targets. No installs, ignored failures or background jobs.
 * Usage: bun tools/jit_aot/targeted.ts ISH FAKEFS OUTPUT [elf|macho]
 * OUTPUT must not exist; the completed manifest is the publication marker.
 */
import { resolve, join, dirname } from 'node:path';
import { existsSync, mkdirSync } from 'node:fs';
import { createHash } from 'node:crypto';

const [ishArg, rootArg, outArg, format = 'elf'] = process.argv.slice(2);
if (!ishArg || !rootArg || !outArg || !['elf', 'macho'].includes(format))
    throw new Error('usage: targeted.ts ISH FAKEFS OUTPUT [elf|macho]');
const ish = resolve(ishArg), root = resolve(rootArg), out = resolve(outArg);
if (existsSync(out)) throw new Error(`refusing existing output: ${out}`);
mkdirSync(out, { recursive: true });
const timeout = Number(process.env.AOT_TIMEOUT_S || 120);
if (!Number.isFinite(timeout) || timeout <= 0) throw new Error('invalid AOT_TIMEOUT_S');
// Do not inherit tuning/image/recording knobs into the recorder.
const env = Object.fromEntries(Object.entries(process.env).filter(([k, v]) =>
    v !== undefined && !k.startsWith('ISH_JIT') && !k.startsWith('ISH_AOT'))) as Record<string, string>;
function run(argv: string[], extra: Record<string, string> = {}) {
    const r = Bun.spawnSync(['timeout', '-k', '3', String(timeout), ...argv], {
        env: { ...env, ...extra }, stdout: 'pipe', stderr: 'pipe', maxBuffer: 64 * 1024 * 1024,
    });
    return { status: r.exitCode, stdout: r.stdout.toString(), stderr: r.stderr.toString() };
}
function requireOK(r: ReturnType<typeof run>, label: string) {
    if (r.status !== 0) throw new Error(`${label}: exit ${r.status}\n${r.stderr.slice(-2000)}`);
}
const discover = run([ish, '-f', root, '/bin/sh', '-ec', `
cat /etc/alpine-release
apk list --installed musl busybox python3 zlib
python3 -c 'import sysconfig; print("PYLIB=" + (sysconfig.get_config_var("INSTSONAME") or sysconfig.get_config_var("LDLIBRARY")))'
for p in /lib/ld-musl-aarch64.so.1 /bin/busybox /usr/lib/libz.so.1; do readlink -f "$p"; done
`], { ISH_JIT: '0' });
await Bun.write(join(out, 'inventory.log'), discover.stdout + discover.stderr);
requireOK(discover, 'inventory');
const pylib = discover.stdout.match(/^PYLIB=(libpython[0-9.]+\.so(?:\.[0-9.]+)?)$/m)?.[1];
if (!pylib) throw new Error('cannot discover Python library');
const py = run([ish, '-f', root, '/bin/busybox', 'readlink', '-f', `/usr/lib/${pylib}`], { ISH_JIT: '0' });
requireOK(py, 'Python canonical path');
const paths = discover.stdout.split('\n').filter(s => s.startsWith('/'));
if (paths.length !== 3 || !py.stdout.trim().startsWith('/usr/lib/')) throw new Error('invalid module inventory');
const modules = { musl: paths[0], busybox: paths[1], python: py.stdout.trim(), zlib: paths[2] };
for (const module of Object.values(modules)) {
    if (!await Bun.file(join(root, 'data', module)).exists()) throw new Error(`missing module: ${module}`);
}
const workload = `set -eu
i=0; while [ "$i" -lt 1000 ]; do i=$((i+1)); done
test "$i" = 1000
python3 -c 'import zlib,json; a=bytes(range(256))*4096; b=zlib.compress(a); assert zlib.decompress(b)==a; assert sum(range(10000))==49995000; print(json.dumps([len(a),len(b)]))'
printf 'AOT_TRAIN_OK\\n'
`;
await Bun.write(join(out, 'workload.sh'), workload);
const sha = async (path: string) => createHash('sha256').update(new Uint8Array(await Bun.file(path).arrayBuffer())).digest('hex');
const images = [];
let abi: number | undefined;
for (const [name, module] of Object.entries(modules)) {
    const file = join(root, 'data', module), before = await sha(file);
    const recording = join(out, `${name}.jsonl`), image = join(out, `aot_${name}.S`);
    console.log(`recording ${name}: ${module}`);
    const r = run([ish, '-f', root, '/bin/sh', '-ec', workload], {
        ISH_JIT: '1', ISH_JIT_PIC: '1', ISH_JIT_RECORD: recording,
        ISH_JIT_RECORD_MOD: module, ISH_JIT_STATS: '1', ISH_AOT_FAMILY: '0',
    });
    await Bun.write(join(out, `${name}.log`), r.stdout + r.stderr);
    requireOK(r, name);
    if (!r.stdout.split('\n').includes('AOT_TRAIN_OK')) throw new Error(`${name}: missing completion marker`);
    const rows = (await Bun.file(recording).text()).trim().split('\n').map(line => JSON.parse(line));
    const header = rows.shift()?.header;
    if (!header?.abi || !rows.length || rows.some(t => t.mod !== module) || !rows.some(t => t.segs?.length))
        throw new Error(`${name}: invalid/empty recording`);
    if (abi !== undefined && abi !== header.abi) throw new Error('recording ABI mismatch');
    abi = header.abi;
    if (await sha(file) !== before) throw new Error(`${module}: changed during recording`);
    const gen = run(['python3', join(dirname(import.meta.path), 'gen.py'), recording, ish,
        join(root, 'data'), image, '--name', name, '--format', format, '--family', '']);
    await Bun.write(join(out, `${name}-generate.log`), gen.stdout + gen.stderr);
    requireOK(gen, `${name} generator`);
    images.push({ name, module, sha256: before, translations: rows.length,
        image, imageSha256: await sha(image), recordingSha256: await sha(recording) });
}
await Bun.write(join(out, 'manifest.json'), JSON.stringify({
    format, abi: abi!.toString(16).padStart(8, '0'), ish, ishSha256: await sha(ish), root,
    inventory: discover.stdout, workloadSha256: await sha(join(out, 'workload.sh')), images,
    limits: 'Host recording only. No iOS signing/device validation. No family matching.',
}, null, 2) + '\n');
console.log(`complete: ${join(out, 'manifest.json')}`);
