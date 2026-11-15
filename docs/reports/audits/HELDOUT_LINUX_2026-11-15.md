# Heldout Linux harness refresh — 15 November 2026

## Final integration result

The initial delegated qualification below is preserved as a dated snapshot, not
silently updated or relabelled. Final parent integration adds alias-retarget
rejection, canonical executable/input selection, all inherited `ISH_*` controls
stripped (including pressure admission), and GNU timeout process-group cleanup.
Seven tests / 18 assertions pass, including descendant termination. The Go
static smoke executable now also runs **interpreted in the guest**, paired with
its native execution; there is still no Go AOT image or acceleration claim.

Final 24 executions pass exact oracles with frozen manifest
`aca04fed812da0c00cd2d4d60ab5c4836014dcdf05d800b35e588267ce434462`, under
`/workspace/artifacts/ish-portable-policy-20261115/heldout-complete-final/`.
Guest staged input hashes were independently matched to the frozen host inputs.
Updated timing/RSS/fault medians, profiling decision and cross-platform limits
are in the [integration receipt](MEMORY_ADMISSION_LINUX_2026-11-15.md).
The smoke Go guest median is 0.120s / 26,688KiB; native elapsed remains below
measurement resolution. Python/Bun linked runs remain slower and larger here.
No policy performance benefit or substantial Go workload result is inferred.

Executable identity is a hash contract; rebuilding a referenced path must fail
verification against its old manifest. The final integration retains matching
binary snapshots separately for provenance. Earlier snapshots below describe
only the files and observations at their own recording time.

## Initial qualification (preserved snapshot)

### Scope and provenance

Only the new heldout runner, preserved Python workload, unit tests, and this report are added. No commit, memory-core/sampler edits, Rust build, or large rerecording. The preserved `prototype/memory-pressure-policy` versions of `heldout.ts`, `heldout.py`, `heldout-search.sh` and `AOT_HELDOUT_2026-10-01.md` were read with `git show`. Python is byte-for-byte preserved; optional search is not included in this bounded evaluation.

Host confirmed: Orange Pi 6 Plus, Debian Trixie AArch64, kernel `6.6.89-cix`; host-native `piclaw.service` active under `systemctl --user`. `/workspace` resolves to `/home/agent/workspace`, profile `/home/agent/.pi` resolves to `/home/agent/config/.pi`. No host identity or service changes.

Current checkout release **2.6.0**, code version **15**, Linux ABI **3babd760**. The originally supplied `ish-bun144-20261115/recordings` directory is actually stale ABI **d5483d56**, including its assembly descriptors and historical linked logs. It is rejected, not relabelled. The existing linked build points to the compatible five images in `/workspace/artifacts/ish-fixes-20261115/bun15-recordings`; all image, module, raw-recording and training-workload hashes were verified against that manifest. No Go image is present.

### Frozen evidence (initial snapshot)

Evidence: `/workspace/artifacts/ish-heldout-linux-20261115-refresh/evaluation/`. `manifest.json` is an exclusive, read-only snapshot written **before evaluation**, separate from append-only `raw.jsonl`. It contains exact binary/build/module/image/raw-recording/training/evaluation hashes, full commands, oracles, release/ABI identity, and harness/accounting executable hashes. Final full-file hash verification and `PASS` are present.

| Identity | SHA-256 |
| --- | --- |
| frozen manifest | `4e4001fee3c27790cac0c5ec30405e73c39b30d19f252484f92ed9dd080420ba` |
| linked executable | `c06321fc57bf6330d26bd78e7485aedadaa14cbd1b2e032cdcbf7a97718d569c` |
| Meson build options | `cad05be296279617870bb442714e1fd218f242b3b8b3fd91753b7d9a3be4e0af` |
| training manifest | `24d4d6834cf1afe1c182e4ee9c55d0246dd8615ac9be321afd90a789f7132153` |
| training workload | `b7af11f3959dad4a5a4984650d34245aad8767188b313dd22525754d22352390` |
| runner | `eab4439408c55e24134e108f5f36862e3faafea46cb168016437c1c5e822be2b` |

