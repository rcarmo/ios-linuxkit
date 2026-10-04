import { expect, test } from 'bun:test';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';

const build = process.env.BUILD_DIR || (process.env.ISH_BIN ? dirname(process.env.ISH_BIN) : '');
test.skipIf(!build || process.platform !== 'darwin' || process.arch !== 'arm64')(
    'DC ZVA retries the exact instruction without replaying earlier pointer updates', () => {
    const stage = mkdtempSync(join(tmpdir(), 'linuxkit-dczva-'));
    try {
        const binary = join(stage, 'retry');
        const compile = Bun.spawnSync(['clang', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-D_XOPEN_SOURCE=700', '-D_DARWIN_C_SOURCE', '-DGUEST_ARM64=1', '-DENGINE_ASBESTOS=1', '-DISH_JIT=1',
            '-I' + resolve(import.meta.dir, '../..'), '-I' + resolve(build), '-Wl,-dead_strip',
            resolve(import.meta.dir, '../arm64/upstream/dczva-retry.c'),
            join(build, 'libish.a'), join(build, 'libish_emu.a'), join(build, 'libfakefs.a'),
            '-lm', '-lsqlite3', '-o', binary], { timeout: 120000 });
        expect(compile.stderr.toString()).toBe('');
        expect(compile.exitCode).toBe(0);
        const run = Bun.spawnSync([binary], { env: { ...process.env, ISH_JIT: '0' }, timeout: 30000 });
        expect(run.stderr.toString()).toBe('');
        expect(run.exitCode).toBe(0);
        expect(run.stdout.toString().trim()).toBe('dczva-precise-retry-ok offsets=64');
    } finally { rmSync(stage, { recursive: true, force: true }); }
});
