# OpenMinis imports — 28 September 2026

Baseline: ios-linuxkit `61719f81` (v2.1.3). Reviewed OpenMinis master `b08c12af`,
69 commits since the July-audited `89269e6f`. Ports are semantic where trees
have diverged; no wholesale branch merge, app-version change or experimental
native JIT/AOT/network import. This is a running import record, not a release.

## Tranche 1: reproduced correctness and filesystem/wait fixes

| Upstream commit | Imported change |
|---|---|
| `ace2cdbf` | Consistent scalar FMOV immediate bit6 convention; all 256 encodings per width. |
| `2427e6ea` | SIGEV_SIGNAL timer targets the group leader, not nonexistent PID zero. |
| `5d84ecd4` | Consume a selected pending signal before re-entering sigtimedwait. |
| `08eeb259`, `f8983200` | AArch64 open flag values and final-component O_NOFOLLOW resolution. |
| `565f4f82`, `2da30b04` | waitid status/code decoding and child UID capture before reap. |
| `19c690c3` | Release pids_lock and reset PID slot on task allocation failure. |
| `74d6a0df` | Synthesised archive root retains S_IFDIR. |
| `a4b5d7e3` (path subset) | Reject exhausted path output before slash/NUL overflow; existing base-path check retained. Fork hook assessed separately. |
| `359a268d` (adapted) | Our bind-mount stat branch gets host device/block metadata matching fstat; guest inode/ownership overlays retained. |

Source: https://github.com/OpenMinis/ish-arm64 (commit IDs above).

### Evidence

Host-native Orange Pi 6 Plus, CIX P1 (8 Cortex-A720 + 4 Cortex-A520), 16 GB-class
RAM, NVMe, Debian AArch64 Linux 6.6.89-cix, Clang 19.1.7; Debian fakefs. No native
offload. Evidence `/workspace/tmp/ish-import-20260928-aXyaSD/`; earlier baseline
failures `/workspace/tmp/openminis-check-20260928-XiXZr8/`.

- Unchanged baseline release/debug: FMOV 64/256 per width, 385 failed assertions;
  timers dropped; O_DIRECTORY/O_NOFOLLOW incorrectly succeeded; delayed selected
  signal timed out. Identical native probes passed.
- Candidate release/debug: FMOV 512/512; timer and signal probes pass;
  upstream syscall matrix **52 pass / 0 fail** on each build.
- `CC=clang make build-arm64-linux-all` passes after the ports.
- `tests/arm64/upstream/run.sh` compiles static native/guest fixtures, rejects
  skipped assertions, and requires exact result markers. Existing realfs path
  diagnostics are permitted; they are not interpreted as test success.
- Upstream tests are imported from `ace2cdbf` (FMOV) and `b08c12af` (syscalls).
  FMOV's erroneous prose "8x" corrected; instruction stream and oracle unchanged.
  Local bounded timer/flag/signal probes supplement the upstream matrix.
- Task-allocation failure and archive/path edges need dedicated fault-injection
  or targeted follow-up beyond this guest matrix; no such coverage is inferred.

## Boundaries

Preserve our native proc-mem seek semantics, FPCR/FPSR conversion gates,
full-width lseek, atomic poke and inline precise-PC improvements. Our crash
field offsets and CLI argument bounds already address overlapping upstream work.
Do not import the reverted poll/epoll changes as if they survived on master.

Further lifecycle, OOM, sleep, accounting and filesystem improvements remain
under review. Memory/fork governor policy and Apple 16 KiB clustering need
separate assessment; Linux evidence cannot establish iOS footprint behaviour.
No macOS build, signing, archive or device validation is claimed.
