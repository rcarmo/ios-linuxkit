export const appleBaseModules = [
    { name: 'musl', path: '/lib/ld-musl-aarch64.so.1' },
    { name: 'busybox', path: '/bin/busybox' },
    { name: 'bun', path: '/usr/local/bin/bun' },
];
export const appleGoModules = [
    { name: 'go', path: '/usr/lib/go/bin/go' },
    { name: 'gofmt', path: '/usr/lib/go/bin/gofmt' },
    ...['asm', 'compile', 'link', 'vet'].map(name => ({
        name: `go_${name}`, path: `/usr/lib/go/pkg/tool/linux_arm64/${name}`,
    })),
];
export const appleLegacyGoModules = [...appleBaseModules, ...appleGoModules];
export const appleModules = appleLegacyGoModules.flatMap(module => module.name === 'go_compile'
    ? [module, { ...module, name: 'go_compile_1' }] : [module]);

export function validateAppleModules(modules: unknown) {
    if (!Array.isArray(modules)) throw Error('missing Apple module set');
    // Retain compatibility with previously recorded Bun-only bundles.
    const expected = modules.length === appleBaseModules.length ? appleBaseModules :
        modules.length === appleLegacyGoModules.length ? appleLegacyGoModules : appleModules;
    if (modules.length !== expected.length || modules.some((module, i) =>
        module?.name !== expected[i].name || module?.path !== expected[i].path ||
        !Number.isSafeInteger(module?.translations) || module.translations <= 0 ||
        !/^[a-f0-9]{64}$/.test(module?.sha256 || '')))
        throw Error('requires complete nonempty musl/BusyBox/Bun images and, if included, the full Go build-tool set');
    return expected;
}
