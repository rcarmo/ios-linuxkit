import { expect, test } from 'bun:test';
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

// Run the real Meson project preamble with the local compiler. These fake host
// descriptions check argument scope only; they are not Darwin SDK builds.
const meson = readFileSync(resolve(import.meta.dir, '../../meson.build'), 'utf8');
const boundary = meson.indexOf("if cc.get_id() == 'clang'");
if (boundary < 0) throw new Error('Missing Meson preamble boundary');
const preamble = meson.slice(0, boundary);

for (const [system, cpu, enabled] of [
  ['darwin', 'aarch64', true],
  ['darwin', 'x86_64', false],
  ['linux', 'aarch64', false],
] as const) {
  test(`Darwin feature macros are scoped to ARM64: ${system}/${cpu}`, () => {
    const root = mkdtempSync(join(tmpdir(), 'ish-host-features-'));
    try {
      const source = join(root, 'source');
      const build = join(root, 'build');
      mkdirSync(source);
      writeFileSync(join(source, 'meson.build'), `${preamble}\nexecutable('probe', 'probe.c')\n`);
      writeFileSync(join(source, 'probe.c'), 'int main(void) { return 0; }\n');
      const cross = join(root, 'cross.ini');
      writeFileSync(cross, `[binaries]\nc = 'clang'\n[host_machine]\nsystem = '${system}'\ncpu_family = '${cpu}'\ncpu = '${cpu}'\nendian = 'little'\n[properties]\nneeds_exe_wrapper = true\n`);
      const setup = Bun.spawnSync(['meson', 'setup', '--cross-file', cross, build, source], { timeout: 30000 });
      if (setup.exitCode !== 0) throw new Error(`Meson setup failed:\n${setup.stdout}\n${setup.stderr}`);
      const entries = JSON.parse(readFileSync(join(build, 'compile_commands.json'), 'utf8'));
      const probe = entries.find((entry: { file: string }) => entry.file.endsWith('probe.c'));
      expect(probe).toBeDefined();
      const args = probe.arguments ?? probe.command.split(/\s+/);
      expect(args.includes('-D_XOPEN_SOURCE=700')).toBe(enabled);
      expect(args.includes('-D_DARWIN_C_SOURCE')).toBe(enabled);
      const compile = Bun.spawnSync(['ninja', '-C', build], { timeout: 30000 });
      if (compile.exitCode !== 0) throw new Error(`Probe build failed:\n${compile.stdout}\n${compile.stderr}`);
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });
}
