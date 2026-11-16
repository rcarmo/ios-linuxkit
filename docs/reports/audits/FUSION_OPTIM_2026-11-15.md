# ARM64 address-fusion stream optimisation — 2026-11-15

## Scope and implementation

Base: `master` / `6fefcaee3ca89185232b15aa485025e5b0d286ff`.
Host: `orangepi6plus`, host-native Linux AArch64; workspace `/workspace` resolves
to `/home/agent/workspace`. Only `asbestos/guest-arm64/gen.c` and the four
assigned new test/report files were written. No commits, gadget additions,
`jit.c` edits, build-definition edits, or parent build-directory writes.
Concurrent kernel changes belong to another delegate.

Implemented a bounded improvement to an **existing supported fusion**:
`ADRP Xd, page; ADD Xd, Xd, #imm12` (64-bit, no shift/flags, `Xd != XZR`).
The old `gadget_fused_adrp_add` stream is three words: gadget, packed
register/immediate, page address. The generator now computes exactly the
existing gadget's `(page + imm12) & 0xffffffffffff` and emits the existing
`gadget_adr` with its packed 48-bit address: **two words (24 → 16 bytes)**.
Both variants perform one architectural register write and one dispatch;
the new variant eliminates the runtime address addition and one stream load.
No newly unsupported DECREF/compare/branch gadget is assumed.

Read the generator, `math.S` ADR definition, `memory.S` fused ADRP+ADD
and memory templates, `control.S` compare/branch definitions, gadget header,
`asbestos.c` compilation loop, and `jit.c` unit recording before implementing.
The matcher remains unchanged; this is not a claim of a new instruction pattern.

## Profile and frozen-recording evidence

Inputs:

- `/workspace/artifacts/ish-portable-policy-20261115/profile-training/final.profile`
- Corresponding `final.log`
- Frozen JSONL manifest corpus `/workspace/artifacts/ish-fixes-20261115/bun15-recordings`
- Exact module bytes `/workspace/artifacts/ish-heldout-linux-20261115-refresh/rootfs/data`

Representative **file-offset** bytes from `/usr/lib/libpython3.14.so.1.0`:

| Pair offset | ADRP word | ADD word | Profile block offset | Guest instructions | Ordinary entry weight |
| --- | --- | --- | --- | --- | --- |
| `0xaea7c` | `90000000` | `9133a000` | `0xaea60` | 11 | 12,196 |
| `0xb472c` | `f0001f62` | `913a2042` | `0xb4720` | 10 | 11,929 |
| `0xf470c` | `d0ffffe0` | `91330000` | `0xf46f0` | 11 | 9,286 |

The new test verifies these exact bytes and profile rows. These pairs are also
executed in the production-generator differential matrix.
Scanning contiguous instruction spans in 85,177 four-column profile rows finds
8,071 matching pair occurrences, with summed ordinary entry weight 66,187.
Total ordinary instruction-weight is 13,785,145. This scan is **candidate
coverage, not exact dynamically executed fusion counts**: ordinary chaining
under-samples entries, and inline-followed branches can make linear spans differ
from executed paths. No exact coverage or workload speedup is inferred.

`final.log` reports `adrp_add=9995`: compile-selection count only.
Frozen JSONL `keysym` entries contain 6,608 `gadget_fused_adrp_add` selections:
Bun 5,812, musl 109, BusyBox 55, Python 619, zlib 13. Also compile/recording
selections, not dynamic hotness. They establish that the supported fusion is
used by the actual corpus rather than just synthetic inputs.

## Semantics, boundaries and restart

- Preserves existing fused gadget's 48-bit mask, including signed ADRP offsets,
  page rounding, unsigned wrap, immediate 0/1/4095 and all 31 writable GPRs.
- NZCV, SP, other GPRs, FP state and all other CPU bytes remain unchanged;
  whole-CPU canaries are compared against an independent signed-offset model.
