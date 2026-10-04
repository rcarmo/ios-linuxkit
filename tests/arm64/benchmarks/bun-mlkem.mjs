import crypto from 'node:crypto';
const { generateKeyPairSync, createPrivateKey, createPublicKey } = crypto;
if (process.argv.includes('--oracle')) {
    const cases = [];
    for (const algorithm of ['ml-kem-768', 'ml-kem-1024']) for (let i = 0; i < 3; i++) {
        const { privateKey, publicKey } = generateKeyPairSync(algorithm);
        const { sharedKey, ciphertext } = crypto.encapsulate(publicKey);
        cases.push({ algorithm, privateKey: privateKey.export({ format: 'der', type: 'pkcs8' }).toString('hex'),
            publicKey: publicKey.export({ format: 'der', type: 'spki' }).toString('hex'),
            sharedKey: sharedKey.toString('hex'), ciphertext: ciphertext.toString('hex') });
    }
    console.log(JSON.stringify(cases));
} else {
    const cases = JSON.parse(await Bun.file(process.argv[2]).text());
    for (const [i, test] of cases.entries()) {
        const privateKey = createPrivateKey({ key: Buffer.from(test.privateKey, 'hex'), format: 'der', type: 'pkcs8' });
        const publicKey = createPublicKey(privateKey).export({ format: 'der', type: 'spki' }).toString('hex');
        const sharedKey = crypto.decapsulate?.(privateKey, Buffer.from(test.ciphertext, 'hex')).toString('hex');
        if (publicKey !== test.publicKey || (sharedKey !== undefined && sharedKey !== test.sharedKey)) {
            console.error({ algorithm: test.algorithm, publicKeyMatches: publicKey === test.publicKey,
                sharedKeyMatches: sharedKey === test.sharedKey,
                actualLength: publicKey.length / 2, expectedLength: test.publicKey.length / 2,
                firstDifference: Array.from({ length: Math.min(publicKey.length, test.publicKey.length) / 2 },
                    (_, j) => j).find(j => publicKey.slice(j * 2, j * 2 + 2) !== test.publicKey.slice(j * 2, j * 2 + 2)),
                actualPrefix: publicKey.slice(0, 128), expectedPrefix: test.publicKey.slice(0, 128) });
            throw Error(`ML-KEM oracle mismatch at ${i}`);
        }
        console.log(`MLKEM_KEYGEN_OK ${test.algorithm} ${i}`);
        if (sharedKey === undefined) console.log('MLKEM_DECAP_UNAVAILABLE');
    }
}
