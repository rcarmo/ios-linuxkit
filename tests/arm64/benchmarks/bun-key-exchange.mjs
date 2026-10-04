import { createECDH, createPrivateKey, createPublicKey, diffieHellman, hkdfSync } from 'node:crypto';
const bytes = (size, seed) => Buffer.from(Array.from({ length: size }, (_, i) => (i * 29 + seed) & 255));
const results = [];
for (const [curve, size] of [['prime256v1', 32], ['secp384r1', 48], ['secp521r1', 66]]) {
    const a = createECDH(curve), b = createECDH(curve);
    const ka = bytes(size, 7), kb = bytes(size, 11);
    if (size === 66) { ka[0] &= 1; kb[0] &= 1; }
    a.setPrivateKey(ka); b.setPrivateKey(kb);
    const shared = a.computeSecret(b.getPublicKey());
    if (!shared.equals(b.computeSecret(a.getPublicKey()))) throw Error(`ECDH asymmetric: ${curve}`);
    results.push({ curve, a: a.getPublicKey().toString('hex'), b: b.getPublicKey().toString('hex'), shared: shared.toString('hex') });
}
const privateKey = seed => createPrivateKey({ key: Buffer.concat([
    Buffer.from('302e020100300506032b656e04220420', 'hex'), bytes(32, seed)]), format: 'der', type: 'pkcs8' });
const a = privateKey(7), b = privateKey(11);
results.push({ curve: 'X25519', a: createPublicKey(a).export({ format: 'der', type: 'spki' }).toString('hex'),
    b: createPublicKey(b).export({ format: 'der', type: 'spki' }).toString('hex'),
    shared: diffieHellman({ privateKey: a, publicKey: createPublicKey(b) }).toString('hex') });
for (const digest of ['sha256', 'sha384']) results.push({ digest,
    derived: Buffer.from(hkdfSync(digest, bytes(32, 7), bytes(32, 11), bytes(13, 19), 128)).toString('hex') });
if (process.argv.includes('--oracle')) console.log(JSON.stringify(results));
else {
    const expected = JSON.parse(await Bun.file(process.argv[2]).text());
    for (const [i, result] of results.entries()) {
        if (JSON.stringify(result) !== JSON.stringify(expected[i])) {
            console.error({ actual: result, expected: expected[i] });
            throw Error(`key exchange mismatch: ${result.curve || result.digest}`);
        }
        console.log(`KEY_EXCHANGE_OK ${result.curve || result.digest}`);
    }
}
