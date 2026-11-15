# Crate/OpenMinis follow-up and Bun 1.4.4 — 15 November 2026

## Scope and references

Baseline: ios-linuxkit `abe413540d4ff8b4ad6b52447ab80f824c58495f`
(2.5.2/build826), after a clean 22-commit fast-forward from `111fc64a`.
Work branch: `investigate/crate-openminis-20261115`.

- Crate v0.1.1: `bd2efb4ae9d8567d581b1e6c2313ac21a112f1f2`.
- OpenMinis/ish-arm64: `c5c264da19148512167aa493d7bd4e50aae1b74e`
  (PR #48); kernel follow-up starts at `e6521d9c`.
- Earlier local audits: [September import](OPENMINIS_IMPORT_2026-09-28.md)
  and [memory follow-up](OPENMINIS_MEMORY_FOLLOWUP_2026-09-30.md).

Crate is a full-system VM with debugger-supplied executable mappings on iOS,
not a no-JIT replacement for our Linux userspace ABI. No Crate Rust build or
head-to-head benchmark was performed. Any future Rust build must use oak oil.
Our default gadget interpreter, linked no-emitter deployment, exact guest
identity checks, fault recovery and memory-accounting ownership remain intact.
No upstream memory governor, clustering policy or speculative TLB-hoisting
change was imported.

## Implemented tranche

| Change | Origin and boundary | Verification |
|---|---|---|
| Paired native-fault snapshots | Local implementation after translator comparison. Adjacent backing slots use STP; sparse pins and promotions use their actual `greg_off()` slots. Snapshots remain unconditional: chained predecessors can leave backing slots stale. | Canary/promotion/sparse tests, native memory oracle and real-fault restart gates. Default checkpoint: 16 stores becomes 9 instructions, saving 28 bytes per memory access. |
| Write-only destination allocation | Local bounded allocator change. Avoid dead preloads for full overwrites, retain source ordering, use weights and MOVK/BFM read-modify-write reads. This is not a lazy writeback or dirty-state rewrite. | MOVZ falls from 3 to 2 words; MOVK retains its preload. Seeded streams exercise aliases, flags, eviction and RMW. |
| Vector DUP(element) | Local emitter coverage; invalid lane/Q encodings still fall back. | 290 legal lane/Q/alias cases against native and C oracles; unchanged NZCV/FPSR and rejection checks. |
| Bounded proc formatting | Adapted from OpenMinis `abaa7144`. Do not treat `vsnprintf`'s required length as initialized stack bytes. | Baseline-failing oversized/error/allocation-failure probes. |
| Poll membership locking | Adapted from `7710898f`. Lock traversal against list deletion. | Baseline-failing lock/lookup regression. This does **not** retain fd objects or make ADD check-and-insert atomic. |
| Emitter capacity guards | Adapted from `9d5e397a`. Failed writes cannot be followed by out-of-bounds branch patching. | Empty/failed patch and full-buffer conditional/CBZ/TBZ probes; removing the guards reproduces assertion failure. |
| Recording capacity policy | Completes part of `99effcae`: target-only recording now defaults on; explicit `ISH_JIT_RECORD_TARGET_ONLY=0` is retained for diagnostics. Reject exhausted local `region_exhausted` and upstream `region_full` headers in generation. Apple recording requires a known non-exhausted arena for every module, not only Go. | Generator regressions; default recording, explicit override and invalid-shard fallback exercised. Existing exact-module and two-shard rules retained. |
| Signal ABI assertions | Alignment already correct locally; add `f78049c5`'s offset invariants. | ARM64 release/debug builds assert reserved area offset 288 and mcontext offset 176. No signal-layout change. |
| Bun musl 1.4.4 | Official artifact, packaging pins, strict recorder workload versions and current guides updated together. | Packaging/downloader tests, gadget/JIT smoke and fresh linked no-emitter execution below. |

The emitter convention changes bump `JIT_CODE_VERSION` from 12 to 13. The
observed Linux PIC/pinned ABI is `d5483d56`; older ABI fixtures are rejected.
This number is not an observed Apple target contract and must not be copied
into an app contract to bypass device-layout observation.

## Paired emitted-code measurements

Host: Orange Pi 6 Plus, CIX P1 ARM64 12-core SoC, CPU 11 (reported maximum
2.6 GHz), 16 GB-class RAM / about 14 GiB visible, NVMe storage, Debian Trixie,
kernel `6.6.89-cix`. Host-native runtime; `/workspace` resolves to
`/home/agent/workspace`. Compiler: Debian Clang 19.1.7.

`tests/arm64/native-aot/speed-bench.c` runs actual emitted code against a
frozen baseline `jit.c` and candidate `jit.c`, with the same current headers,
wrapper and compiler flags. Each sample performs 10,000 warmups and 4,000,000
timed calls, each containing 32 LDR/ADD or MOVZ/ADD pairs. Both variants assert
identical final checksums. Both pin the same guest registers.

The final series has 21 pairs per workload, pinned with `taskset -c 11` and
alternating baseline/candidate order. No other build/test was launched by this
session during that series. The host remained live: frequency, other services
and thermals were not controlled. An earlier 11-pair series overlapped tests
and is retained as exploratory evidence, not used in this table.

| Workload | Baseline → candidate words | Median elapsed, ms | Median paired elapsed reduction | Individual paired range |
|---|---:|---:|---:|---:|
| Memory/checkpoint | 1,699 → 1,475 | 732.40 → 593.09 | **19.41%** | 12.24% to 25.91% |
| Destination overwrite | 162 → 130 | 76.94 → 80.29 | **−1.20%** | −36.01% to 34.04% |

The paired percentage is the median of `1 - candidate/baseline`, not the
ratio of the two medians. Memory code shrinks 13.18%; overwrite code shrinks
19.75%. The overwrite timing is noisy and **does not establish a speedup**.
These loops isolate emitter work; they do not establish Bun/Pi/Go application,
whole-emulator, Crate-comparison or iPhone speedups.

Reproduce the emitter comparison from the repository root (choose a new OUT):

```sh
OUT=/absolute/path/to/new-evidence
mkdir -p "$OUT/baseline-source/asbestos/guest-arm64"
git show abe41354:asbestos/guest-arm64/jit.c > "$OUT/baseline-source/asbestos/guest-arm64/jit.c"
make build-arm64-native
for version in baseline candidate; do
  include=.
  test "$version" != baseline || include="$OUT/baseline-source"
  clang -std=gnu11 -O2 -g -DGUEST_ARM64=1 -DISH_JIT=1 -DISH_JIT_NO_EMIT=1 \
    -I"$include" -I. -Ibuild-arm64-native-release \
    -ffunction-sections -fdata-sections -Wno-unused-function \
    -Wl,--gc-sections -pthread tests/arm64/native-aot/speed-bench.c \
    tests/arm64/native-aot/call.S -o "$OUT/speed-$version"
done
# Repeat 21 paired samples per workload, alternating variant order.
taskset -c 11 "$OUT/speed-baseline" memory
taskset -c 11 "$OUT/speed-candidate" memory
# Repeat for overwrite; retain every JSON result, not only favourable samples.
```

The standalone harness explicitly maps its test code executable. It is a
host emitter test, not the no-executable-memory deployment gate below.

## Bun 1.4.4: fresh Linux no-emitter proof

Official `bun-linux-aarch64-musl.zip` checksum, verified against release metadata
and `SHASUMS256.txt`:

```text
57f01b02ac7113c61c935f28f38c2eab849e3c970dab21f8b640a87b1ef65009
```

A copied Alpine 3.24.2 fakefs with Python 3.14.7 and required C++ runtime
libraries was packaged; existing user filesystems were not replaced. The generic
glibc benchmark lane remains separately pinned at `BUN_VERSION ?= 1.3.13`.

Fresh `AOT_INCLUDE_BUN=1` recordings use ABI `d5483d56` and exact staged files:

| Image | Translations |
|---|---:|
| musl | 2,380 |
| BusyBox | 1,326 |
| Python | 31,655 |
| zlib | 691 |
| Bun 1.4.4 | 43,848 |

All five recording headers report `region_exhausted: false`. Generated assembly
is directly in the recording directory (`aot_*.S`), not an `images/` child.
Build `build-arm64-bun144-aot` uses `jit=true`, **`jit_emit=false`** and those
five assembly inputs. Final gate command:

```sh
make test-arm64-linked-aot RELEASE_BUILD_DIR=build-arm64-bun144-aot \
  ROOTFS_DIR=/workspace/artifacts/ish-bun144-20261115/rootfs \
  AOT_RECORD_DIR=/workspace/artifacts/ish-bun144-20261115/recordings \
  EVIDENCE_DIR=/workspace/artifacts/ish-bun144-20261115/linked-final TIMEOUT_S=240
```

Use `RELEASE_BUILD_DIR` for this Make target; an `ISH_BIN` Make variable does
not override its recipe. Earlier failed invocations selected the default binary
or an incorrect assembly directory; those were invocation failures, not a
successful deployment gate.

Final result: `linked-aot-gate-ok: 5 modules used; emission disabled;
runtime-off parity`. Both modes print `BUN_TRAIN_OK` and `AOT_TRAIN_OK`.
AOT diagnostics show five images in use, zero rejected, and nonzero hits for
all five (Bun: 66,646 hits in the initial successful run). Runtime compilation
counters are **segments 0, units 0, code 0 KB**. The backend object passes the
forbidden emitter-symbol check; gadget-off mode installs zero AOT translations.
This is workload parity, not exhaustive application equivalence or a
process-wide loader/mmap audit.

## Regression receipts

- Release and debug builds and both `make test-arm64-upstream` runs passed.
  Includes the existing 512 FMOV and 52 syscall cases, signal/timer/flags,
  lifetime/failure-injection tests, and new proc/poll probes. These existing
  lifetime tests do not close the newly identified concurrent-fd gaps.
- `make test-arm64-native-emitter` passed at O0/O2: 8,576 integer and 2,720
  memory differential cases, 110 actual restart faults, 2,048 seeded streams,
  290 DUP cases, snapshot canaries, capacity guards and stale-image rejection.
- Preservation/oracle harness also passed GCC 14.2 ASan+UBSan. Clang's ASan
  runtime archives were absent, so the sanitizer run used GCC, not Clang.
- Packaging, generator, Apple-tooling fixtures, kit, downloader and Go-rootfs
  tests passed. Go packaging has one expected platform-specific skipped test.
  Apple fixture tests are not Xcode/app/device execution.
- `make check-docs` passed for 78 Markdown files; `git diff --check` passed.
- A focused independent static review of the emitter and new stream/preservation
  tests returned no blocker. The earlier broader review timed out without a
  result; neither review substitutes for the runtime gates.

Host evidence roots (not redistributable release bundles):
`/workspace/artifacts/ish-crate-openminis-20261115` and
`/workspace/artifacts/ish-bun144-20261115`. Timing series:
`speed-long-paired-quiet.jsonl`; final native, release/debug upstream,
packaging, sanitizer, recording-policy and linked logs are retained there.

## Remaining opportunities: audited, not silently imported

1. **Shared PIC slow paths (`e6629fee`): absent and the clearest next image-density
   candidate.** Port emitter stubs, recording schema/relocations and ELF/Mach-O
   generation together. Our extra `kit.ts`/`record-apple.ts` relocation consumers
   must understand stub references rather than treating them as symbols. Requires
   another ABI bump, link/refusal/poke tests and fresh recordings. Upstream's
   roughly halved-image claim has not been reproduced locally.
2. **Profile-driven selection (`0820c6d6`): partial.** `gen.py --keep` already
   removes links into omitted translations. Module-offset block profiling and
   hot-block selection are absent; import the whole producer/selector flow,
   measure held-out coverage and preserve gadget fallback. Remaining recording
   arena usage reporting from `99effcae` is not imported here.
3. **Stale-TLB marking (`21b420b3`): absent.** The existing generation checks
   remain necessary. Removing them requires the complete attach/detach,
   invalidation and miss-ordering protocol, with remap/CoW/exit concurrency tests.
4. **Gadget-fusion bound (`b79f979c`): motivating peepholes are also absent.** Do
   not add a dormant flag as an alleged speedup. Bring guards with any future
   three-/five-instruction peepholes. **SHA512 fallback (`329056a6`)** is also
   absent: software helpers exist, but host feature dispatch and four gadget
   fallbacks need force-software and alias/preservation tests. Compatibility,
   not a demonstrated speedup on SHA512-capable hosts.
5. **Fd/poll/socket lifetime (`41e162c0`, `e74295ed`): missing coherent follow-up.**
   Retain under the fd-table lock for epoll, poll and select; balance every exit;
   protect cleanup/freelist/destruction with consistent lock ordering. Do not
   permanently retain member fds or break deferred stale-event storage. Include
   exactly-once socket/pipe host-fd handoff, socketpair RLIMIT deadlock and accept
   lock-order repairs, plus original-object retention through publication (a
   descriptor can be closed and reused). Keep local netlink ownership intact.
   Later `63ec0524`'s orphaned-wait/group-exit guards are a dependency. Test
   deterministic close/reuse, zero/one fd slots, EFAULT, stale events and exit.
   Broader borrowed socket-pointer/peer-handshake safety remains separate.
6. **Poll deadlines/backoff (`a57f8ce3`, `ae0e32bf`): partial/absent.** Regular-file
   exclusion/readiness already exists. Repair finite waits with one monotonic
   deadline, including repeated rejected events and timeout zero, not repeated
   one-second subtraction. Kqueue hangup-only EV_CLEAR and bounded unlocked
   backoff require Darwin, level/edge/oneshot, MOD and multiple-waiter tests.
   Membership lookup plus insertion must also become atomic to fix racing ADDs.
   These primarily avoid pathological spin/hangs; no normal-path speedup measured.
7. **Memory policy:** preserve `anon_page_count` and precharged-ticket ownership.
   Governor/clustering commits remain unapproved; no reduction in validated
   safety should be exchanged for an upstream throughput claim.

Apple Silicon/Xcode must observe the actual target layout, regenerate Mach-O
images, inspect final app membership/no-emitter imports, validate signing and
stale-image fallback. Physical iPhone/iPad Bun/Pi and Go crash/lifecycle/CoW,
invalidation, sustained timing, footprint and thermal gates remain open.
The Linux training workload does not establish that either reported crash is
fixed. Arbitrary newly compiled Go programs still use gadgets unless matched.
