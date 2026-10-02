# ios-linuxkit

![ios-linuxkit icon](docs/icon-256.png)

`ios-linuxkit` runs an AArch64 Linux userland inside an iOS app and as a command-line process on an AArch64 Linux host. It derives from [iSH](https://ish.app/) and uses iSH's userspace kernel, filesystems and Asbestos threaded-code interpreter.

The current source version is **2.4.1** with Apple build number **818**. The repository supports one guest architecture: ARM64. The interpreter decodes guest instructions into programs of pointers to precompiled host functions. By default, all executable host instructions come from the built application; the interpreter allocates only data for translated programs. The optional native/ahead-of-time (AOT) backend is disabled by default. Linux recording builds may emit native code; AOT-only builds instead link pre-generated translations into the executable. The iOS schemes do not yet enable that backend.

## What is in the repository

- an ARM64 instruction decoder and AArch64 host gadgets under `asbestos/guest-arm64/`;
- a 48-bit guest address space, Linux syscall layer, signals, sockets and fakefs;
- the `iSH-ARM64` iOS application target, Ghostty Web terminal frontend and an `iSH-ARM64-ffmpeg` integration target;
- Linux-host builds for development and regression testing;
- staged tests for instructions, syscalls, language runtimes and command-line packages;
- startup-only cooperative offload APIs with guest VFS/token ownership and
  restricted TCP streams; no production cooperative handler registration.

The iOS app is a reference terminal and packaging target. The outer iOS sandbox is the security boundary; read [SECURITY.md](SECURITY.md) before embedding the runtime or exposing guest workloads to untrusted input.

## Quick start on AArch64 Linux

Install Clang, Meson, Ninja, pkg-config, SQLite development files and libarchive development files. On Debian or Ubuntu:

```sh
sudo apt install \
  clang make meson ninja-build pkg-config git curl file tar \
  libsqlite3-dev libarchive-dev
```

Clone the submodules and build:

```sh
git clone --recurse-submodules https://github.com/rcarmo/ios-linuxkit.git
cd ios-linuxkit
CC=clang make build-arm64-linux
```

Run against the host filesystem:

```sh
./build-arm64-linux/ish -r / /bin/echo hello
```

To create an Alpine fakefs, download the root filesystem named in `app/GuestARM64.xcconfig`, then import it:

```sh
curl -fLO https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-3.24.2-aarch64.tar.gz
echo '9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773  alpine-minirootfs-3.24.2-aarch64.tar.gz' | sha256sum -c -
./build-arm64-linux/tools/fakefsify \
  alpine-minirootfs-3.24.2-aarch64.tar.gz \
  alpine-arm64-fakefs
./build-arm64-linux/ish -f ./alpine-arm64-fakefs /bin/sh
```

`fakefsify` is built when Meson finds libarchive. Existing build directories retain their original Meson configuration; remove or reconfigure them when changing the compiler or build type.

## Build and test commands

| Task | Command |
|---|---|
| Build release | `CC=clang make build-arm64-linux` |
| Build release and debug | `CC=clang make build-arm64-linux-all` |
| Check documentation links | `make check-docs` |
| Test AdvSIMD FP widening and narrowing | `CC=clang make test-arm64-fcvt-vector` |
| Test `/proc/<pid>/mem` seek semantics | `CC=clang make test-arm64-proc-mem-seek` |
| Test full-width seeks and Python sparse files | `CC=clang make test-arm64-lseek-width` |
| Test signal delivery to guest computation | `CC=clang make test-arm64-poke-stress` |
| Test precise load fault-PC and retry state | `CC=clang make test-arm64-load64-fault-pc` |
| Test full procfs/exit stress and lock/lookup regressions | `CC=clang make test-arm64-proc-exit-race` |
| Test imported correctness, lifetime and failure handling | `CC=clang make test-arm64-upstream` |
| Run staged runtime coverage | `make test-arm64-runtime-coverage` |
| Run coverage with the debug binary | `make test-arm64-runtime-coverage-debug` |
| Run CLI corner cases | `make test-arm64-cli-corner-smoke` |
| Run npm CLI coverage | `make test-arm64-npm-cli-runtime-coverage` |
| Measure Node and Bun | `make test-arm64-node-bun-perf` |

The runtime and CLI targets can install packages into their fakefs. Use a disposable copy when package state matters. Reports are written to `REPORT_DIR`, which defaults to `/workspace/tmp`.

## AOT builds

[Build AOT on Linux](docs/NATIVE_AOT_BUILD_PLAN.md) to record musl, BusyBox,
Python and zlib, link an emission-disabled executable, test it and compare it
with gadgets. [Freeze and share artifacts](docs/NATIVE_AOT_ARTIFACT_KIT.md)
for repeatable builds from retained guest bytes and recordings.
[Prepare iOS images](docs/NATIVE_AOT_IOS.md) after implementing the required
Apple build and recovery hooks.

The tested local prototype improves short shell and zlib workloads by about
12–13%; Python takes about 15% longer and uses more memory. See the
[29 September measurements](docs/reports/audits/AOT_ARTIFACT_KIT_2026-09-29.md).
Package upgrades can invalidate images. Existing installed userlands are left
unchanged; ordinary Meson and Xcode configurations keep the gadget engine.

## Releases and evidence

[2.4.1](docs/reports/releases/IOS_LINUXKIT_2.4.1.md) hardens offload setup,
adds guest-context cooperative execution and restricted TCP cancellation, repairs
VFS descriptor-allocation rollback, and prepares shared fault recovery/read-only
layout diagnostics. The bounded local-copy handler is test-only. Existing
FFmpeg test/legacy behaviour is unchanged; no real FFmpeg or app AOT is enabled.
The gadget defaults, frozen Linux AOT ABI and Alpine 3.24.2 pin are retained.
Apple archive/signing/device validation has not run for this source release.

Earlier source releases and dated audits are indexed under
[reports](docs/reports/README.md). Their measurements apply to the revisions,
hosts and guests named in each report.

## Documentation

| Document | Use it for |
|---|---|
| [Documentation index](docs/README.md) | Choosing the maintained guide or dated report. |
| [Architecture](docs/ARCHITECTURE.md) | Interpreter, memory, kernel and host boundaries. |
| [Linux development](docs/LINUX_DEVELOPMENT.md) | Building, fakefs creation, command-line use and diagnostics. |
| [iOS application](docs/IOS_APPLICATION.md) | Xcode schemes, rootfs packaging and host integration. |
| [Linux AOT](docs/NATIVE_AOT_BUILD_PLAN.md) | Recording, linking, running, testing and measuring local AOT. |
| [Native offload](docs/NATIVE_OFFLOAD.md) | Legacy/cooperative contracts, guest VFS, bounded streams and output policy. |
| [Route-netlink and Tailscale](docs/NETLINK_TAILSCALE.md) | Opt-in host interface discovery and isolated userspace networking tests. |
| [AOT artifacts](docs/NATIVE_AOT_ARTIFACT_KIT.md) | Freezing, restoring, validating and publishing reusable inputs/builds. |
| [iOS AOT](docs/NATIVE_AOT_IOS.md) | Target compatibility checks, Mach-O conversion and Apple integration. |
| [Validation](docs/VALIDATION.md) | Test gates, reports and failure rules. |
| [Limitations](docs/LIMITATIONS.md) | Security, compatibility and unsupported workloads. |
| [Contributing](docs/CONTRIBUTING.md) | Change and documentation requirements. |
| [Versioning and releases](docs/RELEASES.md) | App versions, build numbers, Git tags and release checks. |

Dated benchmark, workload, release and audit records live under [`docs/reports/`](docs/reports/). They preserve their original observations and are not current operating instructions.

## Licence and provenance

`ios-linuxkit` contains work derived from [ish-app/ish](https://github.com/ish-app/ish) and its dependencies. See [LICENSE.md](LICENSE.md), [LICENSE.IOS](LICENSE.IOS), the [preserved May 2026 README](docs/legacy/ORIGINAL_ISH_README_2026-05.md), and [`docs/legacy/`](docs/legacy/).
