# Startup / steady-state AOT break-even harness — 2026-11-15

## Status and scope

This component implements evaluation tooling, not image production. It never
records, trains, builds, installs, stages, clears caches or changes a user
filesystem. Full/selected images and a staged disposable rootfs are inputs.
Actual integrated measurements and their limitations are in the
[AOT RSS optimisation report](AOT_RSS_OPTIM_2026-11-16.md); this harness design
alone establishes no speedup, RSS improvement or break-even result.

Host used for local correctness: Orange Pi 6 Plus, host-native Linux. Native
`/usr/local/bin/bun` is **1.3.10**, Python `/usr/bin/python3` is **3.13.5**. Guest
Bun **1.4.4** must be independently declared and verified, not equated with the
native reference. Native timings are not evidence about guest performance.

## Work and oracle

Both workloads accept `RECORDS REPEATS WARMUP`, read only the existing input,
validate the fixed first 64 records (ids 0–63), and execute JSON parse, active
filter, group/id sort, JSON serialization/roundtrip and weighted sum. The
available input at
`/workspace/artifacts/ish-heldout-linux-20261115-refresh/inputs/records.json`
contains **96** rows; selection is explicitly the first **64**, not an invented
64-row host file. Parsing reads the entire source, while selection and work use
64 records. Extra rows are frozen by the whole-file hash.

The selected corpus is fixed by `group=id%17`, `active=id%3!=0`,
`value=(101+7919*id)%65521`, and hexadecimal `record-<id>` labels. Canonical
compact JSON with field order id/group/active/value/label has SHA-256
`ebf040e9aff5c2b85741e9348bef69f75aeb1d163a7810d4e74ce7a9de149f3e`.
There are 42 active records, weighted sum **12515468 per work unit**. This is an
independent fixed checksum oracle, not learned from measured guest output.
Python and Bun implement equivalent semantic work, not identical runtime costs.

Exactly one stdout line, including its final newline:

```text
BREAK_EVEN_JSON 64 42 <repeats> <12515468*repeats> <corpus-sha256>
```

Stderr contains two `BREAK_EVEN_PHASE <JSON>` records, ordered warmup then
steady-state, with runtime/version, schema 1, monotonic start/end nanoseconds as
strings, seconds and workCount. Python uses `perf_counter_ns`; Bun uses
`process.hrtime.bigint`. The warmup checksum does not contribute to stdout.
Input read/validation, imports, corpus hashing and telemetry/output are outside
these internal timed loops. Phase timing is **not** an oracle. Both versions and
work counts are checked; missing, extra, reordered or malformed phases fail.

## Caller contract / invocation

```sh
/usr/local/bin/bun tests/arm64/native-aot/break-even.ts /absolute/config.json /absolute/new-evidence-directory
```

`freeze()` is deliberately standalone: heldout's two-variant, fixed-run,
training-manifest-shape assumptions are not reused. It imports heldout's current
ABI/code assertion, SHA-256/file identity checks, exact oracle checks, diagnostics
and environment scrubber. Config is detached before use. Required contract:

- `abi: "3babd760"`, `codeVersion: 15`; every module also declares both.
- `trainingFiles`: explicit nonempty absolute files, including each recording;
  arbitrary training manifests are opaque frozen files, never parsed for fields
  such as `images` or `workloadSha256`.
- `evaluationFiles`: separate explicit absolute files, including `records` and
  each case's `workload`. Training/evaluation canonical-path or hash overlap is
  rejected. Parent must list **all** training/evaluation dependencies, not just
  a manifest mentioning them.
- `metadataFiles`: explicit additional immutable build/selector/version metadata
  (an empty array is allowed). No inferred artifact directory scanning.
- `records`: absolute existing host input; the fixed 64-record prefix is checked.
- `modules`: nonempty array, at most 128, each with `file`, `image`, `recording`,
  `abi`, `codeVersion`, `moduleSha256`, `imageSha256`, `recordingSha256`. All three
  hashes must match exact bytes. Images are assembly and must contain numeric
  ABI `parseInt("3babd760",16)`, following heldout's descriptor check. These
  checks do not replace the parent's actual module/descriptor execution proof.
- `counts`: default `[1,8,32]`; 1–8 distinct integers each 1–64.
- `warmup`: default 1, integer 0–8, retained in raw accounting, never discarded.
- `runs`: 1–10 paired repeats; `timeoutSeconds`: 0.05–60;
  `totalSeconds`: 1–1800, including freeze time, version commands and all samples.
- `cases`: 1–8 unique lowercase alphanumeric/hyphen names, each with `runtime`
  (`python` or `bun`), explicit exact semantic `version`, absolute frozen
  `workload`, and exactly three `variants`.
- Each variant has `name` (`gadget`, `full-aot`, `selected-aot`), absolute
  `binary`, absolute `buildOptions` (Meson buildoptions JSON array), `args`,
  nonempty `versionArgs`, exact newline-terminated `versionOracle`, and
  `imageIndexes` into the frozen module/image list. Gadget selects none; full
  and selected have nonempty image sets. Selected modules come from the full
  set, but assemblies differ: all modules can be retained with fewer selected
  translations per image. No emitter variant exists.