Exact module/image hashes (raw recording hashes also retained in manifest):

| Module | Module SHA-256 | Image SHA-256 |
| --- | --- | --- |
| `/lib/ld-musl-aarch64.so.1` | `32377e6d71725bb019e9ff6d5e9f16b4d5156d6f2c36504191c2d6a7c4d4a44d` | `532385da4ef93eeb994426ed9b9cbd8015dce47694c07f4cb294f400103e7642` |
| `/bin/busybox` | `97d52efa149563c8d886e3670e2496d4140d3c54138017afd3a105e0397fae2e` | `241ce773970fe1cb35aba0799f79b406a0fd16d38e34ce934ba93639f683a83f` |
| `/usr/lib/libpython3.14.so.1.0` | `e2e5bbcf0cac0400d330f1ad3fae215a95e03a82323d309f109d5655e39397ee` | `88c3c15493b4a840bb285bce1f34fd8707f8f35236ef638678d6f52405c9bf82` |
| `/usr/lib/libz.so.1.3.2` | `d3b940580ded8f736804c78df7302b9e7a3f27900ac25a5c97bb05f50992f567` | `51e687d9af8fc247ed76cb28819e8b314b3f9ef8449ccebee6c9780f1198401b` |
| `/usr/local/bin/bun` | `a6a3d99a48600b59597070d2d2ff8d37b65975b8aab247ee3479b1eb33d000a6` | `d6ab5a674920b1c8bf94c2f8764376f203847b9e52060325319b36f86012fdb6` |

Evaluation inputs are hashed separately from training. JSON is 96 deterministic records, 64 selected, eight parse/filter/sort/round-trip passes. Python is only the existing preserved `heldout.py` JSON workload. Bun runs an explicit native CLI script with the same new input and independently computed arithmetic/content oracle. Oracle bytes:

```text
HELDOUT_JSON 64 152001368 b0db2f370d53c7de4828377fc3dbbcd71833a6079bf9c385c764df5d7189aaff
GO_AOT_RUN_OK 45
```

Go uses the existing `tests/arm64/benchmarks/go-aot/main.go`, built outside the repository with `GOMAXPROCS=2 /usr/bin/go build -p 2`. It is only a host-native tiny sort fixture; it is not a fresh Go AOT recording or evidence for arbitrary Go programs. Bun native baseline explicitly uses `/usr/local/bin/bun` (1.3.10); guest Bun is 1.4.4. These versions are not presented as equivalent performance targets.

The user guest filesystem was never launched or changed. A disposable `cp -a --reflink=auto` copy under the evidence root was staged through guest tar, preserving fakefs metadata. Original training inputs/images were only read. Training was already frozen independently before these evaluation inputs were generated. Guest scripts copied by tar were checked through exact output oracles; this run does not claim an independently frozen hash of the complete guest filesystem.

### Modes and accounting (initial snapshot)

Default gadget invocation is `ISH_JIT=0`. Every subprocess strips all inherited `ISH_JIT*`, `ISH_AOT*`, `ISH_BLOCK_PROF*` environment keys, then sets only explicit JIT enablement, stats, and family matching off. Modes are `gadget`, `linked-no-emitter`, explicitly permission-gated `native-runtime-emitter`, and `host-native`. Emitter mode was **not run**. Linked mode validates the build options (`jit=true`, `jit_emit=false`, exact five linked image paths), positive AOT hits with five images and zero runtime segments/units/code. No-JIT default invariant remains intact; no application settings are changed.

