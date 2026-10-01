import { expect, test } from 'bun:test';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

// Execute the real bridge with a fake Meson CLI, not an Apple compilation.
const bridge = resolve(import.meta.dir, '../../../app/xcode-meson.sh');
for (const mode of ['reused', 'fresh', 'failure']) {
  test(`Xcode bridge enforces gadgets/no emitter: ${mode}`, () => {
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
  if [[ "$GUARD_MODE" == failure && "$2" == -Djit=false ]]; then exit 23; fi
else
  touch "$GUARD_ROOT/configured"
fi
`, { mode: 0o755 });
      const proc = Bun.spawnSync(['bash', bridge], {
        env: { ...process.env, PATH: `${bin}:${process.env.PATH}`, GUARD_ROOT: root,
          GUARD_MODE: mode, MESON_BUILD_DIR: join(root, 'build'), SRCROOT: '/fixture/source',
          ARCHS: 'arm64', GUEST_ARCH: 'arm64', CONFIGURATION: 'Debug',
          ENABLE_ADDRESS_SANITIZER: '', ISH_LOG: '', ISH_LOGGER: '', ISH_KERNEL: '',
          ISH_JIT: '1', ISH_JIT_EMIT: '1' },
      });
      expect(proc.exitCode).toBe(mode === 'failure' ? 23 : 0);
      const calls = readFileSync(join(root, 'calls'), 'utf8');
      expect(calls).toContain('configure -Djit=false');
      if (mode !== 'failure') {
        expect(calls).toContain('configure -Djit=false -Djit_emit=false -Dcli_aot=');
      } else {
        expect(calls.split('\n').filter(line => line.startsWith('configure '))).toHaveLength(1);
      }
      if (mode === 'fresh') expect(calls).toContain('-Djit=false -Djit_emit=false -Dcli_aot=');
    } finally { rmSync(root, { recursive: true, force: true }); }
  });
}
