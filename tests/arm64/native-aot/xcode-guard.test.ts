import { expect, test } from 'bun:test';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync, existsSync, readlinkSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

// Execute the real bridge with a fake Meson CLI, not an Apple compilation.
const bridge = resolve(import.meta.dir, '../../../app/xcode-meson.sh');
for (const native of [false, true]) for (const mode of ['reused', 'fresh', 'failure']) {
  test(`Xcode bridge enforces ${native ? 'bootstrap native' : 'gadgets'}/no emitter: ${mode}`, () => {
    const root = mkdtempSync(join(tmpdir(), 'ish-xcode-guard-'));
    try {
      const bin = join(root, 'bin'); mkdirSync(bin);
      const values = {
        buildtype: 'debug', log: '', b_ndebug: false, b_sanitize: 'none',
        log_handler: '', kernel: 'ish', kconfig: '', guest_arch: 'arm64',
        jit: true, jit_emit: true, cli_aot: ['stale-native.S'],
      };
      writeFileSync(join(root, 'options.json'), JSON.stringify(Object.entries(values).map(([name, value]) => ({ name, value }))));
      writeFileSync(join(bin, 'meson'), `#!/bin/bash
printf '%s\\n' "$*" >> "$GUARD_ROOT/calls"
if [[ "$1" == introspect ]]; then
  if [[ "$GUARD_MODE" == fresh && ! -f "$GUARD_ROOT/configured" ]]; then exit 1; fi
  cat "$GUARD_ROOT/options.json"
elif [[ "$1" == configure ]]; then
  if [[ "$GUARD_MODE" == failure && "$2" == -Djit=* ]]; then exit 23; fi
else
  touch "$GUARD_ROOT/configured"
fi
`, { mode: 0o755 });
      const proc = Bun.spawnSync(['bash', bridge, ...(native ? ['--aot-bootstrap'] : [])], {
        env: { ...process.env, PATH: `${bin}:${process.env.PATH}`, GUARD_ROOT: root,
          GUARD_MODE: mode, MESON_BUILD_DIR: join(root, native ? 'meson-aot-bootstrap' : 'build'), SRCROOT: '/fixture/source',
          ARCHS: 'arm64', GUEST_ARCH: 'arm64', CONFIGURATION: 'Debug',
          TARGET_NAME: native ? 'iSH-ARM64-AOT-Bootstrap' : 'iSH-ARM64',
          GCC_PREPROCESSOR_DEFINITIONS: 'GUEST_ARM64=1 ISH_JIT=1 ISH_JIT_NO_EMIT=1 ISH_AOT_BOOTSTRAP=1',
          ENABLE_ADDRESS_SANITIZER: '', ISH_LOG: '', ISH_LOGGER: '', ISH_KERNEL: '',
          ISH_JIT: '1', ISH_JIT_EMIT: '1' },
      });
      expect(proc.exitCode).toBe(mode === 'failure' ? 23 : 0);
      const calls = readFileSync(join(root, 'calls'), 'utf8');
      const jit = native ? 'true' : 'false';
      expect(calls).toContain(`configure -Djit=${jit}`);
      if (mode !== 'failure') {
        expect(calls).toContain(`configure -Djit=${jit} -Djit_emit=false -Dcli_aot=`);
      } else {
        expect(calls.split('\n').filter(line => line.startsWith('configure '))).toHaveLength(1);
      }
      if (mode === 'fresh') expect(calls).toContain(`-Djit=${jit} -Djit_emit=false -Dcli_aot=`);
    } finally { rmSync(root, { recursive: true, force: true }); }
  });
}

test('Native bootstrap refuses production target, reused gadget directory and mismatched definitions', () => {
  const env = { ...process.env, TARGET_NAME: 'iSH-ARM64-AOT-Bootstrap', ARCHS: 'arm64',
    MESON_BUILD_DIR: '/fixture/meson-aot-bootstrap',
    GCC_PREPROCESSOR_DEFINITIONS: 'GUEST_ARM64=1 ISH_JIT=1 ISH_JIT_NO_EMIT=1 ISH_AOT_BOOTSTRAP=1' };
  for (const override of [
    { TARGET_NAME: 'iSH-ARM64' }, { MESON_BUILD_DIR: '/fixture/meson' },
    { ARCHS: 'arm64e' }, { GCC_PREPROCESSOR_DEFINITIONS: 'GUEST_ARM64=1 ISH_JIT=1' },
  ]) {
    const proc = Bun.spawnSync(['bash', bridge, '--aot-bootstrap'], { env: { ...env, ...override } });
    expect(proc.exitCode).toBe(2);
  }
});

const buildPhase = resolve(import.meta.dir, '../../../app/xcode-build-arm64.sh');
for (const mode of ['success', 'meson-failure', 'ninja-failure', 'missing-archive']) {
  test(`Xcode ARM64 phase propagates build failures: ${mode}`, () => {
    const root = mkdtempSync(join(tmpdir(), 'ish-xcode-phase-'));
    try {
      const app = join(root, 'app'); mkdirSync(app);
      const build = join(root, 'meson'); mkdirSync(build);
      const products = join(root, 'products'); mkdirSync(products);
      writeFileSync(join(app, 'xcode-meson.sh'), `#!/bin/sh
printf 'meson\\n' >> "$SRCROOT/calls"
exit ${mode === 'meson-failure' ? 23 : 0}
`, { mode: 0o755 });
      writeFileSync(join(app, 'xcode-ninja.sh'), `#!/bin/sh
printf 'ninja\\n' >> "$SRCROOT/calls"
exit ${mode === 'ninja-failure' ? 24 : 0}
`, { mode: 0o755 });
      if (mode !== 'missing-archive') {
        for (const library of ['libish.a', 'libish_emu.a', 'libfakefs.a']) {
          writeFileSync(join(build, library), 'archive fixture');
        }
      }
      const proc = Bun.spawnSync(['sh', buildPhase], {
        env: { ...process.env, SRCROOT: root, MESON_BUILD_DIR: build,
          CONFIGURATION_BUILD_DIR: products, NINJA_TARGETS: 'libish.a libish_emu.a libfakefs.a' },
      });
      const expectedCode = mode === 'success' ? 0 : mode === 'meson-failure' ? 23 : mode === 'ninja-failure' ? 24 : 1;
      expect(proc.exitCode).toBe(expectedCode);
      expect(readFileSync(join(root, 'calls'), 'utf8')).toBe(mode === 'meson-failure' ? 'meson\n' : 'meson\nninja\n');
      for (const library of ['libish.a', 'libish_emu.a', 'libfakefs.a']) {
        expect(existsSync(join(products, library))).toBe(mode === 'success');
        if (mode === 'success') expect(readlinkSync(join(products, library))).toBe(join(build, library));
      }
    } finally { rmSync(root, { recursive: true, force: true }); }
  });
}
