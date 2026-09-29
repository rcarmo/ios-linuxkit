# Alpine 3.24.2 native/AOT host integration — 29 September 2026

## Scope and result

Selective port from `fix/jit-aot-safety` (`b50541ee`) onto
`integrate/native-aot-alpine`, after the prerequisite fixes at `2cfb42bb` and
proc-stat deadlock repair `fdfffa7d`. No wholesale merge, release bump, master
change or iOS backend enablement.

**Linux AArch64 AOT-only execution works against the new Alpine userland.**
Release/debug executables built with `-Djit=true -Djit_emit=false` accept all
four linked images, execute their text, and pass the workload. There is no
runtime native emitter mapping. Host loader executable mappings are normal.
This is not iOS signing/device validation and not a substantial general speedup.

Default Meson remains `jit=false`; `ISH_JIT=0` preserves gadget-only execution
inside an opt-in build. Existing Xcode configuration remains unchanged: no
`ISH_ENABLE_JIT` switch or app image linkage was added. A shipping app remains
a separate integration/validation step.

## Host and userland

Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), 12 ARM64 cores (Cortex-A720/A520),
16GB-class RAM (~14GiB visible), NVMe root filesystem, Debian Trixie host-native.
Clang 19, Meson/Ninja. Performance measurements pin CPU0 (policy0, max ~2.6GHz),
retain the `schedutil` governor, and run serially. No global frequency changes.

Guest Alpine 3.24.2 aarch64, musl 1.2.6-r2, BusyBox 1.37.0-r31,
Python 3.14.7-r1, zlib 1.3.2-r0. The isolated fakefs includes the Python/zlib
packages; the application minirootfs download does **not** add Python by itself.
The earlier [integration report](AOT_ALPINE_INTEGRATION_2026-09-29.md) records
archive checksum/provenance, procfs deadlock and prerequisite repair evidence.

## Port and bugs exposed by integration

- Optional compile/lifecycle/link/context/proc/CLI fault-recovery hooks,
  native BLR return-cache layout (including `blr x30` ordering), inherited
  precise-access checkpoints, fail-stop unrecognised native faults.
- Preserve local 48-bit guest addresses, OOM returns, lazy memory, precise
  gadget fault PC, invalidation and internal-continuation paths.
- Linux native emission uses memfd-backed separate RW/RX aliases, never RWX;
  cache maintenance covers the writable and executable aliases.
- **Refused PIC link ownership:** initial port omitted the donor's incoming
  dependency registration for alternate code pointers. GDB captured a refused
  target being disconnected/freed while the alternate still pointed to it.
  Both normal and eager-prechain paths now register/reassign the edge so target
  invalidation clears it; repeated refusal cannot duplicate the list node.
  This was a port defect, not a newly discovered upstream defect.
- **AOT native-dispatch classification:** the donor's `@region` relocation to
  image base plus a 128MB heuristic misclassified neighbouring ELF host gadgets
  as native code, skipped their first 20 words and corrupted guest state during
  musl startup. PIC whole-block dispatch now uses explicit `native_entry`;
  interior stream dispatch spills and enters cold. No approximate native range
  check in PIC/AOT dispatch. First ABI-9 linked images faulted in `_dlstart`;
  regenerated ABI-10 images pass.
- **Initial allocation failure:** optional `jit_block_init()` originally ran
  after a failed `gen_start()` allocation in this port. Existing OOM injection
  exposed it; initialization now stays inside the successful-allocation branch.
- Host fixtures derive `ISH_JIT` from the candidate compile commands, keeping
  C struct ABI consistent. CLI crash tests link the actual candidate TLS and
  trampoline instead of duplicate stand-ins.
- Init exit reports before mm teardown, including `exit()` as well as group
  exit. Children cannot truncate the process-wide recording. Runtime
  `ISH_JIT_AOT_ONLY=1` also avoids creating an emitter region.

Code version **10**, default pinned/PIC ABI **`3f650e41`**. ABI-8 experimental
and ABI-9 intermediate images must be rebuilt. No claims of cross-host binary
compatibility; images are generated for their corresponding host executable.

## Targeted pipeline

`tools/jit_aot/targeted.ts` discovers actual module paths (Python `INSTSONAME`,
canonical musl/BusyBox/zlib), instead of hard-coded Python3.12. Four serial
workload runs; every status and completion marker checked. No bare `wait`,
background status loss or piped workload status. It validates a single intended
module, nonempty translated segments, identical ABI, and module bytes before/
after recording. Failed stages leave diagnostic files but **no final manifest**.
Existing output directories are refused. Final manifest contains package
inventory, binary/module/recording/image/workload SHA-256 values.

`gen.py`/`compact.py` retained from the donor, with optional ELF sections,
relocations, local labels and `.init_array` constructors. Mach-O remains the
original default dialect; no third-party `tqdm` dependency. The targeted recipe
disables family matching in generated images; it does not rely on another
version's instructions. Family matching code has oracle tests, not new-userland
cross-version production validation.

Final training: a 1,000-iteration shell loop, Python arithmetic/JSON import,
1MiB zlib compress/decompress equality, explicit completion marker.

| Image | Module | Recorded translations |
|---|---|---:|
| musl | `/lib/ld-musl-aarch64.so.1` | 1,991 |
| BusyBox | `/bin/busybox` | 1,318 |
| Python | `/usr/lib/libpython3.14.so.1.0` | 31,649 |
| zlib | `/usr/lib/libz.so.1.3.2` | 691 |

