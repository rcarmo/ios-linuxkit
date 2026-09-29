# Durable local AOT / iOS handoff prototype — 29 September 2026

## Scope

Branch `prototype/aot-artifact-kit`, based on published v2.3.0 (`7f2ce5d8`).
No new release, master change, default enablement or signed iOS build. Tools/docs
are committed separately from large generated artifacts. Build source anchor is
`5c855b1a`; subsequent changes add test tooling and handoff guards, not emulator
instructions or ABI. Code version 10 / ABI `3f650e41` remains unchanged.

Host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), 12 ARM64 cores (8 Cortex-A720 +
4 Cortex-A520), 16GB-class RAM (~14GiB available), NVMe, Debian Trixie/Linux
6.6.89-cix; host-native. Clang19.1.7, Meson1.7.0, Ninja1.12.1, Bun1.4.2,
Python3.13.5. CPU0 pinned, schedutil retained for timing. macOS/Xcode/SDK/device
hardware was not available; Apple execution is not claimed.

Durable root: `/workspace/artifacts/ish-aot/2.3.0-prototype/`.

## Preserved and rebuilt

- `seed/`: original JSONL/training logs and workload, original recorder binary,
  exact four module hashes, installed APK DB/world/repositories/keys, source
  snapshot/submodule revisions/toolchain, raw fakefs including WAL, and portable
  guest `rootfs.tar.gz`. No APK binary archives were available; exact installed
  files are retained. No network or package install used during reproduction.
- Original source fakefs was quiescent. Entire backing-file content/mode digest
  remains identical before/after. Export ran on a private clone; the source DB
  was never opened by a guest or snapshot inspector.
- Export/import comparison: **2,066 logical paths** match exactly, including
  guest stat blobs, file bytes/sizes and hardlink group identities, independently
  of host inode numbering. SQLite integrity check passes. Guest Alpine3.24.2,
  musl1.2.6-r2, BusyBox1.37.0-r31, Python3.14.7-r1, zlib1.3.2-r0 confirmed using
  `apk list --installed` (old `apk info -v NAME` prints descriptions on APK3).
- `elf/`: all four assemblies regenerated **byte-identical** to retained images
  from the existing raw recordings, with no retraining. 1991/1318/31649/691
  translations respectively. Family matching disabled.
- Fresh gadget, AOT release and AOT debug builds; `jit_emit=false` for both AOT
  products. CLI sizes: gadget **779,880 B**, AOT release **42,901,240 B**, debug
  **46,118,264 B**. They dynamically use host libraries; not cross-distro static
  binaries. Final delivery includes rebuild source and dependency requirements.
- `kit.ts` stages sealed payloads atomically, checks content/size/mode and rejects
  extra files, sanitises backend environment knobs, refuses existing outputs
  and preserves failed partials. Hash manifests need a separately trusted
  archive checksum; they are not cryptographic signatures.

## Local acceptance

Release and debug on independently restored guests:

- Linked four-image acceptance, zero rejection, workload/output parity with
  runtime off, zero emitted segments/units/bytes, no emitter object imports.
- Upstream correctness/lifecycle/OOM/native-link tests, poll, FCVT, precise
  load-PC, proc-mem/full-width seek, signal poke and **14/14 continuation** each.
- Full procfs stress each: native oracle + 2 guest runs, **25s /16 forkers /
  6 readers**, all-worker progress and unchanged timeout criteria.
- Fresh O0/O2 native harness: **100 actual faults each**, exact PC/restart and
  no duplicate prefix effects; **8576 integer +2720 memory** comparisons each,
  family/SIMD/ownership checks. This harness permits emission; shipped builds
  do not. It validates recovery code, not Apple signal contexts.
- Hardware PC breakpoints in **all four** debug images; PC inside intended image
  range, `region == NULL`, marker/output and normal exit. No install-counter
  substitution for execution proof. `execution.ts` retains generated GDB scripts.
- Actual retained ABI9 musl image linked to fresh current no-emitter build:
  rejected ABI `0e35aac2`, current `3f650e41`, 0 installs, safe gadget shell exit.
  All rejected images turn the backend off, so no JIT stats footer is expected.
- Tools: **9 kit tests /36 assertions**, generator **3/22**, 55-file local docs links.
  Missing Apple target contract fails without publishing output. Guards require
  thin Mach-O ARM64, defined host symbols and matching binary-bound conventions.

