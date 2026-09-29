# Native JIT / AOT investigation — 29 September 2026

## Disposition

**Do not enable or merge the experimental backend yet.** Its emitter has useful
results, but host-fault recovery is not a safe restart mechanism. Selectively
adopt the applicable CLI recovery hardening instead:

- Store `segfault_was_write` as `bool`, not `int`.
- Assert five recovery offsets against generated `cpu-offsets.h`, including our
  local precise-retry PC slot.
- Make the executable explicitly depend on that generated header.
- Maintain a real-handler regression in `tests/arm64/upstream/jit-crash-context.c`.

These adapt upstream `f7b0a737` and `302c4016`. Our code already derived offsets
from structs; do not import upstream's older block-start retry or Apple-only
handler over the local Linux/Darwin helpers. No version bump, tag, native JIT,
AOT image, memory governor, network backend, or native-abort policy is adopted.

## Frozen inputs and environment

| Input | Revision |
|---|---|
| Local released baseline | `3ec409950429d2b4fa721d5320dec0e8d82c374e` (v2.2.1/build811) |
| OpenMinis master | `b08c12af43f8509be15275aa543d54de8aa28cfc` |
| `perf/ios-general` | `32a283ebf54c233cb1641e84861f55ff3e4575ff` |
| `feat/aot-family-match` | `b2c768fda2e599cfd186f8eb29c94654c9fe24ec` |

Host: Orange Pi 6 Plus, CIX P1/CD8180-class 12-core AArch64 SoC, 16GB-class RAM
(about 14GiB visible), NVMe storage, Debian Trixie; host-native Clang build and
execution. Workspace `/workspace` resolves to `/home/agent/workspace`.

Isolated detached worktrees/evidence:
`/workspace/tmp/ish-jit-investigation-zYDHWD/{native,aot,probes,local-release,local-debug}`.
Pointer: `/workspace/tmp/ish-jit-investigation-path`. Frozen ref inventory,
commit list and file list are retained there. No upstream branch was modified.
The original 55-commit master review is not repeated.

Against upstream master the native branch has 16 ahead commits; the family
branch has 39 (including merges), sharing the native-JIT/AOT base. Distinct
relevant tranches are native emitter `d4c52c5f`, recording `091501c0`, app linking
`a1bac69c`, ABI/build-ID matching `32a283eb`, family matching/slot ownership
`0f4a59fa`, lookup indexing `0cc10736`, compact tables `8ca56caf` and generator
`95fe148f`. No separately named HyperJIT implementation was found in these
checked-out sources/history; that earlier umbrella label is not evidence of
an additional audited engine. Other deferred native-offload/network policies
remain separate from JIT code generation.

## Architecture and integration

The released engine builds a **data stream of precompiled gadgets**, not writable
native instructions. The experimental layer records generator units and replaces
supported runs with generated AArch64 instructions. Unsupported units and TLB
misses return to gadgets. Sixteen guest registers and the cycle counter can be
pinned; lazy registers write through to CPU state. The emitter also provides
loop promotion, SIMD subsets, inline TLB checks, direct links and return-cache
handoffs.

Native allocation is a process-global, 120MiB, never-reclaimed bump region.
macOS uses `MAP_JIT` plus per-thread write protection; the alternate/iOS path
uses Mach remapping and `mprotect` for separate writable/executable views.
Failure falls back to linked images or gadgets. That code is **not proof that a
stock iPhone permits dynamic JIT**; entitlements, signing and actual device
execution remain separate requirements.

PIC recording keeps instructions, gadget/layout keys, relocations, link targets
and promotion metadata. `gen.py` emits Mach-O assembly to be statically linked
and signed. AOT-only builds can avoid executable allocation. Family matching
normalises branch and ADR/ADRP immediates, checks the rest of the unit key and
gadget identity, and supplies per-slot code/data bases. Self-loop matches are
more restrictive. Newer contexts claim one live block per translation slot;
compact tables use 32-bit field-relative references. Images carry a manual code
version plus a structure/convention ABI hash. SHA-256 in image metadata is not
a runtime whole-file authentication check: runtime admission uses build-ID/path
or filename family, followed by block validation.

The frozen branches have explicit Darwin/AArch64 build restrictions and Apple
headers/APIs. Unmodified Linux `meson setup ... -Dguest_arch=arm64 -Djit=true`
rejects configuration, as intended (`native-linux-rejected.log`). Their CLI and
assembly are not a drop-in Linux port. App configuration remains default-off;
JIT emission is also default-off there. No device, Xcode archive or full backend
run was possible here.

## Findings and evidence

### 1. AOT-only fault synchronisation returns without restoring registers

Both tips, `asbestos/guest-arm64/jit.c:jit_crash_sync`, start with:

```c
if (!region || n_pinned <= 0)
    return false;
```