- Meson `guest_arch=arm64`, `jit=false` for gadget, `jit=true` and
  `jit_emit=false` for both AOT variants; `cli_aot` must exactly equal that
  variant's chosen canonical image paths. Gadget's unused `jit_emit` option is
  not constrained. Build options are evidence, not proof that binary bytes were
  really produced by that build; the parent supplies trustworthy matched files.

Arguments are argv arrays, not harness-interpreted shell strings. Every sample
argv must contain separate `{repeats}` and `{warmup}` tokens. Those two tokens
are replaced at execution. Arguments exactly equal to explicit frozen file
aliases become canonical paths, including version-command arguments. Other
arguments (including pre-staged guest paths) remain literal. No implicit
host-to-guest path conversion or rootfs mutation occurs. Parent is responsible
for mapping these host evidence files to the **already staged** guest files and
for supplying a disposable guest filesystem; the harness cannot infer fakefs
content identity from arbitrary shell commands.

A variant argv template for an already-staged guest might be:

```json
{
  "name": "selected-aot",
  "binary": "/absolute/new-selected-build/ish",
  "buildOptions": "/absolute/new-selected-build/meson-info/intro-buildoptions.json",
  "args": ["-f", "/absolute/disposable-root", "/usr/local/bin/bun", "/already-staged/break-even-bun.ts", "/already-staged/records.json", "{repeats}", "{warmup}"],
  "versionArgs": ["-f", "/absolute/disposable-root", "/usr/local/bin/bun", "--version"],
  "versionOracle": "1.4.4\n",
  "imageIndexes": [0]
}
```

This is a template, **not** a runnable config or an assertion about pending
build paths. Caller supplies gadget/full counterparts and module metadata. No
release or old training manifest version is hardcoded. Runtime version processes
run first and exact stdout is checked; workload telemetry checks the same
configured version. Version checks may warm caches, so there is no cold-disk,
no-load, explicit disk-warmup, or cache-control claim.

## Measurement and decomposition

For each case/count/pair, compare gadget vs full and gadget vs selected in
separate fresh-process pairs. Even pair order is baseline then AOT; odd pair
order is reversed. Baseline is measured twice, once against each opponent.
Single-pair runs cannot fully balance order; use at least two for balance.

Each command is wrapped in frozen GNU timeout and GNU time:

```text
timeout -k 1 -s TERM <bounded>s time -f '%e %U %S %M %R %F %x' -o <file> <binary> <args...>
```

GNU timeout without `--foreground` controls the process group; TERM then KILL
covers normal descendants. A bounded outer Bun timeout is a final backstop.
Commands explicitly escaping the process group are unsupported. Three seconds
of grace are reserved within the remaining total budget. No host resource-limit
changes, affinity manipulation or large guest loops are applied. Stdout/stderr
capture is bounded at 4 MiB per stream. Hash verification occurs before and
after commands and at completion, including symlink target identity even when
a retargeted alias has identical bytes. Hash I/O can itself warm disk caches.
Synchronous hashing/output cannot be preempted by the process timeout; provide
bounded local artifact sizes. Total budget is checked around these operations.

GNU time records startup-inclusive wall seconds, user/system CPU seconds,
wait4 maximum RSS KiB and minor/major faults plus exit status. Host command wall
is also retained separately. Metrics measure the command's process tree under
GNU time, not isolated interpreter or AOT memory. Time text remains raw, even if
malformed or nonzero. AOT diagnostics require positive hits, the chosen image
count, zero emitter segments/units/code, and no rejected images.

The phase separation is **a model, not a clean causal decomposition**:

```text
reported = internal warmup seconds + internal steady-state seconds
residual = startup-inclusive GNU host wall - reported
startup estimate = max(0, residual)
```

The signed residual and floor flag are retained. Host and guest/runtime clocks,
wall accounting precision (often 0.01 s), scheduling, input/import/telemetry,
output and runner overhead can make the residual negative or misleading. It
must not be called pure loader/startup time.

`models.json` fits descriptive OLS **host total wall** = intercept + slope ×
workCount separately for gadget and each opponent, preserving fit RMSE and
sample counts. Where identifiable and both slopes are nonnegative with positive
gadget-minus-AOT slope, crossing workCount is
`(AOT intercept - gadget intercept) / (gadget slope - AOT slope)`; otherwise null.
The signed crossing, zero-clipped crossing, measured range and in-range flag are
retained. Fits may be noisy/nonlinear; no causal interpretation, confidence
interval, extrapolated benefit or break-even guarantee is supplied. Synthetic
model tests verify arithmetic, not measured performance.

## Evidence and verification

The new evidence directory contains read-only `manifest.json`,
`manifest.sha256`, per-command `.stdout`, `.stderr`, `.time`, append-only
`raw.jsonl` (including version and failed samples), validated `phases.jsonl`,
`models.json` on success and `PASS` or `FAIL`. Validation failures preserve raw
samples but are not converted into invented successful results. Existing output
directories are rejected. Freeze failure occurs before evidence creation.

Local test command:

```sh
/usr/local/bin/bun test tests/arm64/native-aot/break-even.test.ts
```

Six focused tests cover native Python/Bun correctness for 1/8/32, bounded
workloads/config, malformed phases, detached exact hash/alias identity, opaque
training metadata, image/build contracts, GNU group termination, complete ISH
scrubbing, accounting and model/floor semantics. No large guest loops run.
