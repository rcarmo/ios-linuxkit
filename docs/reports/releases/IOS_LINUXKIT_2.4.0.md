# ios-linuxkit 2.4.0 / build 817 — 1 October 2026

Minor source release requested for the mainline safety tranche. Based on
`e52e0c56` (2.3.3/build816); selectively extracts unconditional repairs from
`prototype/memory-pressure-policy`, not its combined commits. No signed Apple
archive, App Store upload or physical-device validation is claimed.

## Changes

### CoW and ptrace rollback

`kernel/memory.c` now allocates against the current page-table entry after
acquiring its write lock. Failed backing `mmap` or data-object installation
returns failure without changing old backing, flags, MMU generation or the
logical anonymous ledger. Uninstalled backing is unmapped exactly once.
Successful replacement transfers accounting explicitly using the existing
`pt_map_accounted` interface, without policy tickets or TLS precharge.

Ptrace flags are not made writable before allocation/installation succeeds.
Host `PROT_NONE` backing is refused rather than copied from inaccessible memory
or temporarily exposing shared backing. This is a safe refusal, not a new ability
to read protected host memory. The existing ptrace write-permission behaviour on
successful accessible backing is retained.

CoW retains the existing waiting lock-upgrade protocol: `write_wrlock` retries
with try-lock/backoff rather than a blocking host writer wait. `in_jit` covers
both native code and gadgets, so it cannot justify introducing synthetic faults
into gadget execution. Competing removal, protection change and replacement are
rechecked after acquiring the write lock.

### Signal-frame allocation failure

Failed guest signal-frame installation releases `sighand->lock` and terminates
the guest with SIGSEGV. Recursive delivery under the held lock previously hung;
a configured SIGSEGV handler cannot help when no frame can be installed.
No emergency allocator or promise of survival under sustained OOM is introduced.

### Reservation and stack ownership

A lazy reservation takes precedence over a nearby growsdown mapping. Reservation
pointers are re-fetched after the read/write lock upgrade; removed, split or
replaced reservations cannot be dereferenced using an expired pointer.
`PROT_NONE` reservations remain unmaterialized. Stack growth revalidates the
anchor, its growsdown/write permissions, the current stack limit and reservation
ownership before mapping. Existing-page pointers are re-fetched after reacquiring
the read lock. No clustering is performed.

### Xcode gadget guard

Fresh and reused Meson directories explicitly set `jit=false`, `jit_emit=false`
and empty `cli_aot` together. Atomic option reset avoids a dependency error with
stale native images. Configuration errors propagate. Existing app targets have
no native fault integration or image membership; there is no environment bypass
or new AOT scheme.

Version sources: ARM64 `MARKETING_VERSION=2.4.0`; all four project
`CURRENT_PROJECT_VERSION=817` entries; annotated source tag `v2.4.0`.

## Exclusions

No pressure sampler/admission policy, allocation tickets, 16 KiB clustering,
cooperative handler API, real FFmpeg, workload runner or app AOT backend is merged.
Native/AOT remains default-off; the frozen image ABI and Alpine pin are unchanged.
Netlink remains opt-in. Page-table-node allocation OOM, universal fork admission,
full host-footprint budgeting, and broader offload I/O/CWD/failure hardening remain
separate work. These fixes are not a claim that all memory failure paths are safe.

## Host and evidence

Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), eight Cortex-A720 and four Cortex-A520
cores, 16 GB-class RAM (about 14 GiB visible), NVMe/ext4, native Debian Trixie,
Linux 6.6.89-cix AArch64, 4 KiB host pages; Clang 19.1.7, Meson 1.7.0,
Ninja 1.12.1 and Bun 1.4.2. No Xcode/Apple SDK/device is available.

Evidence `/workspace/artifacts/ish-safety-2.4.0-20261001/` retains baseline,
candidate and failed fixture/harness attempts. Tests link actual archives with
matching gadget/native shared-structure definitions. Frozen Alpine 3.24.2 test
roots and four existing ELF AOT images are reused, with no package fetch/training.

## Baseline diagnosis and focused proof

Identical policy-independent fixtures link frozen v2.3.3 archives:

| Baseline case | Result | Candidate |
| --- | --- | --- |
| CoW backing-map allocation failure | host SIGSEGV, 139 | safe refusal, original mapping/accounting retained |
| CoW data-object allocation failure | uninstalled copy leak detected, 134 | copy unmapped, mapping retained |
| Ptrace data-object allocation failure | mutated state detected, 134 | flags/backing/accounting retained |
| Ptrace host PROT_NONE | host SIGSEGV, 139 | safe refusal |
| Signal-frame backing/data-object refusal | signal-lock timeout, 124 | unlocked SIGSEGV guest termination |
| Signal-frame normal recovery control | 0 | 0 |
| Reservation precedence/replacement, removed/read-only stack anchor | fixture assertions, 134 | ownership revalidated |
| Extracted candidate CoW try-lock refusal under real gadget execution | synthetic INT_GPF, fixture assertion 134; proc stress SIGSEGV | existing waiting protocol retained, store/writeback/CoW isolation pass |

Final upstream suite covers, in each of four lanes:

- seven CoW/ptrace modes: backing/data failure, success controls, ptrace failure,
  success and protected-backing refusal; exact flags/data/accounting/generation
  preservation and balanced backing allocation/unmap;
