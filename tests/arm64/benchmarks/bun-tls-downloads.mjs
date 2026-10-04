// Public, credential-free integration gate; certificate verification stays on.
import https from 'node:https';
const handshakes = [
    ...['X25519MLKEM768', 'X25519', 'prime256v1'].map(group => ({ group, version: 'TLSv1.3' })),
    ...['ECDHE-RSA-AES128-GCM-SHA256', 'ECDHE-RSA-AES256-GCM-SHA384'].map(cipher =>
        ({ group: 'X25519', version: 'TLSv1.2', cipher })),
];
for (const { group, version, cipher } of handshakes) {
        await new Promise((resolve, reject) => {
            const request = https.get('https://example.com', {
                minVersion: version, maxVersion: version, ecdhCurve: group,
                ...(cipher ? { ciphers: cipher } : {}),
            }, response => {
                const negotiated = response.socket.getCipher();
                if (response.socket.getProtocol() !== version || (cipher && negotiated.name !== cipher)) {
                    response.resume();
                    return reject(Error(`unexpected negotiated TLS cipher: ${JSON.stringify(negotiated)}`));
                }
                let bytes = 0;
                response.on('data', data => { bytes += data.length; });
                response.on('error', reject);
                response.on('end', () => {
                    if (response.statusCode !== 200 || !bytes) return reject(Error('empty/failed TLS response'));
                    console.log(`TLS_OK ${version} ${group} ${negotiated.standardName || negotiated.name} ${bytes}`);
                    resolve();
                });
            });
            request.setTimeout(30000, () => request.destroy(Error('TLS request timed out')));
            request.on('error', reject);
        });
}
if (process.argv.includes('--downloads')) for (const url of [
    'https://github.com/sharkdp/fd/releases/download/v10.5.0/fd-v10.5.0-aarch64-unknown-linux-musl.tar.gz',
    'https://github.com/BurntSushi/ripgrep/releases/download/15.2.0/ripgrep-15.2.0-aarch64-unknown-linux-musl.tar.gz',
    'https://huggingface.co/api/models?limit=1',
    'https://models.dev/api.json',
]) {
    const response = await fetch(url, { signal: AbortSignal.timeout(60000) });
    const data = new Uint8Array(await response.arrayBuffer());
    if (!response.ok || !data.length) throw Error(`download failed: ${url}`);
    if (url.endsWith('.tar.gz') && (data[0] !== 0x1f || data[1] !== 0x8b)) throw Error('invalid gzip download');
    if (url.includes('/api')) JSON.parse(new TextDecoder().decode(data));
    console.log(`DOWNLOAD_OK ${data.length} ${url}`);
}
