# Validation

Every runtime change needs a focused regression and a broader gate. The Makefile records the supported Linux-host workflows. Broad runtime, CLI and performance scripts write Markdown reports under `REPORT_DIR`; small instruction fixtures can report only their pass marker and exit status.

## Host and test data

The maintained gates run on an AArch64 Linux host. The July 2026 audit used an Orange Pi 6 Plus with a CIX P1 SoC, Debian Trixie and Clang 19.1.7.

Runtime tests modify their fakefs when they install missing packages. Copy the rootfs or rebuild it when package state must be reproducible. Network and package repositories can make broad suites non-deterministic even when the emulator is unchanged.

## Route-netlink networking gate

The opt-in [route-netlink guide](NETLINK_TAILSCALE.md) describes the native/guest
message tests, bounded encoder sanitizers and an isolated official Tailscale
probe. A control-server authentication URL is not proof of account login or
tunnel traffic. The default-off switch, route/notification limits and Apple-device
gates must remain explicit in release evidence.

## App configuration guard

`make test-xcode-gadget-guard` runs the actual Xcode/Meson bridge with fixture
Meson responses for fresh/reused directories and propagated configuration errors.
Existing app targets explicitly select gadgets, disable native emission and clear
CLI AOT images together. Apple compilation, fault-path integration, signing and
device validation remain separate gates.

## Build gate

```sh
CC=clang make build-arm64-linux-all
```

This builds release and debug variants. Treat compiler errors, assembler errors and warnings introduced by the change as failures.

## Runtime gates

| Gate | Command | Scope |
|---|---|---|
| Scalar saturation | `CC=clang make test-arm64-scalar-saturation` | 19,696 native/guest B/H/S/D add/subtract cases, aliases, full destination clearing, cumulative FPSR.QC and NZCV. |
| Gzip compatibility/cleanup | `CC=clang make test-arm64-gzip ROOTFS_DIR=/absolute/disposable-alpine-fakefs` | Same frozen BusyBox native/guest compressed bytes, host/guest inflate, bad-input status and file lifecycle; deterministic corpus and hashes, no package fetch. |
| AdvSIMD FP conversions | `CC=clang make test-arm64-fcvt-vector` | Native AArch64 oracle plus guest widening/narrowing, FP state and decoder masks. |
| Precise load fault PC | `CC=clang make test-arm64-load64-fault-pc` | Native AArch64 oracle plus 18 guest LDR/CBZ/CBNZ cases: alignment, split loads, exact signal PC and retry state after isolated unmap/remap. |
| CPU poke delivery | `CC=clang make test-arm64-poke-stress` | Native oracle and five guest repetitions of acknowledged signals to a compute-bound process. |
| Full-width seeks | `CC=clang make test-arm64-lseek-width` | Static raw-syscall/libc boundary oracle and guest Python sparse-file integration (Debian rootfs with Python required). |
| proc mem seeks | `CC=clang make test-arm64-proc-mem-seek` | One static fixture natively and under iSH; checks `/proc/<pid>/mem` negative, wrapping, `SEEK_SET`/`SEEK_CUR` and rejected `SEEK_END` semantics. |
| Rootfs packaging | `make test-rootfs-download` | Good archive acceptance; failed fetch/hash/architecture and malformed/missing pin preserve the prior bundle. |
| Regular-file readiness | `CC=clang make ROOTFS_DIR=/path/to/fakefs test-arm64-poll-regular` | Native oracle and guest poll/ppoll/select/pselect, access modes/EOF/dup/directory, mixed pipe readiness, epoll semantics and actual shell read. |
| Upstream correctness/lifetime | `CC=clang make test-arm64-upstream` | FMOV512/syscall52 native oracles, timers/signals, precise waits, bounded proc/orphan races, actual-archive JIT/task/OOM/accounting injection, normal-exit TLS handoff, real CLI crash-context checks, CoW/ptrace allocation rollback, failed signal-frame termination, lazy/stack/CoW lock-upgrade revalidation and offload policy/exec guards. |
| Native offload setup | `CC=clang make test-arm64-offload-setup` | Actual-source Linux handler/POSIX adapters; allocation/pipe/thread/spawn rollback, reference/CWD/process preservation, short output, CLOEXEC and low-host-fd cases. Not Darwin app evidence or bounded I/O. |
| Procfs/exit stress | `CC=clang make test-arm64-proc-exit-race` | Actual-archive lock-order/high-PID lookup checks, native stress and two 25s guest runs with 16 forkers, six proc/ps readers, verified progress and shutdown. |
| Release runtime | `make test-arm64-runtime-coverage` | Shell, package manager, C fixtures and language runtimes. |
| Debug runtime | `make test-arm64-runtime-coverage-debug` | Same suite with the debug binary. |
| CLI corner cases | `make test-arm64-cli-corner-smoke` | TUI, DNS, HTTPS, Git, Docker probes and command-line packages. |
| npm CLI packages | `make test-arm64-npm-cli-runtime-coverage` | Moving npm CLI install and startup paths. |
| Multi-manager CLI packages | `make test-arm64-cli-package-runtime-coverage` | npm, Bun and pip lanes selected by `CLI_PACKAGE_MANAGERS`. |
| Internal continue | `make test-arm64-internal-continue-fixtures` | Opt-in executor path and first-call-site fixtures. |
| Node/Bun timing | `make test-arm64-node-bun-perf` | Before-and-after timing and optional executor counters. |
| Pinned performance | `make perf-bench` | Repeated pinned workloads with percentile output. |

