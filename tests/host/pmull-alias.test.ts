import { expect, test } from 'bun:test';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';

test('PMULL and PMULL2 read all source lanes before writing an aliased destination', () => {
    const stage = mkdtempSync(join(tmpdir(), 'linuxkit-pmull-'));
    try {
        const binary = join(stage, 'pmull');
        const compile = Bun.spawnSync(['clang', '-O2', '-Wall', '-Wextra', '-Werror',
            resolve(import.meta.dir, 'pmull-alias.c'),
            resolve(import.meta.dir, '../../asbestos/guest-arm64/crypto_helpers.c'), '-o', binary]);
        expect(compile.exitCode).toBe(0);
        const run = Bun.spawnSync([binary]);
        expect(run.stderr.toString()).toBe('');
        expect(run.exitCode).toBe(0);
        expect(run.stdout.toString().trim()).toBe('PMULL_ALIAS_OK');
    } finally { rmSync(stage, { recursive: true, force: true }); }
});
