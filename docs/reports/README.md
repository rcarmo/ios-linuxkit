# Reports

Files in this directory preserve dated evidence. They are not maintained procedures and may contain obsolete paths, branch names, package versions, commands or links.

- [`audits/`](audits/) records source and upstream comparisons.
- [`benchmarks/game/`](benchmarks/game/) records the Benchmarks Game harness and generated language rows from May 2026; absolute paths in generated reports name the original checkout.
- [`benchmarks/historical/`](benchmarks/historical/) preserves retired compatibility and performance comparisons.
- [`benchmarks/ARM_LINUX_POKE_2026-09-05.md`](benchmarks/ARM_LINUX_POKE_2026-09-05.md) records CPU poke profiling and paired Linux measurements.
- [`benchmarks/ARM_LINUX_LOAD_PC_2026-09-05.md`](benchmarks/ARM_LINUX_LOAD_PC_2026-09-05.md) records the second optimisation pass: inline load fault-PC saving, rejected TLB candidate, paired timings and exact-PC regression evidence.
- [`releases/`](releases/) preserves source-release validation, production and staging snapshots; the latest source release is [2.4.0](releases/IOS_LINUXKIT_2.4.0.md).
- [`workloads/`](workloads/) preserves workload investigations.

Integration evidence, subsequently included default-off in 2.3.0: [Alpine 3.24.2 native/AOT host proof](audits/AOT_ALPINE_HOST_2026-09-29.md) — default-off selective backend, four linked ELF images, Linux release/debug gates and modest/mixed measured results. iOS/device gates remain. [Prerequisite repairs](audits/AOT_ALPINE_INTEGRATION_2026-09-29.md) include the diagnosed proc-stat deadlock.

Latest JIT audit: [Native JIT/AOT investigation — 29 September 2026](audits/JIT_AOT_INVESTIGATION_2026-09-29.md)
records frozen experimental branches, native-oracle probes, adoption blockers and
selective CLI recovery hardening. The backend is disabled by default.

[Durable AOT artifact kit](audits/AOT_ARTIFACT_KIT_2026-09-29.md) records a reusable Alpine image/recording seed, Linux release/debug no-emitter builds, restored-guest acceptance, measured regressions and explicit Apple handoff gates. The tools were subsequently included in 2.3.1; iOS app enablement requires further work.

[Documentation review — 30 September 2026](audits/DOCUMENTATION_REVIEW_2026-09-30.md) records the 13-guide source/command review, technical-writing rules and validation for 2.3.1.

[Cooperative bounded streams/token isolation](audits/OFFLOAD_BOUNDED_IO_2026-10-02.md) adds restricted TCP stdio with per-call nonblocking operations, byte/deadline limits and locked signal/fork isolation. It does not admit arbitrary TTY/file/pipe I/O or enable an app handler.

[Cooperative guest-context execution](audits/OFFLOAD_CONTEXT_EXEC_2026-10-02.md) wires raw guest arguments and retained VFS context into a separate dispatcher, with transactional failure injection and realfs/fakefs concurrent execution. Four-lane compatibility/procfs and frozen-AOT gates pass; the later stream report above follows it.

[Offload filesystem context foundation](audits/OFFLOAD_FS_CONTEXT_2026-10-02.md) records retained guest root/CWD, VFS path/metadata semantics and realfs/fakefs concurrent tests; the execution report above completes its wiring.

[Native offload transactional setup](audits/OFFLOAD_SETUP_2026-10-02.md) records isolated v2.4.0-based setup/rollback hardening and actual-source handler/POSIX adapters. Cooperative CWD/I/O and Apple app gates remain pending.

[Safety minor release](releases/IOS_LINUXKIT_2.4.0.md) extracts unconditional CoW/ptrace rollback, signal-frame failure termination, reservation/stack revalidation and the gadget-only Xcode guard. Prototype admission, clustering, cooperative offload and app AOT remain excluded.

[Scalar saturation/gzip patch release](releases/IOS_LINUXKIT_2.3.3.md) repairs missing scalar saturating add/subtract used by BusyBox gzip, with full-result/QC and compression/file-cleanup regressions. No memory-policy, native-offload or app AOT prototype is merged.

[Route-netlink patch release](releases/IOS_LINUXKIT_2.3.2.md) records opt-in interface discovery, local regression gates and an official Tailscale control-plane probe, with authenticated-traffic and Apple-device limits.

[OpenMinis memory/AOT follow-up](audits/OPENMINIS_MEMORY_FOLLOWUP_2026-09-30.md) compares the footprint governor and host-page clustering against our allocation ledger and defines the next host/device gates; it does not import those changes.

Use the guides in the parent directory for current build, validation and release boundaries. When citing a report, include its date, source revision, host and rootfs if the report supplies them.