Useful parameters are defined at the top of `Makefile`:

```sh
make test-arm64-runtime-coverage \
  ROOTFS_LANES="alpine=$PWD/alpine-arm64-fakefs" \
  REPORT_DIR=/workspace/tmp \
  TIMEOUT_S=180 \
  INSTALL_TIMEOUT_S=1200
```

The default `ROOTFS_LANES` includes both Alpine and Debian. Override it when only one prepared rootfs exists. The Debian target can create `debian-arm64-fakefs`, but it uses `sudo debootstrap`, downloads packages and deletes its temporary output directories; inspect the Makefile recipe before running it.

Cold Go caches can exceed the ordinary timeout because Alpine may ship standard-library source without precompiled archives. Increase `TIMEOUT_S` for a cold toolchain instead of classifying a harness kill as a pass.

## CLI and native fault recovery

`test-arm64-upstream` calls the real CLI handler with eight synthetic
SIGSEGV/SIGBUS contexts to check read/write fields, precise/fallback PC and
SP/trampoline handoff. The [2.2.2 record](reports/releases/IOS_LINUXKIT_2.2.2.md)
contains the original wrong-width store regression.

`test-arm64-native-emitter` uses actual Linux faults to check exact native
restart at O0/O2, prefix side effects, register/memory results and ABI rejection.
Both checks apply to the CLI. Apple app fault recovery needs the implementation
and device tests in [iOS AOT](NATIVE_AOT_IOS.md).

## Runtime coverage stages

`tests/arm64/runtime-coverage.sh` currently checks:

- shell startup, package-manager access, temporary files and symlink retargeting;
- C compilation and execution;
- SysV and POSIX IPC, modern syscall probes, sockets and fd passing;
- ARM64 faults, signal context, barriers, self-modifying code and fused load/store paths;
- Go build, run and test;
- Bun install, TypeScript, test and build;
- Node/npm startup and scripts;
- Python, Lua, Java, Clojure, Rust, Erlang and Zig paths;
- explicit availability results for optional toolchains.

Read the script for the exact current rows. A number in a dated report applies only to that script revision, binary, rootfs and package state.

## Focused low-level fixtures

Standalone source fixtures live under:

```text
tests/arm64/atomics/
tests/arm64/fp/
tests/arm64/fs/
tests/arm64/loadstore/
tests/arm64/proc/
tests/arm64/signals/
tests/arm64/upstream/
```

They contain fixtures for CAS pairs, exclusive monitor clearing, exclusive widths, pair exclusives, AdvSIMD floating-point conversions, `LDPSW`, precise LDR fault/retry PCs, procfs and full-width file seek semantics, CPU poke delivery and per-thread alternate signal stacks. Presence of a fixture is not a passing result; see the current exclusions below. New low-level work should add a similarly small fixture and include it in a repeatable script or runtime row.

The load-PC gate requires a prepared fakefs and a native AArch64 Linux host with 4 KiB pages. Run the debug variant directly:

```sh
CC=clang ISH_BIN="$PWD/build-arm64-linux-debug/ish" \
  tests/arm64/loadstore/run-load64-fault-pc.sh
```

The fixture unmaps both isolated pages before each fault. Its cross-page case checks a first-page fault and successful split access after remapping. Second-page-only faults and `PROT_NONE` enforcement are outside the fixture. A wrong-saved-PC negative mutation must fail; the September pass recorded exit 40, restored the source, rebuilt and confirmed a pass.

