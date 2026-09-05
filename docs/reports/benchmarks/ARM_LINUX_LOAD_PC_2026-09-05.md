# ARM Linux load-dispatch optimisation — 2026-09-05

## Scope and outcome

Second optimisation pass on `v2.1.2`, baseline
`acb007d0f173b0fb319fe20e7a82004c1dd649f7`. Measurements were collected on the
then-uncommitted engineering candidate. The source change, fixture and this
report are preserved together in the engineering commit; version metadata is
handled separately. Publication was authorised after the local pass.

Retain the inline precise-PC save for normal-register unsigned-immediate
`LDR X`. Across two alternating-order, fixed-frequency series, Python compute
medians improve **2.65% and 1.86%**. Of 30 compute pairs, 23 improve; mean paired
improvement is **1.575%**. These are modest Linux-host gains, not an iOS claim.

Reject the preliminary TLB paired-load/shifted-add candidate: its compute
median improves 0.50% in one 15-pair batch and regresses 0.57% in the repeat.
The TLB layout, tag/generation checks and memory miss handlers are unchanged
in the retained source.

## Host and measurement

- Orange Pi 6 Plus, CIX P1/CD8180, AArch64 Cortex-A720/A520 mixed 12-core SoC,
  16 GB-class RAM (about 14 GiB Linux-visible), NVMe storage.
- Host-native Debian; kernel `6.6.89-cix`, Clang 19.1.7; Debian 13.5 ARM64 fakefs.
- CPU11 affinity, `schedutil`, minimum temporarily raised to its maximum
  **2600198 kHz** during controlled batches. EXIT traps restored **799865 kHz**.
- Same release build configuration, fakefs and guest commands for both variants.
- `tests/arm64/perf-poke.ts`: fresh emulator per invocation, warm host page
  caches, two discarded warmups per variant, 15 measured pairs per workload,
  alternating order, exact stdout/zero-exit/empty-stderr checks, no diagnostic
  overrides. No global cache drop. Shared-host scheduling noise remains.
- Source profile: 2,170 `cycles:u` samples at 499 Hz. `load64_imm_fast` plus its
  continuation accounts for 18.38%; standalone `set_jit_saved_pc` 3.18%.
- Short startup profile: 207 samples; `cpu_run_to_interrupt` 19.88%. This is a
  localisation hint only, not a statistically strong startup attribution.
- Candidate profile: standalone PC-save gadget share 1.78%. Percentages are
  samples, not a substitute for paired wall-time evidence.

## Retained change

Previously each relevant LDR emitted four stream words:

```text
[set_jit_saved_pc][guest PC][load64_imm_fast][operands]
```

It now emits three:

```text
[load64_imm_fast][operands][guest PC]
```

The load gadget consumes operands and PC with `ldp`, saves the precise retry PC
before any faultable access, then follows the existing address and TLB path.
This eliminates one indirect gadget dispatch and one eight-byte stream word
per affected load. No new reserved register or platform-specific ABI is used.

The generator skips the standalone save only for unsigned-immediate `LDR X`
with both base and destination different from register 31. The existing
LDR+CBZ/CBNZ fusion for that form already saves its own precise LDR PC. Other
memory forms retain the standalone save. The common gadget always receives
the extra PC word, including existing decoder fallback cases. SP, XZR, stores,
other widths and other addressing modes retain their behaviour.

## Paired measurements

Median wall seconds (15 pairs per row, fresh process each sample):

| Workload | Batch 1 baseline | Batch 1 candidate | Batch 2 baseline | Batch 2 candidate |
|---|---:|---:|---:|---:|
| Shell startup | 0.01745 | 0.01795 | 0.01950 | 0.01924 |
| Python startup (`-S`) | 0.23408 | 0.23323 | 0.23771 | 0.23584 |
| Python `sum(range(5000000))` | 4.39134 | 4.27507 | 4.39516 | 4.31354 |
| Python temporary-file I/O | 0.75053 | 0.74434 | 0.74825 | 0.74182 |

- Compute: 23/30 pairs faster; 1.575% mean paired improvement.
- Python startup: 23/30 faster; 0.805% mean paired improvement.
- Python I/O: 26/30 faster; 0.847% mean paired improvement. This workload includes
  Python execution/startup; it does not establish faster host storage.
- Shell startup: inconsistent medians, 17/30 faster and mean paired **2.81%
  slower** because of outliers. No shell-startup improvement claimed.