Clean-directory rehearsal: extract a committed source archive (no Git checkout),
restore the guest, regenerate all four images byte-identically without retraining,
build a new release CLI and pass linked no-emitter/off parity. Final portable
module extraction repeats the byte-identical generation using host tar, without
executing any retained Linux importer—needed for the later Mac conversion path.
The delivered launcher passes with hostile inherited recording knobs removed and
runtime-off rollback working. Archive/checksum verification is the final delivery
step; fresh source/guest/image/build evidence is retained separately.

Evidence is retained under `linux/evidence/` in the published local bundle.
Build caches originally lived in `linux.partial-2036279`; absolute paths in raw
logs refer to that staging directory, not portable runtime requirements.

## Fresh-process measurements

Five paired samples + discarded warmup, alternated order, same restored guest,
CPU0, warm host filesystem cache; includes CLI/shell startup and proc stats.
Memory is peak RSS via `wait4` in a small C launcher, not Bun's inherited fork
high-water value. Wall times include the identical launcher overhead. Short
startup runs remain noisy; these are not device measurements.

| Workload | Gadget ms | AOT ms | Gadget/AOT | Gadget peak RSS | AOT peak RSS |
|---|---:|---:|---:|---:|---:|
| Startup/echo | 24.1 | 25.3 | 0.955× | 9.3MiB | 11.6MiB |
| Python 200k squares | 1273.3 | 1462.5 | 0.871× | 18.3MiB | 54.1MiB |
| Shell 20k loop | 2287.9 | 2039.0 | 1.122× | 9.6MiB | 11.9MiB |
| zlib 20×1MiB round-trips | 4234.7 | 3747.4 | 1.130× | 21.9MiB | 59.5MiB |

Python regresses ~15% in wall time and nearly triples peak RSS in this case.
Do not call this a broad acceleration. Earlier bundle-independent results had
similar direction. No backend optimisation was mixed into artifact work.

Diagnostic Python 2M-square perf sampling (separate from timed samples): about
1400–1600 user cpu-clock samples, zero lost. Both executions spend ~99% in the
emulator executable. AOT still samples gadget dispatch, TLB and scalar gadget
paths. Short Python run reports ~359k blocks looked up, ~259k AOT installs,
~79k slot conflicts, ~44ms image lookup, ~100ms compile/lookup accounting. These
are different/possibly overlapping counters, **not executed native coverage**.
Anonymous AOT text PCs lack granular function names; perf percentages must not
be presented as exact fallback fractions or proof of one exclusive bottleneck.

Prioritise: (1) executed-native/fallback attribution and startup-versus-steady
cost; (2) bounded context-slot/lookup churn experiments; (3) prune low-value
Python translations and measure size/RSS as well as time; (4) targeted hot-path
coverage. Require correctness and paired measurements per change. Shell/zlib
benefits do not justify enabling Python by default.

## Apple handoff and pending gates

The handoff includes exact guest bytes, raw JSONL, training workload, generators,
source/provenance, contract schema instructions and checked Mach-O generation.
It is deliberately **not** a set of certified iOS images. Actual Apple target
binary/layout observations are required; Linux ELF cannot masquerade as a
Mach-O target, undefined symbols cannot satisfy requirements, and `--abi` is
never used to force compatibility. Generation uses host tar to extract exact
module bytes, not a Linux executable on the Mac. This portable extraction is
tested on Linux; the Apple SDK path itself remains pending. If incompatible, re-record with a matching
Darwin/target-layout recorder and repeat validation. A matching hash alone is
not proof of ISA/context correctness.

Pending on Apple: isolated scheme/Meson flags, shared struct defines, app exact
fault recovery, static Mach-O membership/constructor retention, no-emitter
compile and mapping audit, normal signing/entitlements, exact guest packaging,
physical-device correctness/proc/lifecycle/foreground/memory tests and size/
performance/thermal/energy comparisons. See [artifact kit](../../NATIVE_AOT_ARTIFACT_KIT.md)
and [rollout plan](../../NATIVE_AOT_BUILD_PLAN.md). Existing Xcode schemes stay off.

## Retained failed diagnostics

- Initial GDB scripts referenced obsolete `aot_install(m)` and `t->segs`; then a
  predicate incorrectly treated the module's `trans` pointer as a relative
  offset. Failed logs are kept; final script uses current typed fields and
  rejects GDB predicate errors before counting any pass.
- First memory run used Bun child resource statistics, contaminated by fork
  high-water history. Superseded by the small exec/wait4 launcher; both series
  retained, only corrected results tabulated.
- Stale-image check initially required a stats footer even when all images were
  rejected. Verified backend-off/zero installs and object no-emitter state
  instead. No runtime failure was hidden by this harness correction.
- Delegate fakefs review unavailable (no approved executable model); local
  source review, full logical snapshot comparison and runtime tests used.