- three actual signal-frame modes: backing/data failure with installed SIGSEGV
  handler, unlocked terminal status, and successful frame/recovery/accounting;
- 25 lock-upgrade cases: reservation/stack ownership, removal, protection,
  replacement, CoW protected/remapped pages in ordinary and simulated native
  contexts, plus genuinely contended CoW waiting/completion. Controlled competing
  writers run through actual rwlocks and page-table code, not a replacement model;
- actual gadget execution of a pre-indexed STP while a competing host reader
  delays CoW: no synthetic interrupt, one writeback, exact stored values, original
  child backing unchanged, and balanced anonymous accounting.

Fixtures initially assumed the signal SP was unchanged before installation and
the extended ARM64 frame fit one page; corrected checks use the actual adjusted
SP/frame coverage. The initial writer hook ran recursively; disabling its trigger
before spawning the competing writer fixed that fixture. Neither attempt is a
candidate pass. A delegated narrow review timed out after 90 s; there is no
independent-review claim.

## Broader gates

Four lanes: gadget release/debug and compile-time no-emitter AOT release/debug.
Authoritative final results are `wait-final/<lane>/{upstream,proc,compat}.rc`:
all four lanes are 0. Earlier `*-proc-final.rc` results failed and are retained,
not counted as final passes:

- existing actual-kernel anonymous accounting, mapping/install/task failure,
  normal-exit TLS handoff, syscall52/FMOV512, signal/timer/wait/lifecycle gates;
- full procfs stress, native control plus two guest runs per lane, each 25 s,
  16 forkers and six readers, required progress and worker shutdown;
- FCVT native/guest oracle, precise load-PC, proc-memory seek, full-width seek,
  poll/readiness, 14/14 continuation and 19,696 scalar saturation cases;
- both AOT `wait-final/<lane>/linked-corrected.rc` 0: four frozen images accepted/used, runtime-off
  parity and zero emitted code; no ABI relabelling or retraining;
- gadget release/debug gzip 33 native/guest bitstream, malformed-input,
  partial-output/archive and successful file-lifecycle checks each;
- native O0/O2 100 exact restart, 8,576 integer and 2,720 memory cases each,
  context-owner rejection and state preservation;
- netlink release/debug 211 checks and interface binding;
- three Xcode bridge tests/10 assertions plus real Linux Meson native/image
  configuration reset to gadgets/no emission/no images;
- generator/kit 13 tests/65 assertions, documentation links/style and diff checks.

The intermediate `*-upstream-verified` runners were edited while earlier shell
processes were still reading them; three failed with shifted shell lines/path
errors. Those are retained and not counted as passes. Final runners were frozen
before launch, hashed and rerun in fresh evidence directories. Subsequent
individual 25-case race tests passed, but the original frozen final procfs runs
failed with guest worker SIGSEGV in all four lanes. A sequential v2.3.3/candidate
comparison from identical archive-restored roots passed both baseline guest runs
and failed the candidate's first run. Reduced variants isolated the introduced
CoW try-lock/refusal; restoring the waiting protocol passed full stress.

The first synthetic contention fixture forced `EBUSY` forever and expected safe
refusal. It did not execute guest instructions and incorrectly treated `in_jit`
as native-only. It is replaced by a real held-reader/release fixture and actual
STP gadget execution. The refused candidate fails the latter with INT_GPF rather
than the expected BRK; the corrected path completes without replay. This is not
a general precise-restart proof for every gadget, nor a proven instruction-level
explanation of the downstream musl allocator corruption seen in the stress
trace. The introduced premature CoW refusal is isolated by controlled variants;
retaining the previous waiting semantics removes that regression. Native/AOT
execution remains separately gated. The first STP harness omitted the caller's required memory read
lock; its assertion failure is retained and is not counted as a candidate pass.

The frozen `run-wait-gates.sh` initially used a nonexistent netlink script and
an incorrect AOT recording-directory reference. These failed invocations are
retained as `wait-final/<lane>/{netlink,linked}.rc`; corrected invocations are
`netlink-corrected.rc` / `linked-corrected.rc`, all 0. The AOT assembly hashes
match the existing prototype recording directory, with no rebuilt images.
An initial native runner invocation named a nonexistent build directory; the
matching rebuilt `build-arm64-2.4.0-recorder` rerun passes at O0/O2 and is retained
under `wait-native-corrected`. All final kernel sources and upstream runners
are frozen and hashed.

## Reproduce

```sh
CC=clang make build-arm64-linux RELEASE_BUILD_DIR=build-arm64-2.4.0-gadget-release
CC=clang EVIDENCE_DIR=/absolute/new-upstream make test-arm64-upstream \
  RELEASE_BUILD_DIR=build-arm64-2.4.0-gadget-release DEBIAN_ROOTFS_DIR=/absolute/test-root
make test-xcode-gadget-guard test-aot-generator test-aot-kit
```

Use separate debug/AOT roots/builds with matching definitions. Follow
[validation](../../VALIDATION.md) for full procfs, focused and linked-image gates.
Linux 4 KiB proof does not establish Darwin 16 KiB protection, footprint/jetsam,
app foreground/background, archive signing or physical-device behaviour.
