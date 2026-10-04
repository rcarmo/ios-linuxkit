#!/usr/bin/env bun
// Run on an Apple Silicon Mac using real Darwin contexts, not platform shims.
import { mkdirSync, readFileSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { appleBuildVersion } from '../../../tools/jit_aot/apple';

if (process.platform !== 'darwin' || process.arch !== 'arm64')
    throw Error('Apple native recovery tests require an Apple Silicon Mac');
const root = resolve(import.meta.dir, '../../..');
const build = resolve(process.env.BUILD_DIR || 'build-arm64-native-noemit');
const out = resolve(process.env.EVIDENCE_DIR || '/tmp/ios-linuxkit-apple-aot-tests');
mkdirSync(out, { recursive: true });
for (const opt of ['-O0', '-O2']) {
    const flags = ['-arch', 'arm64', '-std=gnu11', opt, '-g', '-DGUEST_ARM64=1',
        '-DISH_JIT=1', '-DISH_JIT_NO_EMIT=1', '-D_XOPEN_SOURCE=700', '-D_DARWIN_C_SOURCE',
        '-I' + root, '-I' + build, '-Wl,-dead_strip', '-Wno-unused-function'];
    for (const name of ['apple-layout', 'apple-fault']) {
        const binary = join(out, name + opt);
        const sources = [join(import.meta.dir, name + '.c')];
        if (name === 'apple-fault') sources.push(join(import.meta.dir, name + '.S'));
        const compile = Bun.spawnSync(['xcrun', 'clang', ...flags, ...sources, '-o', binary], { timeout: 120000 });
        if (compile.exitCode) throw Error(`Apple compile failed: ${compile.stderr}`);
        const platform = appleBuildVersion(readFileSync(binary)).platform;
        if (platform !== 'macos') throw Error('test binary is not a macOS executable');
        const run = Bun.spawnSync([binary], { env: { ...process.env, ISH_JIT: '1', ISH_JIT_PIN: '1' }, timeout: 30000 });
        await Bun.write(binary + '.log', run.stdout.toString() + run.stderr.toString());
        if (run.exitCode) throw Error(`${name} ${opt} failed: ${run.exitCode}\n${run.stdout}\n${run.stderr}`);
        if (name === 'apple-fault') {
            const chain = Bun.spawnSync([binary, 'basic'], { timeout: 30000 });
            if (chain.exitCode !== 74) throw Error('previous one-argument signal handler was not preserved');
        }
        console.log(JSON.stringify({ name, optimization: opt, platform, status: 'pass', evidence: binary + '.log' }));
    }
}