Final AOT CLI is about **41MiB**, versus ~760KiB gadget CLI; this includes tables,
not just native text. Generated images/recordings are evidence artifacts, not
checked into source or added to the app bundle.

### Reproduce

```sh
CC=clang make build-arm64-native
make test-aot-generator test-arm64-native-emitter
make record-arm64-aot ROOTFS_DIR=/absolute/alpine-fakefs \
  AOT_RECORD_DIR=/absolute/new-output

A=/absolute/new-output
CC=clang meson setup build-arm64-aot-release --buildtype=release \
  -Djit=true -Djit_emit=false \
  -Dcli_aot="$A/aot_musl.S,$A/aot_busybox.S,$A/aot_python.S,$A/aot_zlib.S"
make build-arm64-linux RELEASE_BUILD_DIR=build-arm64-aot-release
make test-arm64-linked-aot RELEASE_BUILD_DIR=build-arm64-aot-release \
  ROOTFS_DIR=/absolute/alpine-fakefs AOT_RECORD_DIR="$A"
```

Use a fresh build directory or explicitly `meson configure` it: Make setup
arguments do not silently reconfigure an existing directory. Python3 is used
only for the existing upstream generator; orchestration/tests use Bun/shell.

## Verification

Evidence root `/workspace/tmp/ish-aot-integration-hmvkF9/`:
`backend/`, final recordings `aot-final/manifest.json`.

- Native emitter at O0/O2: **100 real host faults each**, all five modes,
  scalar/pair/SIMD read/write, exact restart, no repeated prefix side effects,
  cross-allocation entry, relocated AOT, actual Linux CLI handler and extracted
  dispatch/TLB-flush code. Unmatched native PCs exit139; old ABI rejected.
  A separately linked real ABI-9 musl image is also rejected by the current
  no-emitter CLI; shell/proc output succeeds with zero AOT installs.
- **8,576 integer + 2,720 memory oracle cases each** O0/O2; SIMD QC fallback,
  family moved/add/ADR/ADRP/mutation, live slot ownership tests pass.
- Deterministic refused-link lifecycle test: 100 reassignment/retry checks;
  frozen missing-ownership mutation aborts134 at its first ownership assertion.
- Generator: three tests, 22 assertions; empty/malformed recordings and failed
  recorder do not publish images/manifests.
- Upstream correctness (512 FMOV /52 syscall matrix, signal/timer/lifecycle,
  task rollback, OOM, CLI context, exit-current), regular poll, 18 exact load-PC,
  FCVT and **14/14 continuation** pass for gadget/native/AOT release/debug on
  Alpine. Extra PIC release/debug upstream gates pass. Debian gadget and AOT
  release/debug also pass these focused gates (its differing module bytes
  fall back safely; no Debian acceleration claim).
- Full procfs stress: native oracle plus two guest runs per lane, each
  **25s /16 forkers /6 readers**, unchanged watchdog and success checks:
  Alpine gadget, PIC-native, AOT release/debug; Debian AOT release/debug pass.
- Final linked images: all four accepted, no rejection, positive installs per
  module, zero emitted segments/units/bytes. Runtime-off workload parity passes.
  Installation counters are **not** executed-instruction coverage.
- Hardware PC breakpoints independently hit final musl/BusyBox/Python/zlib image
  text with `region == NULL`; each workload subsequently exits normally.
- Old ABI-9 linked startup failure retained in `backend/aot-fault.log`; PIC
  ownership failure/GDB logs also retained. Early recorder development failures
  (BusyBox readlink syntax, Python soname discovery and Bun buffer conversion)
  produced no manifest. One zlib conditional-GDB run exceeded 45s due to debugger
  overhead; the isolated Python command then hit its AOT PC and exited normally
  under a 120s diagnostic bound. Not counted as a runtime stress pass.

## Fresh-process performance, final images

Five measured matched pairs plus discarded warmup, alternated order, CPU0,
release builds, identical fakefs, warm host file cache. Each run is a fresh CLI
process; startup/lookup cost included. Checksums/assertions and AOT-only markers
required on every run. `tests/arm64/native-aot/measure.ts`; raw samples/logs in
`backend/measure-final/`. These short synthetic cases are not whole-userland or
iOS benchmarks, and training coverage is intentionally limited.

| Workload | Gadget median | AOT median | Gadget/AOT |
|---|---:|---:|---:|
| 20,000-iteration ash loop | 2,271ms | 2,120ms | **1.071×** |
| Python sum of 200,000 squares | 1,269ms | 1,448ms | **0.877×** |
| 20 × 1MiB zlib round-trips | 4,369ms | 3,839ms | **1.138×** |

**No substantial general speedup demonstrated. Python regresses ~14% in wall
time.** Prior-image timing had the same direction (1.091× /0.861× /1.143×).
AOT install/slot contention is high during Python; costs also include lookup,
precise checkpoint spills and fallback. These are profiling leads, not an
established exclusive cause. Next optimise hot-image coverage/size and measure
execution/fallback time rather than assuming more recorded blocks are faster.

## Remaining platform gates

Linux success does not authorise shipping the backend on iOS. Still required:
Apple SDK compile/link, opt-in Xcode ABI flags and app recovery hooks, signed
Mach-O image packaging, app-bundle size budget, device execution/fault tests,
stock sandbox entitlement checks and device measurements. No JIT entitlement
workaround is part of this work. No ASan/TSan claim. Keep app/default engine off
until those gates pass; master remains 2.2.2/build812.