Each guest case uses the **same linked executable** for gadget/off versus linked/on. Two pairs alternate position: gadget→linked, then linked→gadget. Native baselines use the same command twice per pair, four repeats total, **not an AOT comparison**. No warmup discarded, no CPU affinity/frequency/thermal control; parent work may compete. All raw samples include commands, explicit ISH settings, pair, position, output, diagnostics and manifest hash. GNU `/usr/bin/time` (installed because absent) supplies elapsed/user/system seconds, peak RSS KiB, minor/major faults and exit status through wait accounting. RSS is CLI/process-tree accounting, not live guest RSS, unique memory or Apple footprint. Time resolution is 0.01 seconds.

### Actual small baseline results (initial snapshot)

Medians; two guest observations per mode, four native observations. This is harness qualification, not statistically supported speedup evidence.

| Case | Mode | n | Wall s | Peak RSS KiB | Minor faults | Major faults |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| bun | gadget | 2 | 0.965 | 65408 | 8297 | 20 |
| bun | linked-no-emitter | 2 | 1.350 | 104128 | 8165 | 0 |
| native-bun | host-native | 4 | 0.035 | 44672 | 3254 | 0 |
| native-go | host-native | 4 | 0.000 | 1792 | 239 | 0 |
| native-python | host-native | 4 | 0.035 | 14464 | 1622 | 0 |
| python | gadget | 2 | 1.670 | 25776 | 4930.5 | 31 |
| python | linked-no-emitter | 2 | 2.310 | 51048 | 5453 | 0 |

All 20 executions returned 0 and exact oracle bytes. Guest Python/Bun linked medians are slower and use more RSS than gadget medians in this tiny, uncontrolled series. Native Go wall time rounded to 0.00 seconds: report it as **below measurement resolution**, never zero execution cost. Raw individual samples, CPU times and outputs are in `raw.jsonl` and per-run `.time/.stdout/.stderr` files.

### Tests and reproduction (initial snapshot)

```sh
cd /workspace/projects/ish-arm64-go
/usr/local/bin/bun test tests/arm64/native-aot/heldout.test.ts
/usr/local/bin/bun tests/arm64/native-aot/heldout.ts \
  /workspace/artifacts/ish-heldout-linux-20261115-refresh/config.json \
  /absolute/new/evidence-directory
```

Actual final local test evidence: **5 tests passed, 0 failed, 15 assertions**. Tests cover stale ABI/code-version rejection, exact output oracles, inherited profiling/JIT/AOT stripping and no-JIT default, changed-file manifest rejection and frozen-hash stability, and positive linked hits/zero-emission diagnostics. Full harness returned `heldout-ok`, with `PASS` and no failed samples. Earlier attempts correctly stopped before evaluation on workspace symlink versus canonical build-path comparison and absent `/usr/bin/time`; those were fixed before the successful series.

Config paths must be absolute. It explicitly supplies training manifest/workload, module backing-data root, evaluation files, binaries/build options, commands/oracles, paired modes, bounded count and per-process timeout. The runner never trains, builds, stages, or silently modifies a rootfs. Callers must include all workload/guest input files in `evaluationFiles`; clone/stage preparation is an operator responsibility. New output directory is mandatory.

### Missing inputs and limits (initial snapshot)

- No trained image/module identity or frozen independent train/evaluation corpus for a substantial Go application. Only an existing tiny native fixture was available and used; arbitrary-Go AOT is unavailable and unclaimed.
- No current-Bun-version matched native-versus-guest package pair. Native Bun is a script baseline, not a guest acceleration comparison.
- No large codec/search evaluation in this 300-second tranche; preserved Python codec remains runnable with explicitly supplied payload and oracle.
- No Apple ABI, signed/device execution, live sampler, sustained/thermal stability, or memory-policy benefit validation.
- The manifest freezes binaries/build options rather than asserting they were rebuilt from the concurrently changing working tree. Current parent memory changes are outside scope.
- Per-process Bun timeout kills the accounting launcher; it is not a verified descendant-tree cancellation contract. The successful series did not hit a timeout. Use an outer process-group timeout for untrusted/long-running workloads.

No unavailable timings, emitter execution, whole-filesystem immutability, or Go AOT results are fabricated.
