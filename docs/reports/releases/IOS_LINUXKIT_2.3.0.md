# ios-linuxkit 2.3.0 source release — 29 September 2026

## Identity and boundary

- Marketing version: **2.3.0**; Apple build number: **813** in all four project
  configurations; annotated source tag: **`v2.3.0`**.
- Master fast-forwards from 2.2.2 (`5ff9f426`) through the three tested integration
  commits below. No wholesale merge of the divergent OpenMinis experimental tree.
- **Native/AOT remains disabled by default.** Ordinary Meson and existing Xcode
  schemes retain the gadget engine. An accelerated iOS app is not included.
- This is a source release, not an Xcode archive, signed binary, TestFlight upload
  or App Store delivery. Linux recordings/images are evidence, not release assets.

## Included changes

| Commit | Change |
|---|---|
| `2cfb42bb` | Alpine 3.24.2 packaging pin and checked atomic download; regular-file poll/select readiness, NULL pselect mask handling, normal-exit TLS lifetime fix. |
| `fdfffa7d` | Separate proc-stat signal snapshot from group/general locks, repairing a captured signal-frame/procfs lock cycle. |
| `25a40aa4` | Selective default-off native/AOT backend, exact fault restart, Linux RW/RX recorder, targeted ELF generator and no-emission linked pipeline; refused native-link ownership and precise native-entry dispatch repairs. |

Code version **10**, tested default pinned/PIC Linux ABI **`3f650e41`**. Earlier
ABI-8/9 images require regeneration; mismatched images are rejected, not replayed
unsafely. The final release metadata/documentation commit also adds the
[local build and iOS rollout plan](../../NATIVE_AOT_BUILD_PLAN.md).

The packaged minirootfs changes from Alpine 3.24.0 to **3.24.2** with a pinned
SHA-256. Existing installed userlands are **not migrated**. Python/zlib packages
were added only to the isolated validation guest, not to the app minirootfs.
The four images target exact musl 1.2.6-r2, BusyBox 1.37.0-r31, Python 3.14.7-r1
and zlib 1.3.2-r0 module bytes; family matching is disabled in these images.

## Validation on merged master

Host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), 12 ARM64 cores (8 Cortex-A720 +
4 Cortex-A520), 16GB-class RAM (~14GiB visible), NVMe, Debian Trixie/Linux
6.6.89-cix, Clang 19.1.7; host-native, not container. Canonical workspace is
`/workspace` → `/home/agent/workspace`; repo `/workspace/projects/ish-arm64-go`.

Fresh gadget release/debug directories `build-arm64-master-aot-off` and
`build-arm64-master-aot-off-debug` both configure **`jit=false`**, no linked
images. `jit_emit=true` is inert when the backend is not compiled. Completed
pre-rotation evidence was retained rather than re-labelled as newly run tests.

| Gate | Gadget release | Gadget debug |
|---|---|---|
| Clean Clang build | Pass | Pass |
| Expanded upstream correctness/lifetime/OOM/CLI context | Pass | Pass |
| Regular-file poll/select | Pass | Pass |
| FCVT vector | Pass | Pass |
| Exact load64 fault-PC/retry | Pass | Pass |
| Proc-mem seek | Pass | Pass |
| Full-width lseek | Pass | Pass |
| Signal poke stress | Pass | Pass |
| Internal continuation | 14/14 | 14/14 |
| Full procfs/exit stress and lock/lookup negatives | Pass | Pass |

Full proc gate: native oracle plus **two guest runs per configuration**, each
**25 seconds / 16 forkers / 6 proc+ps readers**, unchanged watchdog and per-worker
progress requirements. These runs use the staged Alpine 3.24.2 fakefs.

A fresh `build-arm64-master-aot` release executable from merged master links the
four retained final images with `jit=true`, **`jit_emit=false`**. The linked gate
passes: all four accepted, zero rejected, positive installs per module, zero
emitted segments/units/bytes, no emitter object imports, and runtime-off parity.
Image acceptance/install counts alone do not establish executed-instruction
coverage; the earlier same-backend hardware-PC proof is cited below.

Generator tests: **3 pass / 0 fail / 22 assertions**. Atomic rootfs download
failure/preservation tests pass. Documentation links pass across 53 Markdown
files; 29 shell blocks parse, documented test/record targets exist, all version
fields agree, and whitespace checks pass. The runbook's Python/zlib AOT smoke
also runs successfully on the fresh merged-master executable.

Merged-master evidence: `/workspace/tmp/ish-aot-master-cN6hUD/`, pointer
`/workspace/tmp/ish-aot-master-path`; build logs, per-gate stdout, detailed proc
and upstream evidence, `linked-aot/`, and later preservation logs are retained.
Continuation reports: `/workspace/tmp/ish-arm64-internal-continue-fixtures-20260929-210508.md`
and `...-210716.md`.

## Retained integration evidence (same backend revision)

The [host integration report](../audits/AOT_ALPINE_HOST_2026-09-29.md) records
`25a40aa4` evidence, not a second run claimed by this release document:

- O0/O2: **100 actual restart faults each**, **8,576 integer + 2,720 memory
  oracle comparisons each**, precise retries, unmatched-fault fail-stop,
  old-ABI rejection, ownership and SIMD checks.
- Release/debug gadget/native/AOT correctness; PIC linking; full Alpine and
  Debian stress; linked ABI-9 image rejected with safe gadget fallback.
- Hardware breakpoints execute within each of the four final image text sections
  with emitter region NULL, followed by normal workload exit.
- Frozen negative regressions for the integration fixes. Earlier failures are
  preserved and diagnosed in the [prerequisite report](../audits/AOT_ALPINE_INTEGRATION_2026-09-29.md).

Final images/manifest: `/workspace/tmp/ish-aot-integration-hmvkF9/aot-final/`;
backend evidence under that root's `backend/` directory. Generated artifacts are
not committed. The new plan requires a durable complete bundle as follow-up.

## Performance, not a general acceleration claim

Five matched fresh-process samples plus discarded warmup, CPU0 pinned,
`schedutil`, identical fakefs, startup included, warm host page cache:

| Workload | Gadget median | AOT median | Gadget/AOT |
|---|---:|---:|---:|
| 20,000-iteration shell loop | 2,271ms | 2,120ms | **1.071×** |
| Python sum of 200,000 squares | 1,269ms | 1,448ms | **0.877×** |
| 20 × 1MiB zlib round-trips | 4,369ms | 3,839ms | **1.138×** |

These are retained integration measurements, not fresh release re-benchmarks.
Python is ~14% slower in wall time. Four-image release CLI is ~41MiB versus
~762KiB gadget-only. No substantial general speedup, device performance, power
or battery claim. Profile coverage, lookup/slot contention and image size next.

## Publication and remaining work

Master and annotated `v2.3.0` must name the release metadata/documentation commit;
verify remote master and the peeled tag after publication. Default `jit=false`
and the existing Xcode schemes remain unchanged by the release metadata.

The [build/rollout plan](../../NATIVE_AOT_BUILD_PLAN.md) gives working local
commands, durability requirements and Apple gates. Raw PIC recordings may be
reusable only after target ABI/symbol/ISA and guest-identity checks. Linked Linux
ELF images cannot be used directly in iOS. Mach-O generation/linkage, app recovery
wiring, Apple no-emitter build flags, normal signing/sandbox validation, exact
rootfs packaging and physical-device correctness/performance remain uncompleted.
No Apple SDK compile, simulator/device run, signing, sanitizer or distribution
validation is claimed here. Source tagging does not close those gates.
