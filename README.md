# ios-linuxkit

![ios-linuxkit icon](docs/icon-256.png)

`ios-linuxkit` runs an AArch64 Linux userland inside an iOS app and as a command-line process on an AArch64 Linux host. It derives from [iSH](https://ish.app/) and uses iSH's userspace kernel, filesystems and Asbestos threaded-code interpreter.

The current source version is **2.1.3** with Apple build number **809**. The repository supports one guest architecture: ARM64. The interpreter decodes guest instructions into programs of pointers to precompiled host functions. All executable host instructions come from the built application; the interpreter allocates only data for translated programs.

## What is in the repository

- an ARM64 instruction decoder and AArch64 host gadgets under `asbestos/guest-arm64/`;
- a 48-bit guest address space, Linux syscall layer, signals, sockets and fakefs;
- the `iSH-ARM64` iOS application target, Ghostty Web terminal frontend and an `iSH-ARM64-ffmpeg` integration target;
- Linux-host builds for development and regression testing;
- staged tests for instructions, syscalls, language runtimes and command-line packages.

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
make build-arm64-linux
```

Run against the host filesystem:

```sh
./build-arm64-linux/ish -r / /bin/echo hello
```

To create an Alpine fakefs, download the root filesystem named in `app/GuestARM64.xcconfig`, then import it:

```sh
curl -LO https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-3.24.0-aarch64.tar.gz
./build-arm64-linux/tools/fakefsify \
  alpine-minirootfs-3.24.0-aarch64.tar.gz \
  alpine-arm64-fakefs
./build-arm64-linux/ish -f ./alpine-arm64-fakefs /bin/sh
```

`fakefsify` is built when Meson finds libarchive. Existing build directories retain their original Meson configuration; remove or reconfigure them when changing the compiler or build type.

## Build and test commands

| Task | Command |
|---|---|
| Build release | `make build-arm64-linux` |
| Build release and debug | `make build-arm64-linux-all` |
| Check documentation links | `make check-docs` |
| Test AdvSIMD FP widening and narrowing | `CC=clang make test-arm64-fcvt-vector` |
| Test `/proc/<pid>/mem` seek semantics | `CC=clang make test-arm64-proc-mem-seek` |
| Test full-width seeks and Python sparse files | `CC=clang make test-arm64-lseek-width` |
| Test signal delivery to guest computation | `CC=clang make test-arm64-poke-stress` |
| Test precise load fault-PC and retry state | `CC=clang make test-arm64-load64-fault-pc` |
| Run staged runtime coverage | `make test-arm64-runtime-coverage` |
| Run coverage with the debug binary | `make test-arm64-runtime-coverage-debug` |
| Run CLI corner cases | `make test-arm64-cli-corner-smoke` |
| Run npm CLI coverage | `make test-arm64-npm-cli-runtime-coverage` |
| Measure Node and Bun | `make test-arm64-node-bun-perf` |

The runtime and CLI targets can install packages into their fakefs. Use a disposable copy when package state matters. Reports are written to `REPORT_DIR`, which defaults to `/workspace/tmp`.

[The July 2026 OpenMinis audit](docs/reports/audits/OPENMINIS_AUDIT_2026-07-20.md) records the repository-wide comparison at `35dac743` and the AdvSIMD conversion follow-up at `40f1bf40`. The follow-up added `FCVTN`, `FCVTN2`, `FCVTXN` and `FCVTXN2`; clean Clang release and debug builds and the native-oracle/guest fixture passed. The earlier broad suite reached 82/83 because the tested rootfs Clojure package lacked `clojure.main`.

[Source release 2.1.3](docs/reports/releases/IOS_LINUXKIT_2.1.3.md) reduces common 64-bit load dispatch overhead while preserving the exact fault retry PC. Two controlled ARM Linux Python compute series against 2.1.2 measured **1.9–2.6% median improvement**, with 23/30 faster pairs; shell startup remained noisy. Fresh release/debug gates include an 18-case native-oracle fault/retry fixture. The release notes record the existing read-fault workaround, native fixture and broad-suite limitations, and outstanding iOS validation. The preceding [2.1.2 release](docs/reports/releases/IOS_LINUXKIT_2.1.2.md) contains the full-width `lseek` and CPU poke changes; percentages from separate passes are not cumulative measurements.

## Documentation

| Document | Use it for |
|---|---|
| [Documentation index](docs/README.md) | Choosing the maintained guide or dated report. |
| [Architecture](docs/ARCHITECTURE.md) | Interpreter, memory, kernel and host boundaries. |
| [Linux development](docs/LINUX_DEVELOPMENT.md) | Building, fakefs creation, command-line use and diagnostics. |
| [iOS application](docs/IOS_APPLICATION.md) | Xcode schemes, rootfs packaging and host integration. |
| [Validation](docs/VALIDATION.md) | Test gates, reports and failure rules. |
| [Limitations](docs/LIMITATIONS.md) | Security, compatibility and unsupported workloads. |
| [Contributing](docs/CONTRIBUTING.md) | Change and documentation requirements. |
| [Versioning and releases](docs/RELEASES.md) | App versions, build numbers, Git tags and release checks. |

Dated benchmark, workload, release and audit records live under [`docs/reports/`](docs/reports/). They preserve their original observations and are not current operating instructions.

## Licence and provenance

`ios-linuxkit` contains work derived from [ish-app/ish](https://github.com/ish-app/ish) and its dependencies. See [LICENSE.md](LICENSE.md), [LICENSE.IOS](LICENSE.IOS), the [preserved May 2026 README](docs/legacy/ORIGINAL_ISH_README_2026-05.md), and [`docs/legacy/`](docs/legacy/).
