# Bun crypto, TLS and filesystem checks

These tests target guest Bun 1.4.2. Offline oracles use independent native
Node crypto; Node 26 is verified. The ML-KEM oracle requires native
`generateKeyPairSync` ML-KEM support and `crypto.encapsulate`.
They generate synthetic keys locally and require no credentials.

On an Apple Silicon Mac, build the Darwin ARM64 CLI and point `ROOTFS` at a
fakefs userland containing `/usr/local/bin/bun` and `/opt/pi`:

```sh
export BUILD_DIR=/path/to/darwin-arm64-build
export ISH_BIN="$BUILD_DIR/ish"
export ROOTFS=/path/to/bun-fakefs
bun test tests/host/arm64-gadget-differential.test.ts \
  tests/host/bun-crypto-oracles.test.ts tests/host/dczva-retry.test.ts \
  tests/host/bun-remove.test.ts
```

The instruction tests use the actual decoder/gadgets and native Apple Silicon
instructions, not the JIT emitter. Default fixtures cover USHL sizes/aliases,
integer operations and every SIMD single-structure lane with one to four
registers, wrapped register numbers, post-indexing and unaligned/page-crossing
memory. Their binaries optionally accept a file of little-endian instruction
words for broader workload-specific comparisons. Those comparisons isolate
individual instructions and do not prove fused-sequence or fault correctness.

The crypto suite compares 24 AES/GCM records, conventional key exchange/HKDF
and six ML-KEM-768/1024 key derivations against native Node results. It exercises
guest Bun with JavaScript JIT disabled. Bun 1.4.2 does not expose
`crypto.decapsulate`; that limitation is printed explicitly rather than
treated as successful decapsulation coverage. `NODE_BIN` can override `node`.

The live TLS test is separate from offline CI. It contacts public hosts only,
uses no authentication, and leaves certificate verification enabled:

```sh
ISH_JIT=0 \
ISH_BIND_MOUNTS="/mnt/benchmark=$PWD/tests/arm64/benchmarks:ro" \
  "$ISH_BIN" -f "$ROOTFS" /usr/bin/env \
  BUN_JSC_useJIT=0 BUN_JSC_numberOfGCMarkers=1 BUN_JSC_useConcurrentGC=0 \
  /usr/local/bin/bun /mnt/benchmark/bun-tls-downloads.mjs --downloads
```

It checks TLS 1.3 with X25519MLKEM768, X25519 and P-256 and reports the actual
negotiated cipher. It separately forces and verifies TLS 1.2 AES-128/256-GCM,
then fetches the Pi fd/ripgrep release archives, Hugging Face API data and the
models.dev catalog. Bun 1.4.2 did not enforce the requested TLS 1.3 `ciphers`
option, so repeated requests with that option do not establish cipher-suite
coverage. Remove `--downloads` to run only the handshake checks.
Pinned release URLs may eventually stop being served; an HTTP failure should
be distinguished from a crypto failure rather than silently accepted.

For linked AOT validation, repeat the guest checks with the no-emitter linked
CLI and `ISH_JIT=1`, and inspect its AOT/emit counters. Decoder changes require
fresh target-bound recordings; version-11 images cannot validate version 12.
Mac checks do not replace physical iPhone workload and memory-policy checks.

## Pi tool installation

Run the public-download command above with `pi-tool-downloads.mjs` instead of
`bun-tls-downloads.mjs --downloads` to check Pi's complete fd/ripgrep installation.
The fixture must contain Pi but no system fd/ripgrep. It downloads both tools
concurrently, runs `--version` on each, checks that no extraction files remain
and removes its isolated temporary directory. It does not read or change your
Pi configuration or credentials.
