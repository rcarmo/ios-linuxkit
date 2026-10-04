# Reports

Reports describe specific versions, test systems and dates. Their paths,
package versions and commands may be outdated. Use the
[maintained guides](../README.md) for current instructions.

| Directory | Contents |
|---|---|
| [releases/](releases/) | Release notes; latest: [2.5.0](releases/IOS_LINUXKIT_2.5.0.md). |
| [audits/](audits/) | Bug investigations, code comparisons and test results. |
| [benchmarks/](benchmarks/) | Performance measurements and workload results. |
| [workloads/](workloads/) | Compatibility investigations for individual applications. |

## Recent reports

- [Apple AOT and Bun/Pi](audits/APPLE_AOT_2026-10-04.md): current functionality,
  runtime fixes and device-testing limits.
- [Bun/Pi tests](benchmarks/PI_BUN_DARWIN_2026-10-04.md): Bun and Pi behavior
  under the emulator on macOS.
- [Alpine AOT tests](audits/AOT_ALPINE_HOST_2026-09-29.md): Linux compatibility,
  performance and memory measurements.
- [GitHub issues and pull requests](audits/GITHUB_TRIAGE_2026-10-04.md):
  review of the reported build and packaging issues.

Measurements apply to the revision, hardware and guest packages named in each
report. Compare results only when those inputs are compatible.
