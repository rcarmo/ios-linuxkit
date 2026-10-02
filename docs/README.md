# Documentation

The maintained documentation describes the current `master` branch. Dated evidence and superseded instructions are kept under `reports/` and `legacy/`.

## Maintained guides

| File | Subject |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | ARM64 decoder, gadget interpreter, memory model, userspace kernel and host boundaries. |
| [LINUX_DEVELOPMENT.md](LINUX_DEVELOPMENT.md) | AArch64 Linux build, fakefs, command-line use and diagnostics. |
| [IOS_APPLICATION.md](IOS_APPLICATION.md) | Xcode schemes, rootfs packaging, signing boundary and embedding interfaces. |
| [NATIVE_AOT_BUILD_PLAN.md](NATIVE_AOT_BUILD_PLAN.md) | Build, train, link, run and measure Linux AOT. |
| [NATIVE_AOT_ARTIFACT_KIT.md](NATIVE_AOT_ARTIFACT_KIT.md) | Freeze a guest, restore inputs, validate and publish a reusable build. |
| [NATIVE_AOT_IOS.md](NATIVE_AOT_IOS.md) | Observe target layout/ABI, generate Mach-O and integrate the uninstalled app adapter on Apple hardware. |
| [NATIVE_OFFLOAD.md](NATIVE_OFFLOAD.md) | Startup registration, legacy/cooperative ownership, guest VFS, cancellation, restricted TCP and test-only local-copy contracts. |
| [NETLINK_TAILSCALE.md](NETLINK_TAILSCALE.md) | Opt-in read-only interface discovery, isolated Tailscale probe and networking limits. |
| [VALIDATION.md](VALIDATION.md) | Build and runtime gates, focused fixtures, reports and failure rules. |
| [LIMITATIONS.md](LIMITATIONS.md) | Security model, compatibility shims, incomplete facilities and unsupported workloads. |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Source, test and documentation requirements for changes. |
| [RELEASES.md](RELEASES.md) | App versions, Apple build numbers, Git tags and release checks. |

## Reports

Reports record a result at a named date or revision. Paths, package versions and pass counts in these files may be obsolete.

| Directory | Contents |
|---|---|
| [reports/audits/](reports/audits/) | Source and upstream comparison audits. |
| [reports/benchmarks/ARM_LINUX_POKE_2026-09-05.md](reports/benchmarks/ARM_LINUX_POKE_2026-09-05.md) | CPU poke profiling, paired ARM Linux measurements and limitations. |
| [reports/benchmarks/ARM_LINUX_LOAD_PC_2026-09-05.md](reports/benchmarks/ARM_LINUX_LOAD_PC_2026-09-05.md) | Load-dispatch profiling, precise fault-PC regression and paired measurements against 2.1.2. |
| [reports/benchmarks/game/](reports/benchmarks/game/) | Benchmarks Game harness and per-language results. |
| [reports/benchmarks/historical/](reports/benchmarks/historical/) | Retired x86/ARM64 compatibility and performance comparisons. |
| [reports/releases/](reports/releases/) | Source-release validation, production baseline and staging records; latest: [2.4.1](reports/releases/IOS_LINUXKIT_2.4.1.md). |
| [reports/workloads/](reports/workloads/) | Workload investigations such as `go-gte`. |

## Provenance

- [legacy/ORIGINAL_ISH_README_2026-05.md](legacy/ORIGINAL_ISH_README_2026-05.md) preserves the pre-rewrite fork README and its embedded upstream material.
- [legacy/](legacy/) also contains upstream translations and the superseded May 2026 Chinese backend guide.
- [SECURITY.md](../SECURITY.md) defines the security model.
- [LICENSE.md](../LICENSE.md) and [LICENSE.IOS](../LICENSE.IOS) contain licence terms.

Generated `fastlane/README.md` and executable `.pi/skills/*/SKILL.md` files serve their associated tools. Vendored component notes under `app/terminal/` describe only the bundled third-party assets at their named revisions.
