# Reusable native/AOT prototype artifact kit

Prototype tools, based on v2.3.0; ordinary builds remain default-off. No new
release or accelerated iOS application. See [the rollout plan](NATIVE_AOT_BUILD_PLAN.md)
for hardware, measured performance and Apple-only acceptance gates.

`tools/jit_aot/kit.ts` provides explicit stages. Run its `help` command. Use an
absolute durable directory outside `/tmp`; generated outputs must not exist and
their parent must exist. Failed stages retain `.partial-PID` diagnostics and do
not publish a complete output. Run `make test-aot-kit test-aot-generator`.

## Inputs and freezing

Stop **all** guests using the source fakefs. `prepare ROOT RECORDINGS
GADGET_BUILD OUT --quiescent` requires a committed clean source checkout and
checks the targeted recorder's manifest, module/recording/image hashes and ABI.
It retains a raw fakefs archive including SQLite WAL plus a portable guest tar
exported via `unfakefsify` from a private clone. It compares complete source
backing-file hashes/modes before/after; this is mutation detection, not locking
against an uncooperative external writer. The caller owns quiescence.

A plain archive of `data/` is not a guest rootfs: symlink targets and guest
permissions live in fakefs metadata. Restore through `fakefsify` using
`rootfs.tar.gz`; this rebuilds inode identities. Retain `meta.db` and any WAL with
the raw snapshot for forensic recovery. Installed APK metadata, repository URLs
and public keys are included; exact APK archives are not. Exact guest files are
in the export. No package installation or existing-userland upgrade occurs.

Each sealed directory has a versioned JSON manifest with every payload file's
SHA-256, size and mode. Paths are relative; no payload symlinks or unlisted files
are accepted. The manifest is not a signature: use a separately trusted checksum
when distributing. Do not feed untrusted archives or manifests to these tools.

## Stages

```sh
bun tools/jit_aot/kit.ts prepare "$ROOT" "$RECORDINGS" "$GADGET_BUILD" "$SEED" --quiescent
bun tools/jit_aot/kit.ts verify "$SEED"
bun tools/jit_aot/kit.ts restore "$SEED" "$RUN"
bun tools/jit_aot/kit.ts generate "$SEED" "$IMAGES" elf
bun tools/jit_aot/kit.ts build "$SEED" "$IMAGES" "$BUILD"
```

`build` creates fresh gadget, AOT release and AOT debug Meson directories and
prints a pending directory. AOT compiles with `jit_emit=false`. Validate that
pending directory before `publish PENDING BUILD`: an `evidence/acceptance.json`
with passing status is required. Publication removes non-relocatable build
caches but retains binaries, compiler/Meson options, source snapshot and logs.
The acceptance record is an evidence assertion, not a substitute for running
its gates. Source archives contain tracked source and submodule revision lists;
submodules are not embedded. The Linux build uses system SQLite/libarchive.

```sh
bun tools/jit_aot/kit.ts run "$BUILD" "$RUN/root" release /bin/sh
bun tools/jit_aot/kit.ts run "$BUILD" "$RUN/root" off /bin/sh
bun tools/jit_aot/kit.ts run "$BUILD" "$RUN/root" gadget /bin/sh
```

Launch sanitises all inherited `ISH_JIT*` and `ISH_AOT*` knobs and sets explicit
mode/stats/family values. It verifies the immutable bundle, not the writable
userland on each launch: changed packages must fall back safely. Do not launch
against the source snapshot or start concurrent writers on one restored root.
For retraining only, `record RECORDER WRITABLE_ROOT OUTPUT` wraps the existing
checked four-target pipeline; new recordings require a new seed and validation.

## Apple handoff, not a portable Linux executable

The seed is the shared Linux/iOS input kit: raw PIC JSONL, exact module bytes,
workload, recorder binary/provenance, generator source and package identity.
Linux binaries and ELF assembly cannot be linked directly into an iOS app.

```sh
bun tools/jit_aot/kit.ts generate "$SEED" "$MACHO_IMAGES" macho \
  "$APPLE_SYMBOL_BINARY" "$OBSERVED_APPLE_CONTRACT"
```

Generation extracts only the hash-checked module bytes using host `tar`; it does
not execute the retained Linux recorder/importer on macOS. Install Bun, Python3
and Apple command-line tools (`nm`) on the Mac. Use the same generator source.

The contract JSON must contain numeric `abi`, `prologue_words`, `entry_off`,
`n_pinned`, plus `binarySha256`, `arch: "aarch64"`, `endian: "little"`,
`pointerBits: 64`, `platform` (`ios`, `ios-simulator` or `macos`) and nonempty
`evidence` identifying actual target runtime/layout
observations. Obtain these from the bootstrap target's `/proc/ish/jit`,
`jit_abi()` and debugger/layout inspection using its real SDK/flags, not by
copying the Linux header. The command checks every header, binary hash and
required defined host symbol, with Mach-O underscore handling. It requires a
thin ARM64 Mach-O symbol binary, rejects undefined imports and never overrides ABI.
The source generator also rejects unnamed code relocations; the kit adds checks
for gadget key symbols, which could otherwise be null. Missing symbols or ABI
mismatch fail before publication. These are rejection guards, not proof of ABI
semantic equivalence or device correctness.

If incompatible, record using a matching Darwin/target-layout recorder and
repeat identity checks; macOS alone does not prove iOS layout equality. Apple
ISA/platform registers, pointer authentication, FP/SIMD preservation, page-size
and precise fault context remain device gates. The current Linux kit does not
run Apple binaries or claim Apple contract validity.

Required Apple implementation remains: isolated Xcode configuration, matching
shared structure defines, app precise-fault recovery (not CLI `ucontext` copying),
Mach-O assembly build membership and constructor retention, compile-time
no-emission mode, normal static signing, exact guest installation/fallback,
physical-device lifecycle/stress/rollback/size/performance/energy tests. No JIT
entitlement or downloaded executable-code workaround. Keep existing schemes off.

## Required local acceptance

Retained-source and restored-module hashes; release/debug linked no-emitter
parity; hardware PC hits within all four images with emitter region NULL;
upstream, poll, FCVT, precise load-PC, seek/poke, continuation and full procfs
stress; exact native-restart/oracle tests; deliberate incompatible image rejection;
fresh-process performance/startup/memory/size measurements; clean-directory
restore and offline regeneration without retraining. [Prototype evidence](reports/audits/AOT_ARTIFACT_KIT_2026-09-29.md)
records passed gates and explicit Apple pending gates separately.
