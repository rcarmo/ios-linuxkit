# Held-out AOT evaluation — 1 October 2026

## Frozen inputs and method

Candidate `983128ba`, isolated `prototype/memory-pressure-policy`. Master remains
2.3.2/build815. Clustering and pressure admission are disabled throughout this
workload evaluation; no memory-policy speedup or Apple claim follows.

Host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), 12 cores, 16 GB-class RAM
(about 14 GiB visible), NVMe, native Debian Trixie AArch64, Linux 4 KiB pages,
Clang 19, Bun 1.4.2, host Python 3.13.5. CPU 0 affinity, unchanged `schedutil`
governor, warm page cache, normal host load. This is not a fixed-frequency,
thermally controlled benchmark.

A disposable guest was restored from the verified Alpine 3.24.2 seed at
`/workspace/artifacts/ish-aot/2.3.0-prototype/seed/`. Exact guest musl/BusyBox,
Python 3.14.7 and zlib 1.3.2 bytes match that seed. No packages were fetched.
Four existing images at `/workspace/artifacts/ish-2.3.2/20260930/elf/` match the
original frozen recording images. No evaluation input was used for retraining.

Evaluation source:
[runner](../../../tests/arm64/native-aot/heldout.ts),
[Python workloads](../../../tests/arm64/native-aot/heldout.py),
[search workload](../../../tests/arm64/native-aot/heldout-search.sh).
The runner generates deterministic JSON/text/payload bytes, hashes them before
execution and verifies them afterwards. It stages files through guest tar, so
fakefs metadata is registered. Oracle checks use host Python for JSON/codec and
native POSIX search/sort/checksum tools for search.

The same release no-emitter executable runs both modes: `ISH_JIT=0` versus
`ISH_JIT=1`, always with family matching off. Inherited JIT/AOT overrides are
removed. One warm-up per mode/case is discarded, followed by five fresh-process
pairs in alternating order. Wall time includes startup and a small exec launcher.
`wait4` supplies peak RSS and CPU time without Bun's inherited fork high-water.
Peak RSS includes CLI/descendants; it is not live sampling, guest-only memory or
Apple physical footprint. AOT diagnostics require positive AOT hits with four
images and zero emitted segments/units/code. Separate four-image linked gates
already passed in the clustering tranche.

Release binary SHA-256:
`3aa1be3038830d54017dd28a2254b9354fdb47b9d7d99804bd7e9a4a1394a8a0`.

## Results

Evidence `/workspace/artifacts/ish-heldout-aot-20261001/evaluation-final/`;
`results.json` retains all samples, input/training/binary hashes, exact expected
outputs and invocation method. Outer `evaluation-final.rc` is 0. All three
cases pass output oracles.

| Workload | Runtime off median ms (range) | AOT median ms (range) | Off/AOT speedup | Off/AOT median peak MiB | AOT/off RSS ratio |
| --- | --- | --- | --- | --- | --- |
| JSON parse/filter/sort/serialize | 6414.9 (6293.6–6507.5) | 6647.4 (6549.2–6709.1) | 0.965× | 27.78 / 64.44 | 2.320× |
| Raw-deflate codec round-trips | 6971.1 (6965.6–7017.3) | 7343.7 (7272.5–7483.6) | 0.949× | 28.12 / 63.65 | 2.264× |
| BusyBox search/sort/checksum | 3774.0 (3761.1–3779.4) | 3396.0 (3342.3–3422.9) | 1.111× | 17.95 / 22.12 | 1.232× |

JSON uses 3,000 structured records, eight parse/filter/sort/serialization passes,
2,000 selected rows and a fixed arithmetic/content oracle. Codec uses a new
512 KiB deterministic mixed-entropy payload, raw-deflate levels 1/6/9 and
chunked inflate, four repetitions per level. Search uses 18,000 records and
three grep/cut/sort/uniq/checksum passes. The training workload was a shell loop,
simple arithmetic and repeated-byte zlib round-trip, not these inputs/modes.
Libraries and interpreter paths necessarily overlap training; "held-out" here
means workload/input separation, not unseen binaries or statistically independent
programs.

Both Python cases are slower and consume more than twice the peak RSS. Search
is modestly faster with higher RSS. Do not extrapolate from three cases to all
workloads or enable the app backend on this evidence.

## Failed compatibility case retained

The original search/compression corpus also attempted BusyBox gzip compression
of the frozen 674,041-byte text. It failed with guest illegal instruction
`0x7e7f2fde` at `0xeff7b5a4` in:

- candidate runtime-off;
- candidate AOT-enabled;
- frozen 2.3.2/build815 release baseline runtime-off.

Compression alone reproduces the fault; an attempted decompression of its failed
output reports invalid magic. The latter is not a codec result. Gzip is excluded
from timing medians and retained as a failed compatibility case, not silently
counted as a successful held-out workload. Root cause is not diagnosed; no fix,
ROM/binary patch or claim that AOT caused it is made.

Evidence files `gzip-off.log`, `gzip-aot.log`, `gzip-release-baseline.log`,
`gzip-compress-only.log` and corresponding `.rc` files retain the failure.

The first evaluation stopped at that failure. A second stopped because its
stats check expected procfs-format image text in stderr; the actual exit stats
use `AOT N (4 images)`. Corrected checks require positive hits and zero emission.
Neither aborted evaluation contributes samples to the reported medians.

After the full run started, the tracked runner gained an independent native
search-output comparison and explicit pair-position metadata. The full run's
native search output was compared separately with `diff` (exit 0); workloads and
sample invocation did not change. `runner-postcheck.ts` and `postcheck-notes.txt`
retain that distinction. A fresh one-pair release run verifies the final runner;
its timings are not pooled into the five-pair results. It overlapped a debug
verification pinned to the same CPU and its JSON timing was contention-affected
(0.470×); it is only a harness correctness check. The separate debug one-pair
run also checks correctness only, not performance parity. Both final-runner
verification and debug correctness exit 0 with all three output checks and
AOT/no-emitter diagnostics passing. Generator/kit tests pass 13 tests and 65
assertions; documentation/link/style gates pass.

## Reproduction

Use a new writable root/evidence path and the retained images; do not train on
these evaluation files:

```sh
WORK=/absolute/new-evaluation
mkdir "$WORK"
bun tools/jit_aot/kit.ts restore /absolute/verified-seed "$WORK/restored"
PERF_CPU=0 HELDOUT_RUNS=5 bun tests/arm64/native-aot/heldout.ts \
  "$PWD/build-memory-cluster-aot-release/ish" "$WORK/restored/root" "$WORK/results"
```

Record hardware/governor and binary/seed/image hashes. The three-case runner does
not claim gzip compatibility; replay the failed command separately from timing:

```sh
ISH_JIT=0 ./build-memory-cluster-aot-release/ish -f "$WORK/restored/root" \
  /bin/sh -ec 'gzip -c /tmp/aot-heldout/records.txt > /tmp/check.gz'
```

## Follow-up boundary

- Keep native/AOT default-off. Profile lookup/fallback and binary/RSS costs before
  increasing training; more recordings are not assumed to repair the regression.
- App wiring still needs isolated configuration, actual Apple ABI and fault-path
  integration, constructor retention, signing and physical-device gates. Linux
  measurements do not permit accelerated Apple distribution.
- Cooperative offload cancellation needs a per-handler contract and race tests;
  it cannot be validated by these workloads or turn FakeFFmpeg into real FFmpeg.
- Retain the gzip failure as a separate compatibility investigation.