AOT-only deliberately has no `region`. Even a PC inside a registered image
therefore never reaches `in_aot_text` or pinned/promoted register recovery.
Calling the actual function with a synthetic Darwin context, registered image,
and host x19 holding guest x0 produces `sync=0 x0=0` instead of the supplied
`0x12345678`. This is a deterministic function-level reproduction, not an iOS
signal-delivery test. The fix needs AOT text admission independent of allocation,
**but removing this guard alone does not solve finding 2**.

### 2. Native host-fault recovery has no precise restart contract

`jit_crash_sync` copies registers but not the exact guest fault PC or pinned
cycle count. `main.c:crash_handler` then restores the TLS entry PC and reconstructs
an address from gadget registers x7/x10. Native memory emission instead uses
x9 for the host address and x11 for its translation delta; pinned x7 can be guest
x24. The downstream fallback still assumes the gadget ABI. Generated execution
can also have crossed many blocks since the TLS entry snapshot.

An actual emitted `add x0,x0,#1; ldr x3,[x4]` sequence was run on AArch64 with a
TLB-hit backing page protected `PROT_NONE`. The isolated Linux signal adapter
makes that page readable and restarts the sequence with its committed state,
modelling upstream's block replay. It produces **x0=2, not 1**, after one fault;
x3 correctly loads 77. This demonstrates non-idempotence, not an end-to-end
Darwin reproduction. A separate call to actual `jit_crash_sync` restores x0=99
but leaves PC=0x1000, precise-PC=0 and cycle=4 despite host cycle=27.

Before adoption, define native-PC → guest-PC/access/register-state metadata,
identify faultable vs host-helper PCs, preserve exact partially committed state,
and resume only the faulting operation. Cross-block links, promoted-loop
prologues/epilogues, address/write reconstruction, FP state and signal-context
safety all belong to that gate. Blocking `loopmap_lock` in the signal handler
and silent failure of `loopmap_add` allocation are additional unvalidated risks.
The app signal path must be tested separately from CLI recovery.

### 3. Exhaustion makes the dump length exceed the allocation

Both tips: `region_alloc` increments `region_used` before its bounds check and
never rolls back on failure. Starting at 125829120 bytes, a failed 16-byte
allocation leaves 125829136. `jit_report` uses that cursor as the `fwrite` length
for `ISH_JIT_DUMP`. The cursor overflow relative to capacity is reproduced by
calling the actual allocator; an out-of-range dump read is a source-level
consequence, not a crash triggered here. Avoid arbitrary reclaim: contexts,
links and registry entries retain code pointers. Use bounded reservation and
bounded reporting; design cache reclamation separately.

### 4. Old PIC context installs overwrite a live owner; family branch improves it

Older `reg_install`/`aot_install` blindly replace `ctx->blk[idx]`. A deterministic
probe installs a second block at an occupied slot and observes replacement.
Whether normal scheduling exposes a wrong execution requires a concurrent
integration test; the slot is not a unique owner by construction in this tip.

The family branch uses `slot_claim` and releases ownership with
`jit_block_free`; the same probe rejects the second live owner. Retain this
change if experimenting with native JIT, rather than starting from the older
tip. Do not transfer its context structures to the gadget-only engine, which
has no corresponding native slots.

### 5. Performance runner is not a correctness acceptance gate

`benchmark/aojit/guest/run_cases.py:run_case` records nonzero exits in `errors`
but still puts their durations into `times`, computes best-of speedups and can
report equal output for identical error messages. Importing the Python function
and injecting two equal failed runs (exit 1, 1ms) produced `speedup=1.0` and
`same_output=True` alongside `errors=[1,1]`. The error is displayed, not hidden;
results nevertheless require filtering before any speed claim. Volatile cases
also bypass equality checks. Missing needs are skips, not passes.

The suite has alternating mode order, output hashes, holdouts and hit counters,
which are useful ingredients. However `/proc/ish/jit off` affects **new AOT
installs only**; it is not a dynamic-JIT/gadget toggle or a flush of existing
contexts. Best-of-two timings and translation-install hit counts do not measure
native executed-instruction share. The image directory contains a cookbook,
not ready linked images. No fresh end-to-end speedup is claimed by this audit.

### 6. Applicable local fix: boolean footprint and assembly ABI checks

The released CLI already derives C offsets and keeps our more precise retry
slot. It still stores four bytes through `int *` into a one-byte boolean.
The real-handler regression on unchanged local code aborts with status 134,
reporting unexpected writes at bytes **841, 842, 843** (boolean offset 840).
Those bytes are currently padding; this is **not evidence of live-field damage
or an explanation of the previous exit 139**.

The selective patch uses `bool` and asserts C/assembly agreement for `pc`, fault
address, write flag, exit SP and precise retry PC. Eight cases cover
SIGSEGV/SIGBUS × read/write × precise/fallback retry PC, preserve the exact rest
of the frame, and check SP/trampoline and diagnostics. Candidate release/debug
both pass. Mutating the generated boolean offset to zero fails compilation at
the new assertion. Ninja explicitly orders `main.c.o` after `cpu-offsets.h`.

## Executable probe coverage and limits