- Compute median maximum RSS unchanged at 32,452,608 bytes; no memory saving
  demonstrated at process level despite the smaller stream encoding.

## Correctness evidence

`CC=clang make build-arm64-linux-all` passes release/debug. Final release
binary SHA-256 equals the timed candidate after rebuilding. Existing compiler
warnings remain; this is not a warning-free build claim.

Both final release and debug pass:

```text
tests/arm64/fs/run-lseek-width.sh
tests/arm64/signals/run-poke-stress.sh
tests/arm64/fp/run-fcvt-vector.sh
tests/arm64/proc/run-proc-mem-seek.sh
tests/arm64/loadstore/run-load64-fault-pc.sh
```

The new gate builds one static native-AArch64 oracle and runs identical bytes
under iSH. Its 18 cases cover plain LDR, adjacent CBZ and CBNZ (both directions),
aligned/unaligned/cross-page loads and zero/nonzero data. Each case warms code
and translations, unmaps both backing pages, checks SIGSEGV's exact LDR PC and
pre-load registers, restores the mappings, and verifies the instruction before
the load was not repeated. Native, unchanged v2.1.2, release and debug pass.
For split loads the fault is on the **first** page, not the second page alone.

A deliberate negative mutation (`str xzr` instead of saving the LDR PC) fails
with exit **40**, the signal-context assertion. Restoring the source, rebuilding
and running `CC=clang make test-arm64-load64-fault-pc` passes. Negative mutation
is not retained; final SHA-256 matches the measured candidate.

Additional static native-oracle fixtures pass with identical stdout on both
release and debug (eight guest rows):

- `tests/arm64/atomics/cas128.c`
- `tests/arm64/atomics/clrex-stxr.c`
- `tests/arm64/atomics/ldxr-widths.c`
- `tests/arm64/loadstore/ldpsw-pair.c`

## Exposed limitations and exclusions

- An initial protection/one-page-unmap fixture did not raise SIGSEGV on baseline
  or candidate. `kernel/calls.c` has an existing read-fault workaround that maps
  readable zeros when a mapped neighbour is within 16 pages. The final fixture
  uses an isolated region and unmaps **both** pages, without changing that
  runtime workaround. This does **not** validate second-page-only faults or
  PROT_NONE enforcement. The initial failure is preserved in local logs.
- `ldxp-stlxp.c`, built `clang -O2 -static -march=armv8.1-a+lse`, terminated with
  native SIGBUS (135). No guest pass or differential claim is made for it.
- Broad Debian runtime coverage and alternate-stack/thread coverage were not
  rerun. Their previously recorded platform-detection/clone3 blockers remain;
  unrun rows are not passes.
- No macOS compiler, Xcode, signing, archive, simulator, iOS device or upload
  validation. Assembly uses existing AArch64 ABI/register conventions; this
  preserves the source design's portability, not proof of an Apple build.
- No attempt to eliminate every load or dispatcher hotspot. No per-entry TLB
  lifetime change, permission workaround, or broader fusion rollout in this pass.

## Reproduction and local evidence

```sh
CC=clang make build-arm64-linux-all
CC=clang make test-arm64-load64-fault-pc
ISH_BIN="$PWD/build-arm64-linux-debug/ish" \
  tests/arm64/loadstore/run-load64-fault-pc.sh
bun tests/arm64/perf-poke.ts BEFORE AFTER debian-arm64-fakefs samples.json 15
make check-docs
git diff --check
```

Benchmark tool does not alter frequency policy; controlled batches above used
an external trap-restored frequency floor.

Local evidence: `/workspace/tmp/ish-opt-pass2-aRVekV/`:

- `inline-pc.json`, `inline-pc-repeat.json`, `inline-pc-samples.csv`, `summary.json`
- `screen.json`, `tlb-fixed.json`, `tlb-fixed-repeat.json`, `rejected-tlb.patch`
- `baseline-compute.data`, `profile-compute.txt`, `profile-startup.txt`,
  `inline-pc-profile.txt`
- `build-inline-pc.log`, `build-final.log`, release/debug gate logs,
  `fault-pc-negative.log`, `fault-pc-final.log`, `fixtures/`

Binary SHA-256:

```text
baseline c333e1f478ad5172d195a4a304febabd5ecf5f6d6e5a74831eb70ea8c90d0ddc
release  985255ac7ccf28d165c13dc15ed21378d21a99b40609187baa8121c920d9084a
debug    af2f48cfacc42883b99037a5ad5bf97f72ffafd050a5ce24fc9fa2e65909ae42
```
