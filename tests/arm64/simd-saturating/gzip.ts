import { mkdirSync, readFileSync, writeFileSync, chmodSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { gunzipSync, gzipSync } from 'node:zlib';
import { createHash } from 'node:crypto';

// Same frozen BusyBox executable runs directly and under iSH. No packages fetched.
const [binaryArg, rootArg, outputArg] = process.argv.slice(2);
if (!binaryArg || !rootArg || !outputArg) throw Error('usage: gzip.ts ISH ROOTFS NEW_EVIDENCE');
const binary = resolve(binaryArg), root = resolve(rootArg), output = resolve(outputArg);
mkdirSync(output); const inputs = join(output, 'inputs'); mkdirSync(inputs);
const data = join(root, 'data');
const busybox = join(data, 'bin/busybox');
const nativeBusybox = join(output, 'busybox'), loader = join(output, 'ld-musl-aarch64.so.1');
writeFileSync(nativeBusybox, readFileSync(busybox)); chmodSync(nativeBusybox, 0o755);
writeFileSync(loader, readFileSync(join(data, 'lib/ld-musl-aarch64.so.1'))); chmodSync(loader, 0o755);
const sha = (bytes: Uint8Array) => createHash('sha256').update(bytes).digest('hex');
const env = { ...process.env };
for (const key of Object.keys(env)) if (/^ISH_(JIT|AOT|TRACE|ARM64_)/.test(key)) delete env[key];
env.ISH_JIT = '0';
let commandIndex = 0;
function run(label: string, args: string[], success: boolean) {
  const proc = Bun.spawnSync(args, { env, timeout: 120_000, maxBuffer: 16 * 1024 * 1024 });
  writeFileSync(join(output, `${commandIndex++}-${label}.log`), proc.stderr);
  if (success ? proc.exitCode !== 0 : ![1, 2].includes(proc.exitCode))
    throw Error(`${label}: exit ${proc.exitCode}: ${proc.stderr.toString()}`);
  return proc;
}
const native = (label: string, args: string[], success = true) =>
  run(`native-${label}`, [loader, '--library-path', join(data, 'lib'), nativeBusybox, ...args], success);
const guest = (label: string, args: string[], success = true) =>
  run(`guest-${label}`, [binary, '-f', root, '/bin/busybox', ...args], success);
// Reconstruct the exact original failing corpus, not a new training workload.
const records: string[] = [];
for (let i = 0; i < 18000; i++) records.push(`id=${i} group=${i % 37} tag=${i % 7 === 0 ? 'needle' : 'hay'} value=${(i * 3571) % 99991}`);
// Frozen original is preferred when explicitly provided for exact failure replay.
const original = process.env.GZIP_FROZEN_TEXT ? readFileSync(process.env.GZIP_FROZEN_TEXT) : Buffer.from(records.join('\n') + '\n');
const cases: Record<string, Buffer> = { records: original, empty: Buffer.alloc(0), single: Buffer.from('x'), repeated: Buffer.alloc(262144, 0x41) };
let state = 0x91e10da5;
const random = Buffer.alloc(131073);
for (let i = 0; i < random.length; i++) { state ^= state << 13; state ^= state >>> 17; state ^= state << 5; random[i] = state & 255; }
for (const size of [32767, 32768, 65535, 65536, 131073]) cases[`boundary-${size}`] = random.subarray(0, size);
for (const [name, bytes] of Object.entries(cases)) writeFileSync(join(inputs, name), bytes);
const hostGzip = gzipSync(original, { level: 9 });
writeFileSync(join(inputs, 'host.gz'), hostGzip);
writeFileSync(join(inputs, 'truncated.gz'), hostGzip.subarray(0, hostGzip.length - 4));
const corrupt = Buffer.from(hostGzip); corrupt[corrupt.length - 8] ^= 0xff;
writeFileSync(join(inputs, 'corrupt.gz'), corrupt);
run('stage-tar', ['tar', '-cf', join(output, 'inputs.tar'), '-C', inputs, '.'], true);
const staged = Bun.spawnSync([binary, '-f', root, '/bin/sh', '-ec', 'mkdir -p /tmp/gzip-regress; tar -xf - -C /tmp/gzip-regress'],
  { env, stdin: readFileSync(join(output, 'inputs.tar')), timeout: 120_000 });
writeFileSync(join(output, 'stage.log'), staged.stderr); if (staged.exitCode) throw Error('fixture staging failed');
let passed = 0;
const results: any[] = [];
for (const [name, bytes] of Object.entries(cases)) for (const level of [1, 6, 9]) {
  const direct = native(`${name}-${level}`, ['gzip', '-n', `-${level}`, '-c', join(inputs, name)]).stdout;
  const emulated = guest(`${name}-${level}`, ['gzip', '-n', `-${level}`, '-c', `/tmp/gzip-regress/${name}`]).stdout;
  if (!Buffer.from(direct).equals(Buffer.from(emulated))) throw Error(`compressed native/guest mismatch ${name}/${level}`);
  if (!gunzipSync(emulated).equals(bytes)) throw Error(`host inflate mismatch ${name}/${level}`);
  results.push({ name, level, inputSha256: sha(bytes), gzipSha256: sha(emulated), compressedBytes: emulated.length }); passed++;
}
const inflate = guest('host-inflate', ['gzip', '-dc', '/tmp/gzip-regress/host.gz']).stdout;
if (!Buffer.from(inflate).equals(original)) throw Error('guest host-produced stream inflate mismatch'); passed++;
for (const name of ['truncated.gz', 'corrupt.gz']) {
  const direct = native(name, ['gzip', '-t', join(inputs, name)], false);
  const emulated = guest(name, ['gzip', '-t', `/tmp/gzip-regress/${name}`], false);
  if (direct.exitCode !== emulated.exitCode) throw Error(`error status mismatch ${name}`);
  // Failed decompression must retain its archive. Match native treatment of
  // any partial output as well; do not invent transactional cleanup semantics.
  const nativeArchive = join(inputs, `failed-${name}`);
  writeFileSync(nativeArchive, readFileSync(join(inputs, name)));
  const nativeFailure = native(`decompress-${name}`, ['gzip', '-d', nativeArchive], false);
  const guestFailure = guest(`decompress-${name}`, ['sh', '-ec', `cd /tmp/gzip-regress; cp ${name} failed-${name}; gzip -d failed-${name}`], false);
  if (nativeFailure.exitCode !== guestFailure.exitCode) throw Error('decompression failure status mismatch');
  const stateCommand = `test -f failed-${name}; if test -f failed-${name.slice(0, -3)}; then echo partial; cat failed-${name.slice(0, -3)}; else echo absent; fi`;
  const nativeState = native(`cleanup-${name}`, ['sh', '-ec', `cd '${inputs}'; ${stateCommand}`]).stdout;
  const guestState = guest(`cleanup-${name}`, ['sh', '-ec', `cd /tmp/gzip-regress; ${stateCommand}`]).stdout;
  if (!Buffer.from(nativeState).equals(Buffer.from(guestState))) throw Error('failed decompression file state mismatch');
  passed += 2;
}
// File lifecycle: successful compression/uncompression removes only its input;
// failed CRC test keeps the archive. Independent round-trip oracle remains above.
guest('file-cleanup', ['sh', '-ec', 'cd /tmp/gzip-regress; cp records lifecycle; gzip -n lifecycle; test ! -e lifecycle; test -f lifecycle.gz; gzip -d lifecycle.gz; test ! -e lifecycle.gz; cmp records lifecycle; test -f corrupt.gz; test -f truncated.gz']); passed++;
writeFileSync(join(output, 'results.json'), JSON.stringify({ binarySha256: sha(readFileSync(binary)), busyboxSha256: sha(readFileSync(busybox)), originalSha256: sha(original), passed, results }, null, 2));
console.log(`gzip-regression-ok: ${passed} checks; native bitstreams, host/guest inflate, invalid input, file cleanup`);
