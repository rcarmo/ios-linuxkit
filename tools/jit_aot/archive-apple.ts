#!/usr/bin/env bun
import { mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { checked, safePath, sha, verify } from './kit';

const env = process.env;
if (env.TARGET_NAME !== 'iSH-ARM64-AOT-Bootstrap' || env.ARCHS !== 'arm64')
    throw Error('static Apple images require the isolated plain ARM64 target');
const enabled = env.AOT_IMAGE_EXECUTION || '0';
if (!['0', '1'].includes(enabled)) throw Error('invalid AOT_IMAGE_EXECUTION');
if (!env.SDKROOT || !env.CONFIGURATION_BUILD_DIR || !env.SRCROOT || !env.TARGET_BUILD_DIR || !env.UNLOCALIZED_RESOURCES_FOLDER_PATH)
    throw Error('missing Xcode product/SDK settings');
const products = resolve(env.CONFIGURATION_BUILD_DIR);
const objects = join(products, 'apple-aot-objects');
rmSync(objects, { recursive: true, force: true });
mkdirSync(objects, { recursive: true });
const sdk = env.PLATFORM_NAME;
if (!['iphoneos', 'iphonesimulator'].includes(sdk || '')) throw Error('requires iOS SDK');
const platform = sdk === 'iphoneos' ? 'ios' : 'ios-simulator';
const target = `arm64-apple-ios${env.IPHONEOS_DEPLOYMENT_TARGET || '15.0'}${sdk === 'iphonesimulator' ? '-simulator' : ''}`;
const sources: string[] = [];
let manifestHash = '', modules: any[] = [], contract: any;
if (enabled === '1') {
    if (!env.AOT_IMAGES_DIR) throw Error('AOT images requested without AOT_IMAGES_DIR');
    const dir = resolve(env.AOT_IMAGES_DIR), manifest = await verify(dir);
    if (manifest.kind !== 'apple-bun-images' || manifest.format !== 'macho' || manifest.contract?.platform !== platform)
        throw Error('wrong image kind/SDK contract');
    modules = manifest.modules;
    if (!Array.isArray(modules) || modules.map(m => m.name).join(',') !== 'musl,busybox,bun' ||
            modules.some(m => !Number.isInteger(m.translations) || m.translations <= 0))
        throw Error('requires complete nonempty musl/BusyBox/Bun images');
    contract = manifest.contract;
    manifestHash = await sha(join(dir, 'manifest.json'));
    for (const module of modules) sources.push(safePath(dir, `aot_${module.name}.S`));
} else sources.push(join(env.SRCROOT, 'app/aot-empty.c'));
const compiled = [];
for (const [i, source] of sources.entries()) {
    const object = join(objects, `image-${i}.o`);
    checked(['xcrun', 'clang', '-target', target, '-isysroot', env.SDKROOT,
        '-c', source, '-o', object]);
    compiled.push(object);
}
const archive = join(products, 'libish_apple_aot.a');
rmSync(archive, { force: true });
checked(['xcrun', 'libtool', '-static', '-o', archive, ...compiled]);
const resources = join(env.TARGET_BUILD_DIR, env.UNLOCALIZED_RESOURCES_FOLDER_PATH);
mkdirSync(resources, { recursive: true });
await Bun.write(join(resources, 'aot-build.json'), JSON.stringify({
    schema: 'ish-apple-aot-build/v1', executionEnabled: enabled === '1', platform,
    sourceRevision: checked(['git', 'rev-parse', 'HEAD']).trim(),
    sourceDirty: checked(['git', 'status', '--porcelain']).trim().length > 0,
    imageManifestSha256: manifestHash, modules, contract,
}, null, 2));
console.log(`Apple static images: ${enabled === '1' ? manifestHash : 'bootstrap (none)'}`);
