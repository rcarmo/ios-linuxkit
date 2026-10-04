// Generate fixtures on the native host; verify them with guest Bun.
import { createCipheriv, createDecipheriv, createHash } from 'node:crypto';
const bytes = (size, seed) => Buffer.from(Array.from({ length: size }, (_, i) => (i * 29 + seed) & 255));
const cases = [];
for (const algorithm of ['aes-128-gcm', 'aes-256-gcm']) {
    for (const size of [0, 1, 15, 16, 17, 64, 128, 256, 1024, 4096, 16384, 32768]) {
        const key = bytes(algorithm === 'aes-128-gcm' ? 16 : 32, 7), iv = bytes(12, 11);
        const aad = bytes(13, 19), plain = bytes(size, 31);
        const cipher = createCipheriv(algorithm, key, iv, { authTagLength: 16 });
        cipher.setAAD(aad);
        const encrypted = Buffer.concat([cipher.update(plain), cipher.final()]);
        cases.push({ algorithm, size, encrypted: encrypted.toString('hex'), tag: cipher.getAuthTag().toString('hex'),
            sha256: createHash('sha256').update(plain).digest('hex'),
            sha384: createHash('sha384').update(plain).digest('hex'),
            sha512: createHash('sha512').update(plain).digest('hex') });
    }
}
if (process.argv.includes('--oracle')) {
    console.log(JSON.stringify(cases));
} else {
    const expected = JSON.parse(await Bun.file(process.argv[2]).text());
    for (const [i, test] of cases.entries()) {
        const oracle = expected[i];
        if (JSON.stringify(test) !== JSON.stringify(oracle)) throw Error(`crypto oracle mismatch: ${test.algorithm}/${test.size}`);
        const key = bytes(test.algorithm === 'aes-128-gcm' ? 16 : 32, 7), iv = bytes(12, 11);
        for (const chunk of [1, 16, 31, 64, 4096, 32768]) {
            const decipher = createDecipheriv(test.algorithm, key, iv, { authTagLength: 16 });
            decipher.setAAD(bytes(13, 19));
            decipher.setAuthTag(Buffer.from(oracle.tag, 'hex'));
            const input = Buffer.from(oracle.encrypted, 'hex'), output = [];
            for (let offset = 0; offset < input.length; offset += chunk)
                output.push(decipher.update(input.subarray(offset, offset + chunk)));
            output.push(decipher.final());
            if (!Buffer.concat(output).equals(bytes(test.size, 31)))
                throw Error(`crypto decrypt mismatch: ${test.algorithm}/${test.size}/${chunk}`);
        }
        console.log(`CRYPTO_RECORD_OK ${test.algorithm} ${test.size}`);
    }
    console.log(`BUN_CRYPTO_RECORDS_OK cases=${cases.length}`);
}
