# Limitations

`ios-linuxkit` provides Linux compatibility inside an application process. It does not isolate hostile code from the host application and does not implement a complete Linux kernel or AArch64 machine.

## Security boundary

The runtime assumes one user inside an outer iOS sandbox. Guest permissions, memory safety and thread safety do not confine hostile code. A guest process can exercise a large C/assembly codebase and any host integration exposed by the app.

Do not use it to confine hostile workloads. Read [SECURITY.md](../SECURITY.md) for vulnerability reporting and the inherited security model.

Bind mounts and native offload widen the guest's access:

- a bind mount exposes a selected host-sandbox directory through fakefs;
- an in-process native handler receives guest-controlled arguments and runs as host code;
- a spawned native mapping runs a host executable outside instruction emulation.

Validate paths and arguments at the host boundary.

## Native offload and cancellation

[Cooperative offload](NATIVE_OFFLOAD.md) is startup-only and has no production
handler registration. It refuses sibling/exiting guest groups and unsupported
stdio. Only connected TCP streams have the validated per-call nonblocking,
byte/retry contract; Apple TTY/UI, PTY, file and pipe stdio are not admitted.
Poll readiness does not guarantee that a write returns within a deadline.

Cooperative checkpoints are not a watchdog. Local VFS operations can block,
workers cannot be abandoned, and a handler must close its owned descriptors and
join its workers before returning. The test-only local-copy example bounds bytes
and calls, retains partial output on failure/cancellation and never removes a
potentially replaced output path. It provides no stable source snapshot, atomic
publication or durable-storage guarantee. Legacy handlers retain host chdir,
path translation, scans and blocking forwarders; their cancellation is not made
bounded by the cooperative API.

## Guest architecture

Only an AArch64 guest is supported. `meson_options.txt` accepts only `arm64`, and `meson.build` rejects another guest architecture. The Linux command-line build also requires an AArch64 host for the precompiled AArch64 gadgets.

Instruction support is incomplete. Unsupported or unrecognised encodings raise the guest undefined-instruction path. The decoder includes many AdvSIMD and cryptographic operations, but no exhaustive architecture-conformance result exists.

## Linux facilities

The runtime implements Linux interfaces in user space. The following classes are absent or incomplete:

- kernel modules and direct kernel control;
- Linux namespaces, cgroups and the mount behaviour needed by Docker containers;
- hardware passthrough such as GPUs and arbitrary USB devices;
- a general seccomp, BPF, perf or fanotify implementation;
- complete AIO and `io_uring` semantics;
- every modern syscall, socket option, `procfs` field and filesystem edge case;
- GUI stacks such as X11 or Wayland in the supplied app.

Optional probes may receive `ENOSYS` and fall back. Fallback paths can let a program continue without that facility.

## Runtime compatibility settings

The guest environment supplies conservative defaults for Go and JavaScriptCore:

```text
GODEBUG=asyncpreemptoff=1
GOMAXPROCS=2
JSC_numberOfGCMarkers=1
JSC_useConcurrentGC=0
```

These settings disable Go asynchronous pre-emption, limit Go scheduler parallelism, and serialise JavaScriptCore garbage collection. Programs run with less concurrency than on native Linux unless they override the variables, and overriding them can reintroduce known signal or GC failures.

Node starts with V8 JIT disabled and a 512 MiB old-space limit. The initial launcher also disables V8's exposed WebAssembly path; later `execve` launches can load optional rootfs polyfills for selected packages. These paths do not provide native V8 JIT or general native WebAssembly behaviour.

## Memory and code protection

The guest has a 48-bit virtual address model with 4 KiB pages and lazy reservations. Host memory remains managed by the application process. Mapping, copy-on-write, translated-block invalidation and host-fault recovery have concurrency-sensitive paths; tests cover known failures but do not prove all memory races absent.

The code tracks guest read, write and execute permissions. Some comments and paths still reflect iSH's historical compatibility-first model, so callers should not treat guest page permissions as a security control against the host process.

In ordinary builds, `kernel/calls.c` may demand-map readable zeros after an unmapped read fault if a mapped neighbour exists within 16 pages (64 KiB). This is separate from the compile-time-disabled broad synthetic recovery mode. A single-page unmap can therefore fail to deliver the SIGSEGV that native Linux would deliver. Targeted V8 recovery paths also remain. The September load-PC test uses an isolated two-page unmap to obtain a real fault; it does not validate second-page-only faults or general `PROT_NONE` enforcement. See the [dated investigation](reports/benchmarks/ARM_LINUX_LOAD_PC_2026-09-05.md).

## Host differences

Linux and Darwin use different signal frames, polling facilities, socket behaviours, filesystem metadata and synchronisation APIs. The platform layer covers named seams, but some conditional code remains in socket, polling, native-offload and lock paths.

A test passing on the Linux host does not establish iOS behaviour for:

- app lifecycle and background suspension;
- memory pressure and jetsam;
- entitlements and sandbox paths;
- signing, installation or App Store processing;
- physical-device terminal input and rendering.

Run the exact iOS archive on a physical device before distribution.

## Rootfs and package state

Most language and CLI results depend on the packaged distribution, repositories and installed versions. Test scripts can install or update packages in place. A missing class, command or shared library fails the workload. Check package contents before investigating the emulator.

Dated reports under `docs/reports/` record their original rootfs and tool versions where known. They should not be presented as the result of the current package set without rerunning the command.

## Performance evidence

Historical x86-versus-ARM64 measurements are retained under [`reports/benchmarks/historical/`](reports/benchmarks/historical/). They were collected on earlier source, host and rootfs combinations. Use `make perf-bench` or `make test-arm64-node-bun-perf` for a current comparison and report the host, revision, rootfs, run count and percentile method.

Executor statistics are diagnostic counters. They do not establish user-visible speed without elapsed-time and workload measurements. The [September load-dispatch pass](reports/benchmarks/ARM_LINUX_LOAD_PC_2026-09-05.md) measured modest Linux compute gains against 2.1.2; shell startup remained noisy. Do not add percentages from separate optimisation passes or extrapolate them to iOS.

## AOT constraints

AOT images require compatible emulator layouts and matching guest modules.
Package changes can reduce image use; old ABI images are rejected. Code without
a usable translation runs through gadgets. The image tables and native text
increase binary size and memory use. The [29 September prototype](reports/audits/AOT_ARTIFACT_KIT_2026-09-29.md)
measured about 12–13% improvement in shell/zlib workloads and a 15% Python
slowdown, with Python peak RSS rising from 18.3MiB to 54.1MiB.

Linux recordings contain native words and baked offsets. Apple reuse requires
target ABI/symbol checks, app recovery/build integration and physical-device
validation. Shared recovery includes a callable, uninstalled native-only app
adapter, not an integrated Apple fault path. Read-only layout diagnostics report
conventions only after normal backend initialisation (`ready=1`); values from a
not-ready bootstrap or a Linux binary cannot substitute for an observed Apple
contract. Existing Xcode schemes use gadgets. [Apple implementation steps](NATIVE_AOT_IOS.md)
list the checks needed before distribution.

## Release tooling

The checked-in Fastlane configuration uploads upstream iSH targets and repositories. Before using it for this fork, change and review its ARM64 schemes, bundle identifiers, signing profiles, TestFlight groups, repository targets and changelog handling.
