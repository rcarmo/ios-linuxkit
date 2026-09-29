# Native/AOT local build and iOS rollout plan

Status: **2.3.0 / Apple build 813**, 29 September 2026. Master includes the
selective backend port; ordinary Meson and Xcode builds remain gadget-only.
A [durable artifact prototype](NATIVE_AOT_ARTIFACT_KIT.md) now implements the
local bundle and checked recording handoff stages; its dated evidence records
what passed and what remains Apple-only.
The local Linux pipeline below works. The Apple section is an **implementation
and validation plan**, not a working accelerated Xcode recipe.

## 1. Deliverables and current boundary

- [x] Merge prerequisite correctness fixes and the default-off backend into master.
- [x] Prove linked Linux AOT-only execution for Alpine 3.24.2 with runtime
  emission disabled; retain gadget fallback and an off switch.
- [x] Record reproducible local commands and explicit iOS carry-over gates here.
- [ ] Package a durable local image/rootfs bundle with complete provenance and
  a launcher; the current generated images are temporary evidence, not release assets.
- [ ] Profile size, startup and Python regression before expanding coverage.
- [ ] Implement and validate Apple ABI/build/recovery/image integration.
- [ ] Sign, install and test an accelerated physical-device archive, then decide
  whether to distribute it. Keep reference schemes/defaults unchanged meanwhile.

There are two special builds: a **recorder** (`jit=true`, emission allowed) and
an **AOT-only executable** (`jit=true`, `jit_emit=false`, images linked). Guest
Alpine programs do not need recompilation. Updating Alpine by itself does not
enable acceleration. Uncovered or incompatible guest code uses gadgets.

Measured host result: shell **1.071×**, zlib **1.138×**, Python **0.877×** versus
gadgets (five matched fresh-process pairs, startup included). Python takes
about 14% longer. Four-image CLI size is ~41MiB versus ~762KiB gadget-only.
Neither a broad speedup nor an iOS speedup has been established. See the
[host report](reports/audits/AOT_ALPINE_HOST_2026-09-29.md).

## 2. Reproduce on a local Linux ARM64 host

### Hardware, dependencies and isolation

Validated host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), 12 ARM64 cores
(8 Cortex-A720 + 4 Cortex-A520), 16GB-class RAM (~14GiB visible), NVMe root,
Debian Trixie/Linux 6.6.89-cix, Clang 19.1.7, host-native. No global CPU governor
changes or privileged executable-memory workaround. An x86 Linux build or
emulation of this host is not covered by this result.

Use Clang, Make, Meson, Ninja, pkg-config, SQLite/libarchive development packages,
Bun, Python 3 (existing upstream generator), curl, file, tar, binutils, coreutils
and util-linux (`taskset`). See [Linux development](LINUX_DEVELOPMENT.md).
Initialise submodules. Choose fresh build/output directories and a **disposable**
fakefs. Do not edit fakefs backing files directly or run two guest writers on
one root. Stop all guest processes before snapshotting it.

```sh
git checkout v2.3.0
git submodule update --init --recursive
CC=clang make build-arm64-linux RELEASE_BUILD_DIR=build-arm64-gadget
CC=clang make build-arm64-native NATIVE_BUILD_DIR=build-arm64-recorder
make test-aot-generator test-rootfs-download test-arm64-native-emitter
```

Fresh directories are important: Make's setup arguments do not reconfigure
existing Meson directories. Inspect `meson configure BUILD`, or use an explicit
`meson configure` when changing options. `CC=clang` must be explicit on a new
build; Make's built-in `CC=cc` otherwise selects GCC on the validated host.

### Prepare the guest

