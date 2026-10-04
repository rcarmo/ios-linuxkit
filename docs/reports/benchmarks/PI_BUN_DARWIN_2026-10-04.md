# Bun/pi iPhone test build

## Scope

On 2026-10-04, the Release `iSH-ARM64-AOT-Bootstrap` scheme was built with
Xcode 27.0 and the iOS 27.0 SDK, signed, installed and launched on an iPhone
16 Pro running iOS 27.0.1. Its bundle identifier is
`io.carmo.ios-linuxkit.aot-bootstrap`. The app remained running after launch;
terminal interaction and live model responses have not been verified on-device.

This is a bootstrap, not accelerated AOT execution. It retains backend symbols
but disables runtime emission and native execution. No native images or app
fault adapter are installed. No patch release is justified by these checks.

## Filesystem

A fresh Alpine 3.24.2 filesystem was prepared with the checksum-pinned Bun
1.4.2 Linux AArch64 musl executable and its libgcc/libstdc++ dependencies.
pi 1.0.2 was installed separately in `/opt/pi` with package lifecycle scripts
disabled. This pi installation is not included in the default bundled archive.

The [pi launcher](../../../scripts/guest/pi) is installed executable at
`/usr/local/bin/pi`. It runs the installed CLI through Bun, without Node.js.
Both `bun --version` and `pi --version` passed in the macOS gadget guest before
exporting and importing the filesystem for the phone.

The filesystem is in the bootstrap's private application-support container:
`Library/ApplicationSupport/LinuxKitAOTBootstrap/roots/default`. It does not
share or replace the reference application's filesystem. Bun, the launcher
and the pi package were checked in the device container after installation.
No provider credentials were included.

## Repeatable workload

The [host helper](../../../scripts/benchmark-pi-guest.ts) runs the
[guest workload](../../../tests/arm64/benchmarks/pi-workload.mjs) against a
prepared fakefs containing pi:

```sh
bun scripts/benchmark-pi-guest.ts /absolute/path/to/ish /absolute/path/to/fakefs offline 3
```

The offline workload uses pi's real agent and read/edit/write/bash tools with
scripted model responses. It fixes a synthetic sum function and runs a Bun
regression test. It does not access real project files or require credentials.
The helper disables Bun's JavaScript JIT and transpiler cache for diagnosis;
these runs are not native-AOT performance measurements.

Single successful runs took approximately 10-11 seconds of host wall time,
including approximately 8-10 seconds of guest module imports. Repeated runs
intermittently exited with a guest segmentation fault. Serial GC settings,
disabled JavaScript JIT and disabled transpiler cache did not eliminate it.
The fault was observed during JavaScriptCore module-record creation; its cause
is not established. Bun startup and a single passing workload are insufficient
evidence of pi stability.

The helper also supports `copilot 1`, using a read-only temporary mount of
short-lived authentication derived from local pi OAuth configuration. It
does not copy refresh credentials into the guest or modify host authentication.
Credential refresh failed during this session, so no live model benchmark
was completed. Temporary helper credentials are removed when the helper exits.

## Checks

Both reference and bootstrap Release builds completed. The bootstrap's static
Mach-O inspection confirmed iOS platform/minimum-version metadata and disabled
runtime emission/native-image execution. Signed bundle verification passed.
Bun archive, AOT kit, Xcode bridge, Apple inspection and documentation checks
passed. The Linux native-AOT gates and on-device Bun/pi workload remain untested.
