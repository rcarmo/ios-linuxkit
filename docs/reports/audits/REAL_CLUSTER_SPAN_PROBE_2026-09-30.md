# Real-code clustering span probe — 30 September 2026

## Scope and provenance

This is an ownership/failure probe, not a clustering import or Apple validation.
It links the real upstream `pt_map_cluster()`, page tables, mapping, unmap,
protection and fork/CoW code from OpenMinis worktree commit
`9d0fa1db8d566658352a24a27dc59e41fb238001` (netlink PR branch based on
`e6521d9c`). Clustering originated in `ce1b4b86`; current upstream follow-ups are
included. Neither upstream sources nor the released ios-linuxkit master changed.

Host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), 12 cores, 16 GB-class RAM
(about 14 GiB visible), NVMe, Debian Trixie, Clang 19, native AArch64 Linux,
4 KiB real host pages.

The fixture changes `real_page_size` to 4 KiB or 16 KiB and wraps host `mmap64`
and `munmap`. The adapter aligns mappings to the requested span, rounds backing
sizes, tracks allocations and frees an entire synthetic span on its final unmap.
It injects host-map and `struct data` allocation failures. The clustering and
ownership decisions come from the actual linked kernel, not a duplicate model.
No allocation or protection adapters are production changes.

Evidence: `/workspace/artifacts/ish-clustering-20260930/`; fixture
`upstream-cluster.c`, release/debug executables, one `.log` and `.rc` per case.
The [tracked fixture](fixtures/upstream-cluster-span.c) preserves the tested source;
archive and fixture SHA-256 files are retained in the evidence directory.

## Results

Both release (`build-netlink-reproduce`) and debug (`build-netlink-debug`) produce
the same results: **12/14 cases pass per build; two deliberate failure-injection
cases expose a backing leak**.

| Case | Span | Result in both builds |
| --- | --- | --- |
| Aligned cluster, offsets and partial/final unmap | 4 KiB / 16 KiB | Pass; one map and exactly one final unmap |
| Reservation boundary and different permission neighbour | 4 KiB / 16 KiB | Pass; single-page fallback, no fabricated neighbour |
| Partial unmap and hole reuse | 4 KiB / 16 KiB | Pass; no replacement of live neighbour; balanced allocations |
| Fork, CoW write and independent child data | 4 KiB / 16 KiB | Pass; two maps/two unmaps, correct shared ownership and guest ledger |
| Guest permission change, denied write and generation publication | 4 KiB / 16 KiB | Pass; guest flags/accounting/translation invalidation |
| Cluster host-map failure, single-page retry | 16 KiB | Pass; one map/one unmap |
| Both host-map attempts fail | 16 KiB | Pass; zero maps/unmaps, zero commitment |
| Cluster data-object allocation fails, single-page retry | 16 KiB | Fail (134); two maps/one unmap, **16 KiB still live** |
| Both data-object allocations fail | 16 KiB | Fail (134); two maps/zero unmaps, **32 KiB still live** |

A failed check is an assertion in the fixture's final ownership balance, not an
unexplained emulator crash. The expected guest ledger returns to zero while the
host backing remains allocated, demonstrating why the ledger alone is not an
ownership oracle.

## Concrete defect

In upstream `kernel/memory.c`, `pt_map_nothing()` successfully maps backing, then
calls `pt_map()`. When `pt_map()` cannot allocate `struct data`, it returns
`_ENOMEM` without taking ownership. `pt_map_nothing()` returns the error without
`munmap(memory, map_size)`. `pt_map_cluster()` then retries as one page, leaving
the failed cluster allocation live. Failure of that retry leaks its backing too.

The local ticket prototype's existing `pt_map_nothing()` already frees successful
backing on failed installation and refunds admission. A bounded local clustering
port should use that primitive unchanged, not import the upstream allocator or
thread-local precharge. The allocation leak is not a reason to discard the
successful cluster-selection and shared-ownership tests.

## Reproduction

From `/workspace/projects/openminis-tailscale`, with the recorded libraries:

```sh
E=/workspace/artifacts/ish-clustering-20260930
B=build-netlink-reproduce # repeat with build-netlink-debug
clang -O2 -I. -I"$B" -DGUEST_ARM64=1 -DENGINE_ASBESTOS=1 -pthread \
  "$E/upstream-cluster.c" -Wl,--wrap=malloc -Wl,--wrap=mmap64 \
  -Wl,--wrap=munmap -Wl,--start-group "$B/libish.a" "$B/libish_emu.a" \
  "$B/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3 \
  -o "$E/upstream-cluster-release"
"$E/upstream-cluster-release" 16384 basic
"$E/upstream-cluster-release" 16384 data # expected assertion/exit 134
```

The complete logs retain passing 4 KiB controls and the double-failure cases.

## Remaining gates

- No clustering has been imported into the local prototype yet.
- Test a local, separate bounded port using explicit allocation tickets. Repeat
  these ownership cases, both failure attempts and pressure/cap refusal.
- The direct cluster reservation predicate was tested; complete lazy-fault,
  stack/GPF and JIT/AOT restart integration still needs candidate tests.
- Range metadata/protection combinations, page-table-node OOM and competing
  fault/CoW lock-upgrade interleavings are not exhaustively covered.
- Synthetic 16 KiB spans are **not** Darwin `mprotect`, RSS/physical footprint,
  real Apple allocation geometry, app lifecycle or jetsam evidence. The permission
  case checks guest flags/translation, not host subpage protection independence.
- Held-out workload timing/RSS and app AOT/cancellation work remain later gates.

See the [memory prototype](../../MEMORY_POLICY_PROTOTYPE.md) and
[follow-up audit](OPENMINIS_MEMORY_FOLLOWUP_2026-09-30.md) for the adoption boundary.
