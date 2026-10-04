// NIST SP 800-38D test case: AES-128-GCM with all-zero inputs.
import { createCipheriv, createDecipheriv, createHash } from 'node:crypto';
const key = Buffer.alloc(16), iv = Buffer.alloc(12), plaintext = Buffer.alloc(16);
const ciphertext = Buffer.from('0388dace60b6a392f328c2b971b2fe78', 'hex');
const tag = Buffer.from('ab6e47d42cec13bdf53a67b21257bddf', 'hex');
for (let i = 0; i < 100; i++) {
    const cipher = createCipheriv('aes-128-gcm', key, iv);
    const encrypted = Buffer.concat([cipher.update(plaintext), cipher.final()]);
    if (!encrypted.equals(ciphertext) || !cipher.getAuthTag().equals(tag))
        throw Error(`AES-GCM encryption mismatch at iteration ${i}: ciphertext=${encrypted.toString('hex')} tag=${cipher.getAuthTag().toString('hex')}`);
    const decipher = createDecipheriv('aes-128-gcm', key, iv);
    decipher.setAuthTag(tag);
    const decrypted = Buffer.concat([decipher.update(ciphertext), decipher.final()]);
    if (!decrypted.equals(plaintext)) throw Error('AES-GCM decryption mismatch');
    if (createHash('sha256').update('abc').digest('hex') !==
            'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
        throw Error('SHA-256 mismatch');
}
console.log('BUN_CRYPTO_OK');
