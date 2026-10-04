# Build and run AOT on Linux ARM64

Ahead-of-time (AOT) builds link recorded ARM64 translations into the emulator.
The Linux pipeline supports Alpine's musl, BusyBox, Python and zlib. Ordinary
builds use the gadget interpreter; enable AOT explicitly with the commands below.

Use the [artifact guide](NATIVE_AOT_ARTIFACT_KIT.md) to freeze, restore and share
inputs, or the [iOS guide](NATIVE_AOT_IOS.md) to prepare Apple builds. Existing
Reference Xcode schemes use gadgets. The isolated Apple bootstrap compiles the
native/no-emitter backend but forces execution off. Apple image integration and
device tests are incomplete.

## Choose a build

| Build | Meson options | Use |
|---|---|---|
| Gadget | `jit=false` | Default engine and comparison baseline. |
| Recorder | `jit=true`, `jit_emit=true` | Run training workloads and record native translations. |
| AOT-only | `jit=true`, `jit_emit=false`, `cli_aot=…` | Link images at build time; use gadgets for uncovered code. |

Guest programs need no recompilation. The images depend on their exact module
bytes and the emulator's application binary interface (ABI). Keep the recorder,
recordings, guest files and source revisions together. Regenerate images after
an incompatible emulator change; retrain after changing the target guest files.

The September 2026 prototype measured shell **1.12×**, zlib **1.13×** and Python
**0.87×** versus gadgets. Its AOT executable was 40.9MiB; Python peak RSS rose
from 18.3MiB to 54.1MiB. These are short host workloads. Raw samples and test
scope are in the [artifact report](reports/audits/AOT_ARTIFACT_KIT_2026-09-29.md).

## Prepare the host

Use an AArch64 Linux host. The tested host is an Orange Pi 6 Plus: CIX P1
(CD8180/CD8160), eight Cortex-A720 and four Cortex-A520 cores, 16GB-class RAM
(about 14GiB visible), NVMe/ext4, Debian Trixie, kernel 6.6.89-cix. Tests used
Clang19.1.7, Meson1.7.0, Ninja1.12.1, Bun1.4.2 and Python3.13.5.

Install the system packages:

```sh
sudo apt install clang make meson ninja-build pkg-config git curl file tar \
  python3 binutils coreutils util-linux gdb libsqlite3-dev libarchive-dev
```

