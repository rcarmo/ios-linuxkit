# Reports

Reports describe specific versions, test systems and dates. Their paths,
package versions and commands may be outdated. Use the
[maintained guides](../README.md) for current instructions.

| Directory | Contents |
|---|---|
| [releases/](releases/) | Release notes; latest: [2.6.0](releases/IOS_LINUXKIT_2.6.0.md). |
| [audits/](audits/) | Bug investigations, code comparisons and test results. |
| [benchmarks/](benchmarks/) | Performance measurements and workload results. |
| [workloads/](workloads/) | Compatibility investigations for individual applications. |

## Recent reports

- [Portable admission / Linux validation](audits/MEMORY_ADMISSION_LINUX_2026-11-15.md): post-2.6.0 accounting/publication fixes, default-off tickets, opt-in sampler and explicit platform deferrals.
- [Held-out qualification](audits/HELDOUT_LINUX_2026-11-15.md): frozen ABI15 Python/Bun/Go baselines; no new speedup or arbitrary-Go AOT claim.
- [OpenMinis completion](audits/OPENMINIS_COMPLETION_2026-11-15.md): fd/poll
  ownership, shared AOT paths, profiling, stale TLBs, SHA512 fallback and ABI-15 gates.
- [Crate/OpenMinis follow-up and Bun 1.4.4](audits/CRATE_OPENMINIS_2026-11-15.md):
  emitter reductions, paired host timings, fresh Linux no-emitter proof and
  remaining upstream/device work.
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