A focused fixture should test architectural edge cases relevant to the instruction:

- all implemented operand widths and vector arrangements;
- source and destination register aliasing;
- upper-lane preservation or zeroing;
- condition flags and rounding modes;
- alignment and cross-page accesses;
- faults and reserved encodings.

## Failure rules

A row fails when any of these occur:

1. the command exits non-zero;
2. the harness reaches its timeout or kills the process;
3. `SAFETY-VALVE` appears in a non-diagnostic row;
4. unexpected fault, illegal-instruction or `NETDIAG` output appears;
5. a required row is skipped or silently reported as success;
6. the expected output came from stale artifacts.

Unsupported facilities must be reported as unsupported with a reason. Package absence and rootfs packaging errors are not emulator passes; record them separately from instruction or syscall results.

## Diagnostics during tests

`ISH_ARM64_BLOCK_STATS=1` and `ISH_ARM64_FUSION_STATS=1` intentionally add output. Use them for performance investigations. Disable them in exact-output correctness gates. Fault and PC tracing can also perturb timing and produce large logs.

When a broad row fails, rerun its exact guest command with a bounded timeout. Preserve:

- source revision and dirty-tree state;
- release or debug binary path and checksum when needed;
- rootfs name and relevant package versions;
- complete command and environment;
- exit status and diagnostic excerpt.

## Dated evidence and known exclusions

| Record | Results and scope |
|---|---|
| [July OpenMinis audit](reports/audits/OPENMINIS_AUDIT_2026-07-20.md) | ARM64 instruction/syscall review and native/release/debug fixtures; broad Alpine runs reached 82/83 because Clojure lacked `clojure.main`. |
| [September imports](reports/audits/OPENMINIS_IMPORT_2026-09-28.md) | 55 upstream commit dispositions, failure/lifetime tests and filesystem timings. |
| [Procfs stress](reports/audits/PROCFS_EXIT_STRESS_2026-09-28.md) | Shell-harness defects, numeric PID lookup and captured lock cycle. |
| [Alpine integration](reports/audits/AOT_ALPINE_INTEGRATION_2026-09-29.md) | Regular-file readiness, exit TLS handoff and a separate proc-stat/signal lock cycle. |
| [AOT host integration](reports/audits/AOT_ALPINE_HOST_2026-09-29.md) | Four linked images, exact restart, native oracles and release/debug stress. |
| [Reusable artifacts](reports/audits/AOT_ARTIFACT_KIT_2026-09-29.md) | Restored guest identity, no-retraining rebuild, execution PCs, timings/RSS and Apple handoff limits. |

Earlier broad-suite failures include Debian package detection selecting
Alpine's `build-base`, a glibc alternate-stack fixture exiting during
`pthread_create` after `clone3` returned EINVAL, and native SIGBUS in the
`ldxp-stlxp.c` fixture with the recorded compiler flags. Those runs supplied
no guest coverage for the failed stages. Rerun their exact fixtures before
changing their status; focused passes do not close them.

The nearby-page read-fault workaround is described in
[LIMITATIONS.md](LIMITATIONS.md#memory-and-code-protection). A historical proc
exit139 remains unattributed; the captured later defects have separate evidence.
Sanitizer runtime and accelerated Apple device validation have not run.

## Before commit

Run at least:

```sh
CC=clang make build-arm64-linux-all
git diff --check
git status --short
```

Run the focused regression and the release runtime gate for behavioural changes. Use the debug gate for memory, signal, concurrency and translated-execution changes. Check Markdown links after moving documentation. Version or release changes must also follow [RELEASES.md](RELEASES.md).

## Native/AOT and artifact checks

AOT is available on master and disabled by default. Use
`make test-aot-generator test-aot-kit` for tool validation and
`make test-arm64-native-emitter` for emitted-code/restart fixtures.
`test-arm64-linked-aot` requires a separately linked no-emitter CLI.

The [Linux AOT guide](NATIVE_AOT_BUILD_PLAN.md) gives release/debug build and
runtime commands. The [artifact guide](NATIVE_AOT_ARTIFACT_KIT.md) restores
independent roots, retains runner logs and validates staging before publication.
It also compares complete fakefs logical contents and provides actual-image PC
checks. Image-install counters measure lookup/installation, so keep execution
proof separate.

Run `make check-docs` for local link targets and
`make check-docs-style` for maintained prose. The style checker skips code
blocks and dated/vendor material. It catches selected patterns; read the prose
and verify command semantics as well.
