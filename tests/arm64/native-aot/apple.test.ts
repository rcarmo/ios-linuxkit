import { expect, test } from 'bun:test';
import { appleBuildVersion, checkNoEmitter, observedContract } from '../../../tools/jit_aot/apple';

function binary(platform = 2) {
  const bytes = new Uint8Array(56);
  const view = new DataView(bytes.buffer);
  for (const [offset, value] of [[0, 0xfeedfacf], [4, 0x0100000c], [12, 2], [16, 1], [20, 24],
    [32, 0x32], [36, 24], [40, platform], [44, 15 << 16], [48, 27 << 16]]) {
    view.setUint32(offset, value, true);
  }
  return bytes;
}

test('Apple preparation inspects actual SDK/platform commands', () => {
  expect(appleBuildVersion(binary())).toEqual({ platform: 'ios', minimumOS: '15.0.0', sdk: '27.0.0' });
  expect(appleBuildVersion(binary(7)).platform).toBe('ios-simulator');
  expect(() => appleBuildVersion(binary().subarray(0, 40))).toThrow('truncated');
  const malformed = binary(); new DataView(malformed.buffer).setUint32(36, 0, true);
  expect(() => appleBuildVersion(malformed)).toThrow('command size');
  const arm64e = binary(); new DataView(arm64e.buffer).setUint32(8, 2, true);
  expect(() => appleBuildVersion(arm64e)).toThrow('plain arm64');
  const noBuild = binary(); new DataView(noBuild.buffer).setUint32(32, 0x1b, true);
  expect(() => appleBuildVersion(noBuild)).toThrow('LC_BUILD_VERSION');
});

test('Native object inspection rejects code mapping but permits context data mappings', () => {
  expect(() => checkNoEmitter('                 U _malloc\n                 U _mmap\n00000120 T _jit_layout_read')).not.toThrow();
  for (const symbol of ['_mprotect', '_vm_remap', '_pthread_jit_write_protect_np', 'map_jit', 'memfd_create']) {
    expect(() => checkNoEmitter(`                 U ${symbol}\n`)).toThrow('emitter symbol');
  }
});

test('Observed contract refuses bootstrap, emitter and fabricated conventions', () => {
  const layout = { ready: 1, emission_compiled: 0, pic: 1, abi: 0x12345678,
    code_version: 10, prologue_words: 20, entry_off: 76, n_pinned: 16,
    arch: 'aarch64', pointerBits: 64, little_endian: 1 };
  const build = appleBuildVersion(binary());
  expect(observedContract(layout, 'binary-hash', build, 'target debugger observation')).toMatchObject({
    abi: 0x12345678, binarySha256: 'binary-hash', platform: 'ios', sdk: '27.0.0',
  });
  for (const changes of [{ ready: 0 }, { emission_compiled: 1 }, { pic: 0 }, { abi: 0 }, { pointerBits: 32 }]) {
    expect(() => observedContract({ ...layout, ...changes }, 'binary-hash', build, 'target observation')).toThrow();
  }
  expect(() => observedContract(layout, 'binary-hash', build, ' ')).toThrow('evidence');
});
