# AOT RSS and memory-policy optimisations — 2026-11-16

## Result and limits

Training-selected images reduce measured linked-AOT RSS. They do **not** make
AOT the default or establish application acceleration. At the longest measured
work count (32), median peak RSS fell **44.0% for Python** and **16.3% for Bun**
against the full-image build. Both selected variants still used more RSS and
wall time than gadget execution. There was **no demonstrated break-even within
1–32 work units**. In particular, selected Bun's steady phase was slower.

Version remains 2.6.0/build 827; code version 15 / ABI `3babd760` is unchanged.
There is no release/tag, default-execution, sampling-default or host-policy
change. This is Linux evidence, not Darwin SDK/signing/device/jetsam or real
16KiB-host qualification.

## Environment and frozen evidence

- Orange Pi 6 Plus, CIX P1, 12 CPU cores, 16GB-class RAM (~14GiB visible), NVMe.
- Debian Trixie, native AArch64, kernel `6.6.89-cix`, actual host pages 4096 bytes.
- `/workspace` resolves to `/home/agent/workspace`; host-native Piclaw remains
  unconstrained in its user systemd service. No governor, global cache/swap,
  overcommit or shared-cgroup changes were made. Builds used `-j2`.
- Guest Python **3.14.7**, Bun **1.4.4**. Native references are Python **3.13.5**
  and Bun **1.3.10**, not version-matched native speedup denominators. The Bun
  test runner's version is not substituted for an explicitly invoked Bun.
- Evidence root: `/workspace/artifacts/ish-rss-optim-20261115` (work began Nov15).
  `binaries/` retains full/selected/gadget executables and build options.
  `break-even-complete/` retains frozen identities, commands, raw output,
  accounting, phases, fitted models and PASS; `native-reference/` retains
  separate host-native replays. `summary.json` and `summarise.ts` preserve the
  aggregation and raw policy samples. Rootfs is a disposable clone.
- Complete paired manifest SHA-256:
  `b4917459a9d0839f499cc02ecd1d03172d047f7f25f446d38a3174733a8125b0`.
- Source ABI15 recordings: `ish-fixes-20261115/bun15-recordings`; training profile:
  `ish-portable-policy-20261115/profile-training/final.profile`, `dropped=0`.
  Training and evaluation hashes/aliases are independently frozen and checked
  before execution. No evaluation input or result chooses selection weights.

## RSS diagnosis and selection

Python readiness smaps, same new source/control and disposable rootfs, with
family matching disabled:

| Variant | RSS KiB | Pss_Anon KiB | Pss_File KiB |
|---|---:|---:|---:|
| Gadget | 20,452 | 13,108 | 5,632 |
| Full AOT | 44,348 | 13,484 | 29,164 |
| Selected AOT | 22,468 | 12,136 | 8,632 |

This single snapshot locates the amplification predominantly in linked text
and read-only metadata, not admission tickets. It is not a peak-RSS benchmark.

`tools/jit_aot/select.ts` streams recordings, verifies module/recording hashes
and ABI, and ranks training-observed candidates by weight/native-byte density.
The default stopping conditions are 90% represented training weight or 50% of
recorded native segment bytes **per image**. The generator's actual `--keep`
closure is charged, including duplicate variants and overlapping segments;
output is exclusive, deterministic and carries hashes, keep lists and manifest.
Inputs and aliases are rechecked before publication; profile/manifest hashes
come from the bytes actually parsed. Copied recordings/workload are rehashed.
Untrained modules are explicitly reported with zero representation. Impossible
budgets fail rather than silently exceeding their bounds.

| Image | Kept / original translations | Kept / original native bytes |
|---|---:|---:|
| musl | 75 / 2,380 | 28,648 / 1,190,896 |
| busybox | 265 / 1,326 | 84,420 / 721,732 |
| Python | 4,706 / 31,652 | 2,196,748 / 17,731,016 |
| zlib | 281 / 691 | 93,100 / 469,392 |
| Bun | 34,067 / 43,850 | 15,842,396 / 32,231,972 |

