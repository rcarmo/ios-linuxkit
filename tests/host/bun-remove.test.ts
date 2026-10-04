import { expect, test } from 'bun:test';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';

const ish = process.env.ISH_BIN, root = process.env.ROOTFS;
const build = process.env.BUILD_DIR || (ish ? dirname(ish) : '');
test.skipIf(!build || process.platform !== 'darwin' || process.arch !== 'arm64')(
    'Darwin unlink returns Linux directory errors without changing permissions or following symlinks', () => {
    const stage = mkdtempSync(join(tmpdir(), 'linuxkit-realfs-unlink-'));
    try {
        const binary = join(stage, 'unlink');
        const compile = Bun.spawnSync(['clang', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-D_XOPEN_SOURCE=700', '-D_DARWIN_C_SOURCE', '-DGUEST_ARM64=1', '-DENGINE_ASBESTOS=1', '-DISH_JIT=1',
            '-I' + resolve(import.meta.dir, '../..'), '-I' + resolve(build), '-Wl,-dead_strip',
            join(import.meta.dir, 'realfs-unlink.c'), join(build, 'libish.a'), join(build, 'libish_emu.a'),
            join(build, 'libfakefs.a'), '-lm', '-lsqlite3', '-o', binary], { timeout: 120000 });
        expect(compile.stderr.toString()).toBe('');
        expect(compile.exitCode).toBe(0);
        const run = Bun.spawnSync([binary, stage], { timeout: 30000 });
        expect(run.stderr.toString()).toBe('');
        expect(run.exitCode).toBe(0);
        expect(run.stdout.toString().trim()).toBe('REALFS_UNLINK_OK');
    } finally { rmSync(stage, { recursive: true, force: true }); }
});
test.skipIf(!ish || !root)('guest Bun removes directory trees without following symlinks', () => {
    const script = resolve(import.meta.dir, '../arm64/benchmarks/bun-remove.mjs');
    const run = Bun.spawnSync([resolve(ish!), '-f', resolve(root!), '/usr/bin/env',
        'BUN_JSC_useJIT=0', 'BUN_JSC_numberOfGCMarkers=1', 'BUN_JSC_useConcurrentGC=0',
        '/usr/local/bin/bun', '/mnt/bun-remove'], {
        env: { ...process.env, ISH_JIT: '0', ISH_BIND_MOUNTS: `/mnt/bun-remove=${script}:ro` },
        timeout: 60000,
    });
    expect(run.exitCode).toBe(0);
    expect(run.signalCode).toBeFalsy();
    expect(run.stdout.toString().split('\n').filter(line => line.startsWith('BUN_REMOVE_OK'))).toHaveLength(4);
}, 90000);
