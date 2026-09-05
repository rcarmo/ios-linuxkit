# Linux development

The Linux command-line build requires an AArch64 host because its gadget files contain AArch64 assembly and execute directly on the host CPU. The validated development host is an Orange Pi 6 Plus running Debian Trixie.

## Host used for validation

| Component | Value |
|---|---|
| Board | Orange Pi 6 Plus |
| SoC | CIX P1 (CD8180/CD8160) |
| CPU | 4 Cortex-A520 cores and 8 Cortex-A720 cores |
| RAM | 16 GB class; about 14 GiB visible to Linux |
| Host architecture | AArch64 |
| OS | Debian Trixie |
| Compiler used by the July audit and September source-release validation | Clang 19.1.7 |
| September validation kernel | `6.6.89-cix` (AArch64) |
| Workspace storage | NVMe, ext4 |

The board details identify the measured host; they are not minimum requirements. The `test-arm64-fcvt-vector` gate also uses the host CPU as an AArch64 floating-point oracle.

## Dependencies

On Debian or Ubuntu:

```sh
sudo apt install \
  clang \
  make \
  meson \
  ninja-build \
  pkg-config \
  git \
  curl \
  file \
  tar \
  libsqlite3-dev \
  libarchive-dev
```

The core binary needs POSIX threads and SQLite. Meson builds `tools/fakefsify` only when it finds libarchive.

Optional SDL/VNC terminal development needs SDL2, SDL2_ttf, libvterm, libvncserver and libutil development files. `tools/meson.build` omits `ish-sdl-vnc` when any dependency is unavailable.

## Clone

```sh
git clone --recurse-submodules https://github.com/rcarmo/ios-linuxkit.git
cd ios-linuxkit
```

For an existing checkout:

```sh
git submodule update --init --recursive
```

The repository uses submodules under `deps/`. A source archive without those revisions is not sufficient for every build path.

## Build

The Makefile wraps the supported Meson commands:

```sh
make build-arm64-linux
make build-arm64-linux-debug
```

Build both variants with:

```sh
CC=clang make build-arm64-linux-all
```

The outputs are:

- `build-arm64-linux/ish` for the release build;
- `build-arm64-linux-debug/ish` for the debug build;
- `build-arm64-linux/tools/fakefsify` when libarchive is present.

Equivalent first-time Meson commands are:

```sh
CC=clang meson setup build-arm64-linux \
  -Dguest_arch=arm64 \
  --buildtype=release
ninja -C build-arm64-linux
```

Meson stores compiler and option choices in the build directory. Use `meson configure build-arm64-linux` to inspect them. Reconfigure explicitly or remove the directory before changing the compiler or build type.

The supported Clang build uses Clang's integrated assembler. GNU `as` rejects named-register `.req` syntax used by the existing AArch64 gadget sources; the July 2026 audit reproduced that failure on the pre-audit baseline.

## Run with realfs

`-r` mounts a host directory as the guest root:

```sh
./build-arm64-linux/ish -r / /bin/echo hello
```

This is useful for a small smoke test when the host contains AArch64 Linux programs. With `/` as the root, the current launcher can print `init: failed to chmod /dev/shm: Operation not permitted` because it attempts to enforce guest `/dev/shm` permissions on the host mount; the command still exits with the guest program's status. Realfs exposes the selected host tree. Prefer a restricted directory for ordinary tests; this runtime is not a security boundary for hostile guest code, even with a restricted realfs root.

Common command-line options implemented by `xX_main_Xx.h` are:

| Option | Meaning |
|---|---|
| `-r PATH` | Mount `PATH` as a realfs root. |
| `-f PATH` | Open a fakefs root at `PATH`; the file contents live below `PATH/data`. |
| `-d PATH` | Set the initial guest working directory. |
| `-c PATH` | Select the guest console path. |
| `-n NAME=PATH` | On Darwin hosts, register a native-offload command mapping; Linux rejects it. |

The program after these options is the initial guest process. There is no separate built-in help page; invalid option diagnostics come from `getopt`.

## Create and run a fakefs

The rootfs URL and architecture are defined in `app/GuestARM64.xcconfig`. At the time of this rewrite they name Alpine 3.24.0 for AArch64:

```sh
curl -LO https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-3.24.0-aarch64.tar.gz
./build-arm64-linux/tools/fakefsify \
  alpine-minirootfs-3.24.0-aarch64.tar.gz \
  alpine-arm64-fakefs
./build-arm64-linux/ish -f ./alpine-arm64-fakefs /bin/sh
```