Each reached about 90% of its **representable ordinary dispatch weights**.
Ordinary profiles miss chained/return-cache dispatches. This is not 90% of
execution, native hits, all module activity or total program instructions.
The current tool separately reports total module weights. Byte budgets omit
stubs/table/alignment overhead and are not RSS limits. Context indices remain
sparse: no unsafe rebasing of native instructions or dense-context claim.
Removed links use existing generator fixup/fallback behaviour.

The full and selected comparison deliberately uses identical compatible raw
recordings. The ADRP+ADD compaction below changes affected stream keys, so those
old keys may safely miss in **both** builds. Raw recordings were not relabelled;
fresh recordings are needed for complete coverage of the new stream shapes.
`selection-tool.ts` retains the selector version used for measurements; later
changes add reporting/validation without changing those selection decisions.
All five final regenerated assemblies compare byte-for-byte with the measured
selected images (`selected-final/`, `selected-published/`,
`selection-final-equivalence.txt`); final input-publication guards also pass.

## Paired sustained evaluation

Fresh processes, counts 1/8/32, one warmup unit, two alternating paired runs
per contrast: gadget/full and gadget/selected. Thus there are four gadget and
two samples of each AOT variant per runtime/count. The 54 raw records include
six version checks and 48 timed workload executions. Exact checksums include
the frozen first-64-record corpus hash, 42 active records and the expected
weighted sum 12,515,468 per unit. Timings are never output oracles.

Count 32 medians (GNU time wall/peak RSS; internal monotonic steady phase):

| Runtime / variant | Wall s | Peak RSS KiB | Steady s |
|---|---:|---:|---:|
| Python gadget | 1.990 | 24,048 | 0.494 |
| Python full | 2.550 | 53,266 | 0.449 |
| Python selected | 2.375 | 29,820 | 0.468 |
| Bun gadget | 1.880 | 77,748 | 0.953 |
| Bun full | 2.535 | 126,710 | 1.046 |
| Bun selected | 2.725 | 106,000 | 1.338 |

All measured major-fault counts were zero. These are small exploratory sample
sets, not confidence intervals. Python full's descriptive linear crossing was
217 work units, **outside the measured range**; the other crossings were null.
Do not extrapolate a win. Reported startup residual is host wall minus runtime
phases, including imports/input/output/instrumentation and clock precision,
not a causal loader/startup decomposition. There are no cold-disk claims.

Native replay adds 32 records (version checks and repeated native references),
with exact common oracles and preserved RSS/fault/accounting data. Identical
host-native variants are protocol repeats, not a comparison of native engines.
Some native GNU wall values are below 0.01s resolution. Native/guest versions
differ; do not call these matched-version acceleration ratios.

The first complete-series attempt exhausted its explicit 240s harness budget
because identity checking and all repeats count towards it. Its 37 records and
FAIL are retained in `break-even-final/`, not merged into PASS. A separate frozen
900s run completed. The harness permits explicit total bounds up to 1800s,
while individual commands remain bounded and GNU timeout kills process groups.

## Fusion

The existing guarded ADRP+ADD matcher now folds the addition and emits the
existing `gadget_adr`: two stream words rather than three (24→16 bytes).
Both guest instructions are still consumed; the one-instruction guard,
refusals, register/flags semantics and native emitter support remain intact.
O0/O2 real-gadget differential tests each execute 2,328 cases. Existing native
precise-restart/checkpoint gates also pass. No new gadget encoding or ABI
layout is introduced. This is a stream-size improvement, not an application
speed claim. See [the focused audit](FUSION_OPTIM_2026-11-15.md).

## Admission and publication

- Disabled tickets avoid the policy mutex, pinning mode using a counted atomic
  gate. Enabling CASes the idle state; an outstanding disabled ticket cannot
  be overwritten by a mode change. Logical caps still apply.
- Protection/reservation updates publish once under the memory write lock,
  including completed prefixes on failure; no-op updates do not publish.
- The first anonymous protection upgrade claims its full host-rounded backing
  maximum once. Shared aliases serialize across reservation/protection/rollback;
  logical alias charges remain independent. No sample refunds debt; final
  failed unmaps retain conservative debt. This may refuse a small upgrade
  inside a large PROT_NONE backing earlier than the former per-page charge.