Follow the checksum-verified Alpine **3.24.2** import in
[Linux development](LINUX_DEVELOPMENT.md#create-and-run-a-fakefs), using
`build-arm64-gadget/tools/fakefsify` as the importer. Set absolute paths:

```sh
ROOT="$PWD/alpine-aot-fakefs"          # already imported, disposable fakefs
A="$PWD/aot-alpine-3.24.2"             # must NOT exist yet
G="$PWD/build-arm64-gadget/ish"
R="$PWD/build-arm64-recorder/ish"
```

The stock minirootfs does not contain Python. Configure guest DNS through the
guest (choose a reachable resolver) and install the target packages there:

```sh
DNS=1.1.1.1  # replace if this resolver is not reachable on your network
"$G" -f "$ROOT" /bin/sh -ec "printf 'nameserver %s\\n' '$DNS' > /etc/resolv.conf"
"$G" -f "$ROOT" /bin/sh -ec 'apk update; apk upgrade; apk add python3 zlib'
"$G" -f "$ROOT" /bin/sh -ec 'cat /etc/alpine-release; apk info -v musl busybox python3 zlib'
```

The tested installed versions were musl **1.2.6-r2**, BusyBox **1.37.0-r31**,
Python **3.14.7-r1**, zlib **1.3.2-r0**. Repository contents move: these commands
are a pipeline reproduction, not a guarantee of those exact package bytes at
a later date. For exact replay, retain the quiescent fakefs (metadata database
and data directory together), or the exact APKs and dependency set, plus hashes.
Stop on package/download errors; do not record an incomplete guest.

### Record and link

```sh
CC=clang make record-arm64-aot NATIVE_BUILD_DIR=build-arm64-recorder \
  ROOTFS_DIR="$ROOT" AOT_RECORD_DIR="$A"
test -s "$A/manifest.json"

CC=clang meson setup build-arm64-aot --buildtype=release \
  -Djit=true -Djit_emit=false \
  -Dcli_aot="$A/aot_musl.S,$A/aot_busybox.S,$A/aot_python.S,$A/aot_zlib.S"
make build-arm64-linux RELEASE_BUILD_DIR=build-arm64-aot
make test-arm64-linked-aot RELEASE_BUILD_DIR=build-arm64-aot \
  ROOTFS_DIR="$ROOT" AOT_RECORD_DIR="$A"
```

The serial recorder discovers actual sonames, validates four workloads and
unchanged module bytes, and publishes `manifest.json` only after all succeed.
It records shell arithmetic, Python arithmetic/JSON, and zlib round-trip work;
coverage is intentionally narrow. Family matching is disabled. A failed output
directory contains diagnostics, **not** a usable completed bundle. Preserve it
and select a fresh output directory when retrying.

### Run, verify and measure

```sh
ISH_JIT=1 ISH_AOT_FAMILY=0 ISH_JIT_STATS=1 \
  ./build-arm64-aot/ish -f "$ROOT" /bin/sh -ec \
  'python3 -c "import zlib; a=b\"aot\"*10000; assert zlib.decompress(zlib.compress(a))==a"; cat /proc/ish/jit'

# Runtime rollback, same executable and guest:
ISH_JIT=0 ./build-arm64-aot/ish -f "$ROOT" /bin/sh

# Fresh processes; identical guest; alternated order; discarded warmup:
PERF_CPU=0 PERF_RUNS=5 bun tests/arm64/native-aot/measure.ts \
  "$G" "$PWD/build-arm64-aot/ish" "$ROOT" "$PWD/aot-measurements"
```

Require four accepted images, zero rejected, positive module installs, zero
emitted segments/units/bytes and exact output parity. The linked gate also checks
`ISH_JIT_NO_EMIT` and absence of emitter imports. **Installs are not an executed
instruction denominator**: use hardware PC breakpoints in each image (as in the
host report), then explicit execution/fallback counters before coverage claims.
The normal executable/shared-library mappings remain executable; “no emitter”
is not a claim that the process has no executable memory.

Preserve correctness in release **and** debug, using matching fresh Meson builds:

```sh
CC=clang HOST_CC=clang make RELEASE_BUILD_DIR=build-arm64-aot \
  ROOTFS_DIR="$ROOT" DEBIAN_ROOTFS_DIR="$ROOT" \
  test-arm64-upstream test-arm64-poll-regular test-arm64-fcvt-vector \
  test-arm64-load64-fault-pc test-arm64-proc-mem-seek \
  test-arm64-lseek-width test-arm64-poke-stress \
  test-arm64-internal-continue-fixtures test-arm64-proc-exit-race
```

Several historical target names use `DEBIAN_ROOTFS_DIR` even for Alpine; set both
variables. Keep the proc stress at **25s, 16 forkers, 6 readers, native plus two
guest repetitions**; do not shorten its timeout or count timeout as a pass.
Run the no-emitter linked gate for debug too. Retain the native-emitter O0/O2
restart/oracle regressions and stale-image rejection when changing the backend.

### Make the local result durable (next implementation tranche)

Store outside `/tmp`: source revision/tag, submodule revisions, full build
options/compiler versions, recorder binary, JSONL, generated assembly and logs,
completed manifest, final binaries, exact guest snapshot, checksums and test/
benchmark results. Add a launcher with explicit root/image selection and a
runtime-off option; do not silently select old images. Hash the whole bundle.
The current manifest already records module/recorder/recording/image/workload
hashes and inventory, but is not a complete rootfs/toolchain snapshot. Future
package upgrades require new recording/validation for changed targets.

Optimisation order: measure executed native versus fallback time, startup/image
lookup and Python slot contention; shrink low-value translations; expand only
representative hot paths; repeat matched benchmarks and correctness gates after
each change. Do not turn all code recording on and assume more images are faster.

## 3. What carries from Linux to iOS?

| Artifact | Carry-over rule |
|---|---|
| Guest Alpine binaries and training workloads | Reuse exact module bytes and checksums. Package/soname changes require new coverage and identity checks. |
| Raw PIC `.jsonl` recordings | **Potentially reusable**, not yet demonstrated across Linux/Darwin. Keep originals and prove the target ABI/symbol/ISA gates below. |
| Linux ELF `.S`, `.o` or linked CLI | **Not directly usable in an iOS app.** Generate target Mach-O assembly and assemble/link/sign it with the Apple SDK. |
| Test oracle outputs and manifests | Useful provenance, not Apple execution or signing evidence. |

A recording embeds native instructions and C/assembly offsets, not just portable
guest IR. `jit_abi()` in `asbestos/guest-arm64/jit.c` hashes code version,
prologue/entry, pinned register count, context constants, CPU/frame/MMU/TLB/block
layout and AOT table sizes. Current Linux code version is **10**, tested ABI
**`3f650e41`**. Target flags, platform structure alignment, debug/assertion
configuration and backend changes can affect layout. The hash is a rejection
guard, **not** proof of cross-platform execution correctness.

`gen.py` resolves recorded host names against `nm -g` of the binary supplied as
its second positional argument. It handles Mach-O leading underscores and emits
Mach-O sections, PAGE/PAGEOFF relocations and module constructors by default.
Unnamed code relocations cause generation to fail; missing gadget-key symbols
need separate checking (the artifact-kit wrapper requires defined symbols).
Compare translation counts and coverage, not just generator exit status. `targeted.ts ... macho` alone still
supplies its **recorder** binary's symbol map; it is not a Linux-to-iOS converter.

For an Apple bootstrap binary with compatible exported symbols, the intended
conversion is (template; not validated here):

```sh
python3 tools/jit_aot/gen.py "$A/musl.jsonl" "$APPLE_SYMBOL_BINARY" \
  "$ROOT/data" "$APPLE_OUTPUT/aot_musl.S" --name musl --format macho --family ''
# Repeat for busybox, python and zlib, then assemble/link with the target SDK.
```

Before accepting reuse, check **all** of: matching ABI and entry/pinning
conventions on the actual Apple target; exact guest hashes; complete required
host-symbol definitions/relocations; AArch64 ISA/calling convention and x18/
platform register obligations; precise fault recovery and FP/SIMD preservation;
Apple page sizes and cache/invalidation behavior. Do not target arm64e/pointer
authentication by assumption. Do not pass `--abi` to relabel an incompatible
recording or disable the rejection guard. If any gate fails, produce recordings
with a matching Darwin ARM64 recorder (or a proven target-layout recorder), then
regenerate. A macOS recorder's ABI must also be compared with the actual iOS
app; Darwin alone does not guarantee equality.

## 4. Apple implementation and acceptance sequence

1. **Establish the reference build.** On an Apple Silicon Mac, install Xcode,
   SDK/CLI tools and Meson/Ninja. Build the existing gadget-only schemes first.
   Select team/bundle/app-group identifiers owned by the developer. Record Mac
   model/SoC/RAM, macOS/Xcode/SDK versions and target iPhone/iPad model, SoC,
   RAM and iOS version. None of these Apple hardware/toolchain/device gates has
   been run on the Linux host; use [iOS application](IOS_APPLICATION.md).
2. **Add an isolated accelerated scheme/configuration.** Wire the Meson backend
   through `app/xcode-meson.sh` with `jit=true`, **`jit_emit=false`**. Apply the
   matching `ISH_JIT`/frame ABI defines to every app/library translation unit
   that touches shared structures. Existing `cli_aot` links the CLI only; add
   app-target assembly build membership/linkage explicitly. Leave current
   schemes and default engine off.
3. **Implement app recovery integration before executing images.** Linux CLI
   recovery in `main.c` is not app recovery. Connect exact checkpoint/TLS/native
   entry handling to the real Apple app fault path, preserve signal/Mach context,
   and fail-stop unmatched native faults. Compile assertions alone do not prove
   context restoration. Test fault/retry without replaying prior side effects,
   chained image entry, invalidation, OOM and thread lifecycle with actual device
   contexts. Do not copy Linux `ucontext` field assumptions into Apple code.
4. **Prove or reject recording portability.** Build a target-layout/symbol
   bootstrap, compare ABI and symbol definitions, generate Mach-O, retain counts
   and hashes, then link all four images. Inspect the final archive for image text,
   constructor retention/dead stripping and normal static code signing. If
   cross-host reuse fails, re-record rather than spoofing the ABI.
5. **Enforce no runtime emission.** Keep compiler-level `jit_emit=false`; do not
   rely solely on `ISH_JIT_AOT_ONLY`. Audit imports and runtime mappings. No JIT
   entitlement, dynamic downloaded executable image, MAP_JIT exception or RWX
   workaround is part of this plan. Standard signing/sandbox acceptance is a
   separate gate; Linux memfd emission is recorder-only.
6. **Package exact guest identity.** Musl/BusyBox must match the bundled rootfs;
   Python/zlib images need the corresponding installed packages. The current app
   minirootfs includes neither a Python installation step nor automatic upgrades
   of existing users. Provide safe fallback for old users/package upgrades;
   test rejected/missing images, never overwrite users' data to force a match.
7. **Sign, install, test the exact archive on hardware.** Verify image acceptance
   and execution per module with zero emission; stock entitlement/sandbox rules;
   native-oracle outputs; precise faults; procfs stress; shell/Python/zlib;
   terminal/resize, foreground/background, memory pressure, package upgrades and
   off-switch rollback. Simulator compilation is useful but not device proof.
   Quantify app size, launch time, sustained speed, memory, thermals and battery
   against the same gadget-only build/userland. Keep Python's known regression
   visible. Test both shipped schemes if both are to be distributed.
8. **Distribute only after review.** Use a fresh Apple build number for every
   upload, even for the same marketing version. Update Fastlane identifiers and
   scheme/team settings before any upload (inherited lanes target upstream iSH).
   Record archive/signing/device evidence and publish assets deliberately. A
   Git tag, including `v2.3.0`, is only a source release.

## 5. Completion gates for future work

Local bundle: a fresh checkout can verify hashes, build/run without training
again against retained exact guest bytes, pass parity/off-switch/stress checks,
and reproduce recorded performance within stated variance. Apple prototype:
Mach-O accepted **and executed** on a signed physical device, no runtime emitter,
precise recovery/stress gates pass, safe fallback works. Distribution: reviewed
size/performance/energy trade-offs, default/rollback decision, exact archive
provenance and valid signing/upload configuration. Until then, “backend merged”
must not be reported as “accelerated iOS app shipped.”