The generated fakefs directory is ignored by Git. Keep an untouched copy if tests are allowed to install packages.

Export a fakefs by invoking the same binary through its `unfakefsify` symlink:

```sh
./build-arm64-linux/tools/unfakefsify \
  alpine-arm64-fakefs \
  rootfs-export.tar.gz
```

## Bind host paths

The Linux launcher reads `ISH_BIND_MOUNTS` as a comma-separated list:

```sh
ISH_BIND_MOUNTS='/mnt/src=/home/me/src:ro,/mnt/out=/tmp/out:rw' \
  ./build-arm64-linux/ish -f ./alpine-arm64-fakefs /bin/sh
```

Both paths must be absolute. The suffix defaults to read-write; use `:ro` for a read-only mount.

## Focused AdvSIMD conversion test

The focused conversion gate builds one static AArch64 fixture on the host, runs it natively, then runs the same binary under iSH:

```sh
CC=clang make test-arm64-fcvt-vector
```

The target uses `debian-arm64-fakefs` by default and creates that fakefs through the Makefile recipe when absent. The fixture itself is static and needs no guest compiler. It checks `FCVTN`/`FCVTN2`, `FCVTL`/`FCVTL2` and `FCVTXN`/`FCVTXN2`, including guest rounding mode, cumulative floating-point exceptions, vector-half semantics and register aliasing.

## Precise load-PC and paired performance tests

With a prepared `debian-arm64-fakefs`, run the native-oracle and guest retry gate:

```sh
CC=clang make test-arm64-load64-fault-pc
CC=clang ISH_BIN="$PWD/build-arm64-linux-debug/ish" \
  tests/arm64/loadstore/run-load64-fault-pc.sh
```

This static fixture tests exact LDR signal PCs and retry state. Its isolated two-page unmap avoids the existing nearby-page recovery workaround; it does not establish general page-permission conformance. See [VALIDATION.md](VALIDATION.md#focused-low-level-fixtures).

For paired shell/Python startup, compute and temporary-file timing:

```sh
PERF_CPU=11 bun tests/arm64/perf-poke.ts \
  /absolute/path/ish-before /absolute/path/ish-after \
  debian-arm64-fakefs /tmp/paired-samples.json 15
```

Use an available CPU on your host. The script requires Bun and guest Python, starts a fresh emulator each sample, alternates order, checks outputs and rejects `ISH_*` diagnostic overrides. It does not change CPU policy. Record frequency settings, workload, binary hashes and spread; a profile alone is not performance evidence.

## Diagnostics

Diagnostics are disabled unless named below or enabled by a debug build.

| Variable | Effect |
|---|---|
| `ISH_TRACE_FAULTS=1` | Print guest fault and translated-block diagnostics. |
| `ISH_TRACE_HIGHBITS=1` | Trace high-bit general-register values used during earlier fault investigations. |
| `ISH_TRACE_PCS=...` | Trace selected guest program counters. |
| `ISH_TRACE_GATE_PC`, `ISH_TRACE_GATE_X4`, `ISH_TRACE_GATE_BUDGET` | Bound a trace around a selected PC/register condition. |
| `ISH_ARM64_BLOCK_STATS=1` | Print block-cache, chaining and prechain counters at exit. |
| `ISH_ARM64_FUSION_STATS=1` | Print instruction-fusion counters. |
| `ISH_ARM64_EAGER_PRECHAIN=0` | Disable outgoing eager prechain for diagnosis. |
| `ISH_ARM64_EAGER_PRECHAIN_INCOMING=0` | Disable guarded incoming eager prechain. |
| `ISH_ARM64_INTERNAL_CONTINUE=1` | Enable the experimental internal-continue path. |
| `ISH_ARM64_INTERNAL_CONTINUE_TAKEN=1` | Enable the associated taken-path mode. |

Do not enable statistics or trace output in exact-output test runs unless the harness explicitly expects it.

## SDL/VNC harness

When optional dependencies are installed, configure and build the harness:

```sh
CC=clang meson setup build-linux-harness -Dguest_arch=arm64
ninja -C build-linux-harness tools/ish-sdl-vnc
```

`tools/run-sdl-vnc.sh` starts the terminal harness. Its default inputs are `build-arm64-linux/ish`, `alpine-arm64-fakefs` and TCP port 5907. Inspect the script before exposing the VNC listener outside a trusted development network.
