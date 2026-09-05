# Conditional CPU poke consumption — 5 September 2026

## Scope and baseline

Performance base: correctness commit `a5d571f23d128ef53a48632f611eb0d3b0476ec4`.
Original engineering baseline: `df8903dd`.
See the [seek investigation](../audits/ARM_LINUX_LSEEK_2026-09-05.md) for
correctness evidence and rootfs provenance. Local work only; no release/push.

Host: Orange Pi 6 Plus, CIX P1, 4 Cortex-A520 + 8 Cortex-A720, 16 GB class
RAM (14 GiB visible), NVMe/ext4, Debian Trixie, kernel `6.6.89-cix #15`.
Clang 19.1.7, Meson 1.7.0, Ninja 1.12.1. Release `-O3`, no LTO, no sanitizer,
no added C flags. Debug checks use `-O0`. Both binaries built in the same tree
with the same settings, copying the base before the one-site source change.
Guest Debian 13.5 fakefs copy, Python 3.13; all measured guest execution uses
`ish -f ROOTFS`, Asbestos gadgets and guest syscall translation. No host offload.

Binary SHA-256:

- base: `fae2445d1142ac1ccad8f1e1869a75337d2ef139365d4f68cd6caaccac5d3f84`
- candidate: `c333e1f478ad5172d195a4a304febabd5ecf5f6d6e5a74831eb70ea8c90d0ddc`

## Attribution and candidate

`perf` user-mode cycles are available with `perf_event_paranoid=2`. Initial
499 Hz sampling collected 2,429 samples, no lost samples; 6.65% landed in
`__aarch64_swp1_acq`. Disassembly locates the call in `cpu_run_to_interrupt`:
the inner block loop exchanges the poke flag with false even when unset.
A follow-up profile using exactly `print(sum(range(5000000)))` attributed 7.77%
to this helper on the base; it falls below 1% on the candidate. These percentages
are attribution evidence, not speedup claims. The initial callgraph unwind
printed unsupported-name warnings, so symbol samples plus disassembly, not
speculative full stack attribution, were used.

Candidate: relaxed atomic load before the existing acquire exchange. If false,
no read/modify/write; if true, retain the atomic consume. A store after a false
load remains pending for the next boundary, as does a store after the old
exchange. `cpu_poke` remains sequentially consistent. Interrupt cadence, pointer
ownership, assembly-chain polling, signal delivery and timers are unchanged.
This is portable atomic C supported by Linux and Darwin, not a platform bypass.

## Measurement protocol

```sh
bun tests/arm64/perf-poke.ts BASE CANDIDATE ROOTFS samples.json 15
PERF_WORKLOAD=python-compute bun tests/arm64/perf-poke.ts \
 BASE CANDIDATE ROOTFS confirmation.json 15
perf record -e cycles:u -F 499 -o profile.data -- taskset -c 11 \
 BASE -f ROOTFS /usr/bin/python3 -S -c 'print(sum(range(5000000)))'
```

Fresh interpreter per sample (cold translation caches), two discarded warmups
per binary/workload, warm host page caches. No global cache drop; therefore not
cold-storage numbers. Alternating AB/BA order, same rootfs and exact command,
CPU 11 affinity. Fixed runs temporarily set CPU11's frequency floor equal to
its existing maximum 2,600,198 kHz, then restore 799,865 kHz via an EXIT trap;
governor remains schedutil. Recorded board sensor: 43–44°C. Host load average
roughly 1.6–2.3; background services were not stopped, and a core is pinned but
not isolated. No thermal throttling observed, but other sensors/OS noise are
not eliminated. Diagnostics and perf are absent from timed runs.

Bun collects monotonic wall time and child resource usage through the
`timeout/taskset` wrapper. CPU time includes wrapper overhead. RSS is the child
process-tree high-water statistic, not a precise live guest allocation census.
Every run must exit zero with exact stdout and empty stderr. The I/O workload
writes/reads 4 MiB through guest tempfile; it is warm-cache I/O, not disk latency.

### Controlled series 1 — 15 pairs per workload

Wall values in ms, p10 / median / p90 (nearest-rank index); CPU columns are medians.