- IP still advances eight guest bytes. Production `jit_step_end` records
  `n=2`, both instruction words, and stream range `start=0,end=2` (old end=3).
  This source does not have a separate `n_expected_inst` field: its guard is
  the consumed-byte calculation, accepting only four/eight-byte units.
- Production `JIT_MAX_UNITS` overflow fallback is exercised at the last
  remaining slot and beyond; no extra unit is introduced by the change.
- Different destination/source, W ADD, shifted ADD, ADDS and SUB are refused.
  A failed next-page peek compiles only ADRP, preserving the first instruction's
  four-byte progress; the subsequent fetch emits the production GPF stream
  with precise PC `0x2000`. Cross-page successful lookahead is also covered.
- Existing page/segment budget consumption is unchanged (the same two guest
  instructions were already fused). No guest memory access, TLB lookup,
  speculation, or memory-fault checkpoint is introduced or hoisted.
- Allocation and partial-block failure policy remains production `gen`'s OOM
  handling; this change does not replace it or add a new allocation path.

## Verification and before/after measurements

Isolated compilation/evidence: `/workspace/tmp/ish-fusion-optim-build`.
Compiler runs serially; no unbounded make/Ninja or Rust builds. Only the parent's
`cpu-offsets.h` is read and copied into isolated output. The preserved baseline
is `gen-base.c` in that directory; the parent generator is never rolled back.

Commands:

```sh
bun test tests/arm64/native-aot/fusion-optim.test.ts tests/arm64/native-aot/generator.test.ts
FUSION_GEN_SOURCE=/workspace/tmp/ish-fusion-optim-build/gen-base.c FUSION_WORDS=3 \
  bun test tests/arm64/native-aot/fusion-optim.test.ts
EVIDENCE_DIR=/workspace/tmp/ish-fusion-optim-build/existing-gates \
  bun tests/arm64/native-aot/run.ts
```

Results: new and baseline fusion tests pass at **O0 and O2**, each with 2,328
native-gadget/model differential cases; production unit metadata, bounds,
refusals, failed peek and CPU preservation pass. New+generator suite: six tests
pass. Existing `run.ts`: all layout/restart/preservation executables pass at
O0/O2; its Bun suite reports **10 pass, 1 skip, 0 fail**. The skipped test is
exact profiling: `PROFILE_EXACT_BIN` was not supplied. Existing gate log is
`existing-gates.log`; final focused logs are `final-tests.log`, `final-fusion.log` and `base-tests.log`.
Unused unrelated gadget symbols abort in the probe; selected ADR/fused bodies
are extracted **verbatim** from production assembly, not reimplemented models.

Benchmark: seven samples of two million production-stream calls per executable.
Final unpinned O2 medians: baseline 7,865,266 ns, new 7,473,997 ns. These include
the common call/preservation wrapper and are not application timings.
Alternating CPU-11 runs demonstrate instability: first pair medians
8,636,594 → 7,405,858 ns, second pair 6,896,272 → 7,600,842 ns.
**No statistically reliable timing gain is claimed.** Definite improvement is
one fewer 64-bit stream word per matched unit (33.3%); native JIT still emits
the original guest ADRP and ADD, so no native machine-code reduction is claimed.

## ABI/AOT integration and limits

Current source/recording ABI: codeversion 15, `0x3babd760`.
Neither existing gadget's operand ABI/semantics changes. Previously emitted
streams remain valid with the same gadgets; this does not require migration
of old executable gadget streams. However newly generated affected units have
a different stream size and first gadget symbol, changing their AOT keys.
**Regenerate affected recordings/images for complete AOT hit coverage**;
conservatively regenerate the frozen corpus at parent integration. Parent owns
any codeversion/ABI policy bump; none was made here. The frozen ABI15 corpus
was read only and was not rewritten.

No held-out end-to-end workload speedup, RSS change, full application rebuild,
exact-profile replay, sanitizer run, or fresh AOT image was measured. Parent
AOT/RSS and kernel work remains independent. This is a tested, useful stream
compaction with inconclusive timing benefit, not a broader optimisation claim.
