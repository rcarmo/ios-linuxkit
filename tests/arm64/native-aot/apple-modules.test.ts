import { expect, test } from 'bun:test';
import { appleBaseModules, appleModules, validateAppleModules } from '../../../tools/jit_aot/apple-modules';

const populated = (modules = appleModules) => modules.map(module => ({
    ...module, sha256: 'a'.repeat(64), translations: 1,
}));
test('Apple images accept the complete Go toolchain and legacy Bun-only bundles', () => {
    expect(validateAppleModules(populated())).toHaveLength(9);
    expect(validateAppleModules(populated(appleBaseModules))).toHaveLength(3);
    expect(appleModules.map(module => module.name)).toContain('go_compile');
    expect(appleModules.map(module => module.name)).toContain('go_link');
    expect(appleModules.map(module => module.name)).toContain('go_asm');
});
test('Apple images reject partial, duplicated, reordered and substituted Go modules', () => {
    for (const modules of [undefined, [], populated().slice(0, 4), populated().slice(0, 8),
        [...populated(), populated()[3]], populated().reverse(),
        populated().map(module => module.name === 'go' ? { ...module, path: '/tmp/go' } : module),
        populated().map(module => ({ ...module, translations: 0 })),
        populated().map(module => ({ ...module, sha256: 'invalid' }))])
        expect(() => validateAppleModules(modules)).toThrow();
});
