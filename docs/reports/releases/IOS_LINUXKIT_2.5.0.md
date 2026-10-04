# ios-linuxkit 2.5.0 / build 823 - 4 October 2026

Minor source release requested after the Bun/pi crash and TLS repairs.
Annotated tag `v2.5.0` identifies the release source, not an App Store upload
or completed physical-device acceptance. The final iPhone build uses 823;
822 is reserved for observing the version-12 no-emitter target contract.

## Changes since 2.4.1

- xterm is the default terminal; Ghostty remains selectable. Touch selection,
  native clipboard copying, scroll bounds and keyboard viewport tracking are
  repaired.
- An isolated `.aot-bootstrap` app bundles pinned Bun 1.4.2 with Alpine 3.24.2.
  Checked Mach-O images for musl, BusyBox and Bun can be linked explicitly.
  Runtime emission is compiled out, with a native-only fault adapter and
  recorded source/build/image identity. Reference schemes remain gadget-only.
- Aliased polynomial byte multiplication is repaired for AES/GCM correctness.
- `DC ZVA` records its exact memory-fault checkpoint, preventing musl `memset`
  from replaying earlier pointer/length updates and corrupting Bun's heap.
- SIMD halfword single-structure accesses select the correct lane. USHL uses
  native signed-low-byte shift counts and lane-width semantics. The lane repair
  resolves deterministic ML-KEM key corruption and hybrid TLS `BAD_DECRYPT`.

## Validation scope

The final crypto/fault/AOT helper suite passes 37 tests with 189 assertions.
Independent native Node oracles verify 24 AES/GCM cases, conventional key
exchange/HKDF and six ML-KEM key derivations. Native-versus-gadget instruction
tests cover recorded integer/SIMD/memory operations; the previous lane decoder
fails 192 cases in the new self-contained memory regression.

Certificate-verified TLS 1.3 succeeds using X25519MLKEM768, X25519 and P-256,
with the actual negotiated cipher reported. TLS 1.2 AES-128/256-GCM selection
is separately enforced and checked. Bun downloads both Pi tool archives from
GitHub, Hugging Face API data and the models.dev catalog. No certificate
verification bypass or runtime substitution is used.

The offline pi read/edit/write/bash workload passes. Actual Darwin fault/layout
tests pass at O0/O2. See the [Apple audit](../audits/APPLE_AOT_2026-10-04.md) and
[reproduction instructions](../../../tests/arm64/benchmarks/CRYPTO_TESTS.md).
Linux release/debug gates were not rerun on this Mac for this source tag.

## iPhone build boundary

Decoder output changed: AOT code version is 12, Apple ready ABI `3333e99b`.
Generate fresh images from the exact pinned guest modules and checked target
contract. Do not relabel version-11 artifacts. Existing installed userlands are
preserved by app updates; changing the bundled filesystem does not reset them.

The prior installed iPhone build 821 contains the `DC ZVA` fix but not the TLS
lane/shift fixes. The new build must be identified by version 2.5.0/build823 and
its image manifest, not inferred from terminal appearance. At tagging, the new
signed image build and physical-device pi/memory/performance checks remain
pending. Those outcomes must be recorded separately without moving the tag.