`probes/prepare.ts` copies each frozen `jit.c`, replaces only Apple platform
includes/restriction with a test adapter, generates actual struct offsets and
builds extracted functions with linker section GC. No entire emulator/backend
was ported. Darwin context tests use a synthetic context. Instruction tests run
the original emitters on the host's actual AArch64 CPU, with separate RW then RX
probe buffers. `probes/run.sh` is the reproduction entry point.

Per tip:

- **8,576 integer comparisons**: 134 assembled templates × 32 operand/flag sets
  × pinned/unpinned modes. Arithmetic, flags/carries, logicals, shifts, selects,
  compares, bitfields, multiply/divide and wide-immediates match native execution.
- **2,720 memory comparisons**: 85 templates × 16 alignments × both pin modes.
  Byte/half/word/doubleword, sign extension, pairs including LDPSW, register
  offset and pre/post writeback compare all eight exercised registers and 4KiB
  of memory against native execution.
- Fourteen tested saturating SIMD forms correctly decline translation (QC
  remains the gadget's responsibility); accepted SMAX matches all sixteen lanes.
- Actual sync/region/slot functions reproduce the findings above; family exact,
  moved, ADR/ADRP and mutated-word matching probes pass.

These are positive bounded semantics tests, **not decoder coverage or proof of
correctness**. They do not exercise every SIMD operation, sustained direct
chaining, translated helper calls, TLB miss/cross-page gadget bailout, all
register-pressure combinations, multi-thread invalidation, FP environment,
complete AOT record/link/load, Mach memory permissions or iOS recovery. Registry
memory growth and hash-only family memo hits remain concerns to test, not newly
proved exploits. Self-modifying-code protection has a P_CODE/write-translation
mechanism; a missing native dirty-page store alone is not proof that it fails.

The first exploratory probe mistakenly left bailout branch placeholders
unresolved and trapped with SIGILL; it was corrected to resolve every bailout
to an explicit test trap. Only final strict/pipefail executions count. Two
delegate attempts timed out without findings and are not review evidence.

## Selective-fix validation

Fresh `build-arm64-jit-audit-release/ish` and
`build-arm64-jit-audit-debug/ish` each pass all seven maintained gates:

- `test-arm64-upstream` (including new real-handler cases, OOM/lifecycle/etc.)
- `test-arm64-lseek-width`
- `test-arm64-poke-stress`
- `test-arm64-fcvt-vector`
- `test-arm64-proc-mem-seek`
- `test-arm64-load64-fault-pc` (18 precise-fault cases)
- `test-arm64-proc-exit-race` (native plus two 25s guest runs, 16 forkers,
  six proc/ps readers, verified per-worker progress and shutdown)

Continuation fixtures: **14/14 release and 14/14 debug**. This script currently
uses `ROOTFS`, not `ROOTFS_DIR`; the initial wrong-variable invocation failed
before tests and was corrected, not counted as a pass. Guest fixtures are copied
through guest tar, never through fakefs backing-store writes. Binary hashes,
logs and baseline/offset-mutation negatives are retained in the evidence tree.

The first Make invocations supplied `ISH_BIN` in the environment, but the recipes
overrode it with `RELEASE_BUILD_DIR`; those logs are preserved separately as
`initial-make-env-*`, not treated as distinct release/debug evidence. All seven
gates were rerun with explicit `RELEASE_BUILD_DIR=build-arm64-jit-audit-{release,debug}`
and `-o build-arm64-linux` (already built, avoiding reconfiguration of debug as
release). The final `local-*` logs identify the intended binaries. Markdown
checks pass for 48 maintained / 52 expanded files, and `git diff --check` passes.

The local CLI handler still assumes x1 is a valid fiber CPU pointer whenever
`in_jit` is set; it does not establish fault-site ownership if a host helper
faults. This is an existing source-level risk, not reproduced or fixed here.
The new byte-footprint test intentionally does not claim broader recovery safety.

No ASan runtime, Darwin host, signed iOS build or phone validation is claimed.
The earlier proc-stress exit 139 remains unattributed. This patch does not claim
to fix it. Version stays **2.2.1/build811**.

## Next experimental acceptance gates

1. Fix and test exact fault recovery first, in emitted and AOT-only modes;
   prove no duplicate register/memory effects and exact signal PC/address.
2. Bound code-region allocation/dumps, handle promotion metadata OOM, and retain
   the newer slot-ownership fixes with adversarial invalidation/fork tests.
3. Preserve local 48-bit/lazy-memory, FPCR/FPSR, poke and precise-fault contracts;
   do not merge the branches' unrelated older kernel/platform changes.
4. Exercise a complete record → compact/link → load pipeline with exact and
   changed modules, stale ABI, cross-page units, duplicate offsets and slot reuse.
5. Require clean outputs/exits, verified AOT/native execution, isolated paired
   timing and device permission/recovery validation before any performance claim.

The emitter is promising enough for a separate experimental port, but that is
larger work than importing useful correctness fixes into the released engine.