| Guest workload | Base wall | Candidate wall | Base user/sys ms | Candidate user/sys ms | RSS MiB base/candidate |
|---|---:|---:|---:|---:|---:|
| shell startup | 18.966 / 19.836 / 21.540 | 17.358 / 19.951 / 23.600 | 12.935 / 6.872 | 12.189 / 6.199 | 26.98 / 26.98 |
| Python startup | 237.106 / 237.876 / 242.520 | 236.797 / 237.879 / 240.186 | 218.477 / 19.218 | 215.172 / 21.708 | 30.30 / 30.30 |
| Python sum(range(5M)) | 4407.303 / 4467.187 / 4560.588 | 4330.315 / 4369.621 / 4454.480 | 4447.357 / 19.018 | 4346.937 / 19.902 | 31.12 / 31.12 |
| Python tempfile I/O | 742.782 / 745.773 / 755.396 | 743.533 / 750.324 / 755.156 | 708.914 / 34.907 | 702.218 / 43.703 | 34.34 / 34.34 |

### Independent confirmation — 15 compute pairs

Base wall p10/median/p90: 4406.919 / 4456.919 / 4566.664 ms.
Candidate: 4344.444 / 4389.503 / 4504.189 ms.
User/sys medians: 4438.582/17.971 → 4365.316/19.910 ms.
RSS high-water: 27.51 MiB for each. Cross-series RSS levels differ with the
measuring wrapper; there is no memory-saving claim.

Across the two controlled compute series: 23/30 faster pairs; mean paired
improvement 1.768%. A descriptive 10,000-resample paired bootstrap with seed
12345 gives 0.964–2.515% for the mean; this does not eliminate shared-host noise.
Median improvements are 2.18% and 1.51%. Retain the small atomic-load change
for this modest compute benefit. Startup and I/O have no demonstrated benefit.
The preliminary 11-pair schedutil run was noisy (compute median 4498→4415 ms;
shell 26.90→31.20 ms) and was not sufficient to accept the candidate alone.
No other source optimisation was trialled; no broad speedup is claimed.

## Correctness and remaining failures

- New `poke-stress.c`: native and guest identical static binary, 8 fork rounds,
  128 acknowledged SIGUSR1 deliveries each to a syscall-free compute loop.
  No sleep-based assumption or swallowed timeout. SHA-256 static binary:
  `8eadbe7e992ce9186f34705a8f251ac8d8653ed89bce525514b93f07820e18a3`.
  Base, candidate and debug pass; final runner repeats five times (5,120
  deliveries per guest gate). Alarms and outer kill-after timeout are failures.
- Full-width seek/Python integration, FCVT, proc-mem gates pass. Release/debug
  builds and local Markdown checks pass.
- Nine existing C fixtures extracted unchanged from runtime-coverage heredocs,
  compiled Clang `-O0 -static -fno-pie` and run through guest base/candidate/debug:
  hello, SysV IPC, DC ZVA, signal ucontext, precise fault PC, fused LDR/CBZ fault,
  CCMP/NV, barriers and self-modifying code all pass. These host-compiled
  fixtures supplement, not replace, the blocked guest-compiler broad suite.
- Existing per-thread altstack fixture fails identically before/after at
  pthread_create (status 3). Native passes. Trace shows clone3 (435) returns
  EINVAL before a thread exists; alternate-stack behaviour was not exercised.
  Next action: compare glibc clone_args with `sys_clone3` validation, minimise
  the rejected flags. Do not claim full thread/signal coverage.
- Broad suite still stops in Debian/Alpine package-name detection before C
  coverage. No complete language/runtime matrix claimed; no ASan/TSan/fuzzing
  claim. Fix detector blank-line handling in a separate harness change.

## iOS checks still required

No iOS cross-build or device run here. On a signed device build, run poke stress
and the seek integration, then sustained guest computation interrupted by
signals, foreground/background cycles, thread creation and memory pressure.
Check wakeup responsiveness, signal-return state, battery/thermal effects and
sandbox file limits. Timing, scheduler and memory pressure differ from this
Linux host; no device speedup, signing or App Store acceptance is established.
