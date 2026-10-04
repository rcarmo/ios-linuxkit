#!/usr/bin/env bun
/** Apple AOT preparation checks. These do not establish device execution. */
import { readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { checked, definedSymbols, requireAppleBinary, sha } from './kit';

export function appleBuildVersion(bytes: Uint8Array) {
  requireAppleBinary(bytes, 'ios');
  const data = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (data.getUint32(8, true) !== 0 || data.getUint32(12, true) !== 2)
    throw Error('requires plain arm64 Mach-O executable');
  const end = 32 + data.getUint32(20, true);
  if (end > bytes.length) throw Error('truncated Mach-O load commands');
  let offset = 32;
  let build: { platform: string; minimumOS: string; sdk: string } | undefined;
  const version = (word: number) => `${word >>> 16}.${(word >>> 8) & 255}.${word & 255}`;
  for (let i = 0; i < data.getUint32(16, true); i++) {
    if (offset + 8 > end) throw Error('truncated Mach-O command');
    const command = data.getUint32(offset, true);
    const size = data.getUint32(offset + 4, true);
    if (size < 8 || offset + size > end) throw Error('invalid Mach-O command size');
    if (command === 0x32) {
      if (size < 24 || build) throw Error('invalid/duplicate LC_BUILD_VERSION');
      const platform = ({ 1: 'macos', 2: 'ios', 7: 'ios-simulator' } as Record<number, string>)[data.getUint32(offset + 8, true)];
      if (!platform) throw Error('unsupported Apple platform');
      build = { platform, minimumOS: version(data.getUint32(offset + 12, true)), sdk: version(data.getUint32(offset + 16, true)) };
    }
    offset += size;
  }
  if (offset !== end || !build) throw Error('missing/inconsistent LC_BUILD_VERSION');
  return build;
}

export function checkNoEmitter(symbols: string) {
  // mmap is also used for non-executable AOT context data; it is not evidence
  // of emission. Check the code-mapping/protection primitives specifically.
  for (const line of symbols.split('\n')) {
    const symbol = line.trim().split(/\s+/).at(-1)?.replace(/^_/, '');
    if (symbol && ['map_dual', 'map_jit', 'mprotect', 'vm_remap', 'memfd_create', 'pthread_jit_write_protect_np'].includes(symbol))
      throw Error(`runtime emitter symbol in native backend: ${symbol}`);
  }
}

export function observedContract(layout: any, binarySha256: string, build: ReturnType<typeof appleBuildVersion>, evidence: string) {
  if (layout.ready !== 1 || layout.emission_compiled !== 0 || layout.pic !== 1)
    throw Error('requires ready=1, PIC and a compiled no-emitter target observation; bootstrap ready=0 is insufficient');
  for (const key of ['abi', 'code_version', 'prologue_words', 'entry_off', 'n_pinned']) {
    if (!Number.isSafeInteger(layout[key]) || layout[key] <= 0)
      throw Error(`invalid observed layout field: ${key}`);
  }
  if (layout.arch !== 'aarch64' || layout.pointerBits !== 64 || layout.little_endian !== 1 || !evidence.trim())
    throw Error('invalid target architecture or missing observation evidence');
  return { binarySha256, abi: layout.abi, prologue_words: layout.prologue_words,
    entry_off: layout.entry_off, n_pinned: layout.n_pinned, arch: 'aarch64',
    endian: 'little', pointerBits: 64, ...build, codeVersion: layout.code_version, evidence };
}

async function inspect(buildDirectory: string, binary: string, executionEnabled = false) {
  const options = JSON.parse(readFileSync(join(buildDirectory, 'meson-info/intro-buildoptions.json'), 'utf8'));
  const value = (name: string) => options.find((option: any) => option.name === name)?.value;
  if (value('jit') !== true || value('jit_emit') !== false || value('guest_arch') !== 'arm64' || value('cli_aot')?.length !== 0)
    throw Error('bootstrap Meson options must be native ARM64, no emitter and no CLI images');
  const commands = JSON.parse(readFileSync(join(buildDirectory, 'compile_commands.json'), 'utf8'));
  const jit = commands.find((entry: any) => entry.file.endsWith('/guest-arm64/jit.c'));
  if (!jit) throw Error('native backend was not compiled');
  const command = jit.arguments ? jit.arguments.join(' ') : jit.command;
  for (const define of ['GUEST_ARM64=1', 'ISH_JIT=1', 'ISH_JIT_NO_EMIT=1']) {
    if (!command?.split(/\s+/).includes(`-D${define}`)) throw Error(`native compiler missing ${define}`);
  }
  const object = join(buildDirectory, 'libish_emu.a.p/asbestos_guest-arm64_jit.c.o');
  checkNoEmitter(checked(['nm', object]));
  const symbols = definedSymbols(checked(['nm', '-g', binary]));
  for (const symbol of ['ish_aot_register', 'jit_layout_read', 'jit_layout_describe', 'jit_aot_prepare_layout', 'ish_app_native_fault_install']) {
    if (!symbols.has(symbol)) throw Error(`bootstrap is missing defined symbol: ${symbol}`);
  }
  const build = appleBuildVersion(readFileSync(binary));
  if (!['ios', 'ios-simulator'].includes(build.platform)) throw Error('bootstrap requires an iOS device or simulator build');
  const loadCommands = checked(['xcrun', 'otool', '-l', binary]);
  const haveImages = /sectname\s+__ish_aot\b/.test(loadCommands);
  if (haveImages !== executionEnabled) throw Error('linked images do not match the requested execution mode');
  if (executionEnabled) for (const name of ['musl', 'busybox', 'bun'])
    if (!symbols.has(`ish_aot_module_${name}`)) throw Error(`missing static image: ${name}`);
  console.log(JSON.stringify({ kind: executionEnabled ? 'apple-aot-linked' : 'apple-aot-bootstrap', binary: resolve(binary), binarySha256: await sha(binary),
    ...build, runtimeEmissionCompiled: false, imageExecutionEnabled: executionEnabled,
    validation: 'compiled binary and backend object only; signing, observed ready ABI and device gates remain' }, null, 2));
}

export function bootstrapBuildPaths(settings: any) {
  if (!Array.isArray(settings)) throw Error('expected Xcode build-settings array');
  const targets = settings.filter(entry => entry.target === 'iSH-ARM64-AOT-Bootstrap');
  if (targets.length !== 1) throw Error('missing/ambiguous bootstrap target settings');
  const value = targets[0].buildSettings;
  for (const key of ['MESON_BUILD_DIR', 'TARGET_BUILD_DIR', 'EXECUTABLE_PATH']) {
    if (typeof value?.[key] !== 'string' || !value[key] || value[key].includes('$('))
      throw Error(`missing/unexpanded Xcode build setting: ${key}`);
  }
  return { buildDirectory: resolve(value.MESON_BUILD_DIR), binary: join(value.TARGET_BUILD_DIR, value.EXECUTABLE_PATH) };
}

async function main() {
  const [command, ...args] = process.argv.slice(2);
  if (command === 'inspect' && args.length === 2) await inspect(resolve(args[0]), resolve(args[1]));
  else if (command === 'inspect-settings' && args.length === 1) {
    const settings = JSON.parse(readFileSync(args[0], 'utf8'));
    const paths = bootstrapBuildPaths(settings);
    const value = settings.find((entry: any) => entry.target === 'iSH-ARM64-AOT-Bootstrap').buildSettings.AOT_IMAGE_EXECUTION;
    if (!['0', '1'].includes(value)) throw Error('missing/invalid AOT execution setting');
    await inspect(paths.buildDirectory, paths.binary, value === '1');
  }
  else if (command === 'contract' && args.length === 4) {
    const [binary, layoutFile, output, evidence] = args;
    const build = appleBuildVersion(readFileSync(binary));
    const contract = observedContract(JSON.parse(readFileSync(layoutFile, 'utf8')), await sha(binary), build, evidence);
    writeFileSync(output, JSON.stringify(contract, null, 2) + '\n', { flag: 'wx' });
    console.log(`Wrote observed Apple contract: ${output}`);
  } else {
    console.log('apple.ts inspect MESON_BUILD_DIRECTORY APP_BINARY\napple.ts inspect-settings XCODE_SETTINGS.json\napple.ts contract SYMBOL_BINARY OBSERVED_LAYOUT.json NEW_CONTRACT.json EVIDENCE');
    if (command && command !== 'help') throw Error('invalid arguments');
  }
}

if (import.meta.main) await main();