Install [Bun](https://bun.sh/docs/installation) separately. Python runs the
upstream image generator; Bun runs orchestration and tests. Hardware-PC checks
need GDB. Optional profiling needs `perf` and permission to sample user events.

```sh
git clone --recurse-submodules https://github.com/rcarmo/ios-linuxkit.git
cd ios-linuxkit
git checkout v2.4.1
git submodule update --init --recursive
CC=clang make build-arm64-linux RELEASE_BUILD_DIR=build-arm64-gadget
CC=clang make build-arm64-native NATIVE_BUILD_DIR=build-arm64-recorder
make test-aot-generator test-aot-kit test-rootfs-download
```

Use fresh build directories. Make does not reconfigure existing Meson options.
Inspect them with `meson configure BUILD`; choose a new directory to change
compiler. Set `CC=clang` explicitly: Make's built-in `CC=cc` otherwise selects
GCC on the tested host.

## Create a training guest

Run each code block with a shell that stops on errors. Start at the repository
root. Put artifacts outside the checkout so a later seed can record a clean Git
revision. Choose new paths; do not overwrite an installed userland.

```sh
set -eu
WORK="$HOME/ish-aot-2.4.1"
mkdir "$WORK"
ROOT="$WORK/training-fakefs"
RECORDINGS="$WORK/recordings"
GADGET_BUILD="$PWD/build-arm64-gadget"
G="$GADGET_BUILD/ish"
R="$PWD/build-arm64-recorder/ish"
ARCHIVE="$WORK/alpine-minirootfs-3.24.2-aarch64.tar.gz"
curl -fL https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-3.24.2-aarch64.tar.gz -o "$ARCHIVE"
printf '%s  %s\n' 9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773 "$ARCHIVE" | sha256sum -c -
"$GADGET_BUILD/tools/fakefsify" "$ARCHIVE" "$ROOT"
```

Configure a reachable guest DNS resolver and install the training packages:

```sh
DNS=1.1.1.1  # replace with a resolver reachable from this host
"$G" -f "$ROOT" /bin/sh -ec "printf 'nameserver %s\\n' '$DNS' > /etc/resolv.conf"
"$G" -f "$ROOT" /bin/sh -ec 'apk update; apk upgrade; apk add python3 zlib'
"$G" -f "$ROOT" /bin/sh -ec \
  'cat /etc/alpine-release; apk list --installed musl busybox python3 zlib'
```

The tested versions were musl1.2.6-r2, BusyBox1.37.0-r31, Python3.14.7-r1 and
zlib1.3.2-r0. Alpine repositories change; a later install can produce different
bytes. Save the guest export and package inventory for exact replay. The app's
stock minirootfs does not include this Python installation step.

## Record the four modules

Clear diagnostic and tuning overrides in the shell before manual runs. The
recorder script removes inherited `ISH_JIT*` and `ISH_AOT*` variables itself.

```sh
CC=clang make record-arm64-aot NATIVE_BUILD_DIR=build-arm64-recorder \
  ROOTFS_DIR="$ROOT" AOT_RECORD_DIR="$RECORDINGS"
test -s "$RECORDINGS/manifest.json"
```

`targeted.ts` discovers the actual sonames and runs a shell loop, Python
arithmetic/JSON and zlib round-trips serially. It checks exit statuses,
completion markers, module hashes and a common ABI. Family matching is disabled.
The final `manifest.json` appears only after all four recordings and images
succeed. `AOT_TIMEOUT_S` controls each recorder/generator command (default 120s).

On failure, keep the output directory for diagnosis and retry with a new path.
An existing output directory is refused. A manifest lists recorder, workload,
module, JSONL and image hashes; it does not contain a complete guest snapshot.

## Link and test an AOT-only executable

```sh
IMAGES="$RECORDINGS/aot_musl.S,$RECORDINGS/aot_busybox.S,$RECORDINGS/aot_python.S,$RECORDINGS/aot_zlib.S"
CC=clang meson setup build-arm64-aot --buildtype=release \
  -Djit=true -Djit_emit=false -Dcli_aot="$IMAGES"
make build-arm64-linux RELEASE_BUILD_DIR=build-arm64-aot
EVIDENCE_DIR="$WORK/linked-release" make test-arm64-linked-aot \
  RELEASE_BUILD_DIR=build-arm64-aot ROOTFS_DIR="$ROOT" AOT_RECORD_DIR="$RECORDINGS"
```

The check requires four accepted images, zero rejections, positive installs for
each module, zero emitted segments/units/bytes and matching workload results
with AOT on and off. It also checks the no-emitter compiler define and object
imports. Executable mappings for the application and host libraries still exist.

Run a command or open a shell:

```sh
ISH_JIT=1 ISH_AOT_FAMILY=0 ISH_JIT_STATS=1 \
  ./build-arm64-aot/ish -f "$ROOT" /bin/sh -ec \
  'python3 -c "import zlib; a=b\"aot\"*10000; assert zlib.decompress(zlib.compress(a))==a"; cat /proc/ish/jit'
ISH_JIT=1 ISH_AOT_FAMILY=0 ./build-arm64-aot/ish -f "$ROOT" /bin/sh
```

For rollback, start a new process with `ISH_JIT=0`, or use the gadget binary:

```sh
ISH_JIT=0 ./build-arm64-aot/ish -f "$ROOT" /bin/sh
"$G" -f "$ROOT" /bin/sh
```

`ISH_JIT_AOT_ONLY=1` can disable emission in a recorder build. Use the compile-time
`jit_emit=false` option for delivered AOT executables.

## Validate release and debug

Freeze the training guest before tests that write fixtures; the [artifact
guide](NATIVE_AOT_ARTIFACT_KIT.md) restores independent test roots. For a disposable
training guest, the commands below can run in place.

```sh
CC=clang meson setup build-arm64-aot-debug --buildtype=debug \
  -Djit=true -Djit_emit=false -Dcli_aot="$IMAGES"
make build-arm64-linux RELEASE_BUILD_DIR=build-arm64-aot-debug
for BUILD in build-arm64-aot build-arm64-aot-debug; do
  CC=clang HOST_CC=clang make RELEASE_BUILD_DIR="$BUILD" \
    ROOTFS_DIR="$ROOT" DEBIAN_ROOTFS_DIR="$ROOT" REPORT_DIR="$WORK" \
    test-arm64-upstream test-arm64-poll-regular test-arm64-fcvt-vector \
    test-arm64-load64-fault-pc test-arm64-proc-mem-seek test-arm64-lseek-width \
    test-arm64-poke-stress test-arm64-internal-continue-fixtures test-arm64-proc-exit-race \
    test-arm64-offload-setup test-arm64-native-fs test-arm64-offload-context \
    test-arm64-offload-io test-arm64-offload-local-copy
  EVIDENCE_DIR="$WORK/linked-$BUILD" make test-arm64-linked-aot \
    RELEASE_BUILD_DIR="$BUILD" ROOTFS_DIR="$ROOT" AOT_RECORD_DIR="$RECORDINGS"
done
EVIDENCE_DIR="$WORK/native-restart" make test-arm64-native-emitter \
  NATIVE_BUILD_DIR=build-arm64-recorder
bun tests/arm64/native-aot/execution.ts \
  "$PWD/build-arm64-aot-debug/ish" "$ROOT" "$WORK/image-execution"
```

Historical targets use `DEBIAN_ROOTFS_DIR` even for Alpine; set both rootfs
variables. The full procfs check runs a native control and two guest repetitions,
each 25s with 16 forkers and six readers. Keep its progress checks and timeout.
The emitter harness covers exact native fault/retry state at O0/O2, the callable
native-only app adapter under Linux signals, and read-only diagnostics before/
after normal init. The separate
GDB check requires an actual PC hit within each image and a NULL emitter region;
image-install counters measure lookup results. Execution coverage needs separate measurement.

## Observe layout without enabling the backend

Native builds expose a read-only JSON endpoint:

```sh
./build-arm64-aot/ish -f "$ROOT" /bin/cat /proc/ish/jit-layout
```

`jit_layout_read` and `jit_layout_describe` return the compiled CPU/frame/block/
TLB/context layouts, code version and compile-time emission setting. They do not
call backend init, allocate, map executable memory or change pinning. Convention
fields (`abi`, `prologue_words`, `entry_off`, `n_pinned`, `pic`) are usable only
with `ready=1` after normal backend initialisation or explicit no-emitter
`jit_aot_prepare_layout` startup preparation. A no-emitter/no-image or
runtime-off process can report `ready=0` and zero conventions. Readiness does not
mean images are accepted or code is executing. Gadget builds have no endpoint.

The tested Linux PIC/pinned ABI is `3f650e41`, code version 10, 20 prologue words,
entry offset 76 and 16 pinned registers. Collect values separately for a different
build; never relabel recordings. An Apple contract additionally requires actual
SDK/platform, binary SHA-256 and calling-context evidence. The
[shared-recovery report](reports/audits/SHARED_NATIVE_RECOVERY_2026-10-02.md)
records Linux exact restart and native-only app-adapter tests; the app adapter
is now installed by the isolated Apple bootstrap, with real Darwin static-fault
tests described in the [Apple report](reports/audits/APPLE_AOT_2026-10-04.md).

## Measure and update

```sh
PERF_CPU=0 PERF_RUNS=5 bun tests/arm64/native-aot/measure.ts \
  "$G" "$PWD/build-arm64-aot/ish" "$ROOT" "$WORK/timings"
# Optional RSS and perf sampling; requires user-event perf permissions:
PERF_CPU=0 bun tests/arm64/native-aot/profile.ts \
  "$G" "$PWD/build-arm64-aot/ish" "$ROOT" "$WORK/profile"
```

Use an available CPU. Keep profiler runs separate from timings. The RSS script
uses an exec/wait4 launcher to exclude Bun's inherited fork high-water value.
Record binary hashes, governor, rootfs, samples and spread. Profile image lookup,
slot conflicts and native/fallback execution before increasing training coverage.

When packages change, make a new guest snapshot and recording set. When the
emulator ABI changes, regenerate with a matching recorder. Runtime rejection
should leave the gadget path usable; exercise that case before distributing an
updated image. Never override the ABI field to force image acceptance.

## Troubleshoot

| Symptom | Check |
|---|---|
| `refusing existing output` | Keep the failed run; select a new output path. |
| Missing Python/soname | Run the inventory command above in the selected guest. |
| Recorder timeout | Inspect the module log and guest command before increasing `AOT_TIMEOUT_S`. |
| Image rejected | Compare module hashes, recorded ABI, entry offset and pinned-register settings. |
| Four images accepted but little benefit | Measure lookup/fallback cost and representative workloads; acceptance alone gives no speed estimate. |
| Unexpected emitter mappings | Check `meson configure BUILD` and `ISH_JIT_NO_EMIT=1` in compile commands. |
| Clang/GCC or frame-layout build error | Use fresh directories and consistent compiler/backend defines. |
| GDB proof fails | Read the saved script/log; a predicate error or timeout fails the check. |
