# Reports

Files in this directory preserve dated evidence. They are not maintained procedures and may contain obsolete paths, branch names, package versions, commands or links.

- [`audits/`](audits/) records source and upstream comparisons.
- [`benchmarks/game/`](benchmarks/game/) records the Benchmarks Game harness and generated language rows from May 2026; absolute paths in generated reports name the original checkout.
- [`benchmarks/historical/`](benchmarks/historical/) preserves retired compatibility and performance comparisons.
- [`benchmarks/ARM_LINUX_POKE_2026-09-05.md`](benchmarks/ARM_LINUX_POKE_2026-09-05.md) records CPU poke profiling and paired Linux measurements.
- [`benchmarks/ARM_LINUX_LOAD_PC_2026-09-05.md`](benchmarks/ARM_LINUX_LOAD_PC_2026-09-05.md) records the second optimisation pass: inline load fault-PC saving, rejected TLB candidate, paired timings and exact-PC regression evidence.
- [`releases/`](releases/) preserves source-release validation, production and staging snapshots; the latest source release is [2.3.0](releases/IOS_LINUXKIT_2.3.0.md).
- [`workloads/`](workloads/) preserves workload investigations.

Integration evidence, subsequently included default-off in 2.3.0: [Alpine 3.24.2 native/AOT host proof](audits/AOT_ALPINE_HOST_2026-09-29.md) — default-off selective backend, four linked ELF images, Linux release/debug gates and modest/mixed measured results. iOS/device gates remain. [Prerequisite repairs](audits/AOT_ALPINE_INTEGRATION_2026-09-29.md) include the diagnosed proc-stat deadlock.

Latest JIT audit: [Native JIT/AOT investigation — 29 September 2026](audits/JIT_AOT_INVESTIGATION_2026-09-29.md)
records frozen experimental branches, native-oracle probes, adoption blockers and
selective CLI recovery hardening. The experimental backend remains disabled by default.

Prototype: [Durable AOT artifact kit](audits/AOT_ARTIFACT_KIT_2026-09-29.md) records a reusable Alpine image/recording seed, Linux release/debug no-emitter builds, restored-guest acceptance, measured regressions and explicit Apple handoff gates. No new release or iOS enablement.

Use the guides in the parent directory for current build, validation and release boundaries. When citing a report, include its date, source revision, host and rootfs if the report supplies them.