- The eight-alias regression now expects eight logical charges but one backing
  charge, surviving destruction of aliases until successful final unmap. New
  tests cover claim contention, failed mprotect, cap/budget refusal, retry,
  partial publication, stale TLBs, failed unmap and fresh-sample non-settlement.

Seven alternating before/after microbenchmark pairs, restricted to the same
CPU allowlist 4–7, medians (heterogeneous cores; not frequency-controlled):

| Fixture | Before | After |
|---|---:|---:|
| Disabled ticket, ns | 78.73 | 73.14 |
| Four-thread ticket, ns/op | 354.50 | 161.48 |
| 64-page protection pair, ns | 50,002.97 | 40,207.14 |
| Publications / 1,000 pairs | 130,000 | 2,000 |

Raw samples and binary hashes are retained. Baseline kernel sources match
`6fefcaee`; unchanged archives are shared with the isolated benchmark build.
This isolates memory bookkeeping, not emulated application performance. The
single-thread spread is noisy; do not advertise a universal speedup.
An independent static review found no additional memory-policy defects under
the documented locking/lifetime assumptions; it was not a proof or selector
review. Earlier review attempts timed out or failed file-path validation and
are not counted as completed reviews.

## Verification and reproduction

Passed against final candidate source:

- Release/debug upstream correctness gates and actual-core policy matrix
  (45 executable invocations per policy lane), plus GCC ASan+UBSan policy lane.
- New optimisation correctness/concurrency suite in release, debug and
  GCC ASan+UBSan. Mixed sanitizer build uses Clang assembly, GCC C and libatomic.
- 65 native/AOT/profile/heldout/selector/fusion/PC-script tests, 428 assertions, **no skips**
  with `PROFILE_EXACT_BIN` set.
- Native emitter and import-fix gates; heldout contract gate.
- Both five-image linked gates: zero rejects, positive installs for every image,
  zero emission, no-emitter imports and runtime-off parity.
- Separate debug hardware-breakpoint proofs hit actual PCs inside all five
  native image ranges for **both** full and selected sets, with NULL emitter
  region and normal output/exit. Named module symbols make the proof independent
  of registration order; `execution.ts --bun` includes the fifth image.
- Linux sampler O0/O2 fixtures; disposable delegated cgroup gate: 11 scenarios,
  ~97.8M peak, swap zero, no OOM. Never constrain Piclaw/shared cgroups.

Initial expected-debt assertions, retained-binary build-directory invocation,
missing sanitizer libatomic and compiler-selection failures remain in earlier
logs; corrected reruns passed. An old expectation was not merely ignored.
The first PC-proof revision incorrectly treated absolute module-header pointers
as relative; the corrected script retains relative decoding only for translation
segment/code fields. All ten final per-image PC proofs passed.

```sh
# All arguments absolute; recordings/profile come only from training.
bun tools/jit_aot/select.ts PROFILE MANIFEST SYMBOL_BINARY MODULE_ROOT NEW_OUT \
  --coverage=0.90 --max-text-ratio=0.50 --format=elf
# Link NEW_OUT/aot_{python,bun,zlib,musl,busybox}.S with jit=true,jit_emit=false.
# Explicit full/selected build-option and assembly identities are required.
bun tests/arm64/native-aot/break-even.ts CONFIG NEW_EVIDENCE
# Existing replay harness supports separately frozen host-native references.
bun tests/arm64/native-aot/heldout.ts NATIVE_CONFIG NEW_NATIVE_EVIDENCE
make test-arm64-aot-optim-contract
make test-arm64-memory-policy-optim CC=clang NINJA='ninja -j2'
PROFILE_EXACT_BIN="$PWD/build-arm64-profile-exact/ish" \
  bun test tests/arm64/native-aot/*.test.ts
```

Example concrete configs, raw measurements and hashes are preserved under the
evidence root above. Defaults remain interpreter/no pressure worker. Selection
is an explicit image-production option, not runtime eviction or an RSS ceiling.
