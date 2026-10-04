import { expect, test } from 'bun:test';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';

const build = process.env.BUILD_DIR || (process.env.ISH_BIN ? dirname(process.env.ISH_BIN) : '');
const enabled = !!build && process.platform === 'darwin' && process.arch === 'arm64';
for (const [name, assembly, marker] of [
    ['neon-differential', 'neon-differential', 'NEON_DIFFERENTIAL'],
    ['integer-differential', 'integer-differential', 'INTEGER_DIFFERENTIAL'],
    ['simd-memory-differential', 'neon-differential', 'SIMD_MEMORY_DIFFERENTIAL'],
]) test.skipIf(!enabled)(`${name} matches native Apple Silicon instructions`, () => {
    const stage = mkdtempSync(join(tmpdir(), 'linuxkit-gadget-oracle-'));
    try {
        const binary = join(stage, name);
        const compile = Bun.spawnSync(['clang', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-D_XOPEN_SOURCE=700', '-D_DARWIN_C_SOURCE', '-DGUEST_ARM64=1', '-DENGINE_ASBESTOS=1', '-DISH_JIT=1',
            '-I' + resolve(import.meta.dir, '../..'), '-I' + resolve(build), '-Wl,-dead_strip',
            join(import.meta.dir, name + '.c'), join(import.meta.dir, assembly + '.S'),
            join(build, 'libish.a'), join(build, 'libish_emu.a'), join(build, 'libfakefs.a'),
            '-lm', '-lsqlite3', '-o', binary], { timeout: 120000 });
        expect(compile.stderr.toString()).toBe('');
        expect(compile.exitCode).toBe(0);
        const run = Bun.spawnSync([binary], { env: { ...process.env, ISH_JIT: '0' }, timeout: 60000 });
        expect(run.stderr.toString()).toBe('');
        expect(run.exitCode).toBe(0);
        expect(run.stdout.toString().trim()).toMatch(new RegExp(`^${marker} cases=[1-9][0-9]* failures=0$`));
    } finally { rmSync(stage, { recursive: true, force: true }); }
}, 180000);
