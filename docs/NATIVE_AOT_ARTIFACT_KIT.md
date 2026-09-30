# Freeze, rebuild and share AOT artifacts

`tools/jit_aot/kit.ts` packages the exact guest, recordings and build provenance
needed to reproduce a Linux AOT executable. A seed also supplies the inputs for
[Apple image generation](NATIVE_AOT_IOS.md).

Use the [Linux AOT guide](NATIVE_AOT_BUILD_PLAN.md) to build a recorder and train
musl, BusyBox, Python and zlib. The commands here start with its `WORK`, `ROOT`,
`RECORDINGS` and `GADGET_BUILD` variables. All paths must be absolute. Output
parents must exist, outputs must be new, and only one writer may use each path.

## Payloads and trust

| Directory kind | Contents |
|---|---|
| Seed | Raw fakefs snapshot, portable guest archive, JSONL, original images/recorder, workload, package metadata, source archive and toolchain record. |
| Generated | Four ELF or Mach-O assemblies, generator logs, headers/symbol requirements, workload and seed-manifest hash. |
| Build | Gadget/release/debug binaries, build options, source archive, logs and acceptance evidence. |
| Restored guest | Writable `root/data` and SQLite metadata for a new run; keep outside sealed payloads. |

A completed seed, generated set or build has an `ish-aot-kit/v1` manifest.
`verify` checks every payload file's SHA-256, size and mode and rejects extra
files and symlinks. The manifest itself can be edited, so distribute a trusted
archive checksum separately. Use trusted input archives; the kit does not
sandbox native helpers or validate hostile tar files.

Publication renames a staging directory to the output on the same filesystem.
Failed stages retain `.partial-PID` files. This protects against publishing an
incomplete run; it provides no lock against another process writing the same
paths. Keep both output and source under a single operator's control.

## Freeze the training guest

Stop every guest using `ROOT`. Include SQLite `meta.db`, `meta.db-wal` and
`meta.db-shm` with the backing data. Guest permissions and symlink types are
stored in metadata, so archiving `data/` alone loses them.

Use a clean Git checkout at the intended source revision. `prepare` and `build`
run `git archive`; they refuse tracked or untracked working-tree changes. Keep
artifact outputs outside the checkout. A build's source revision records the
checkout used by the kit; the original recorder hash/revision stays separate.

```sh
SEED="$WORK/seed"
ELF="$WORK/elf"
PRODUCT="$WORK/linux"
bun tools/jit_aot/kit.ts prepare \
  "$ROOT" "$RECORDINGS" "$GADGET_BUILD" "$SEED" --quiescent
bun tools/jit_aot/kit.ts verify "$SEED"
```

`prepare` checks module, JSONL, image, workload and original recorder hashes. The
original recorder must still exist at the path in the recording manifest. It
copies the raw fakefs, exports `rootfs.tar.gz` from a private clone and compares
source content/mode hashes before and after. `--quiescent` is your confirmation
that no guest is running; the script cannot stop an external writer.

The seed retains installed APK metadata, repository URLs and public keys, plus
all installed guest bytes. It does not fetch original APK archives. Keep any
available package archives separately if later package installation must work
without repositories. Source archives list submodule revisions but omit their
contents; the Linux CLI uses system SQLite/libarchive. Apple builds need the
recorded submodules.

## Restore and regenerate without training

```sh
bun tools/jit_aot/kit.ts restore "$SEED" "$WORK/restored"
RUN_ROOT="$WORK/restored/root"
bun tools/jit_aot/kit.ts generate "$SEED" "$ELF" elf
bun tools/jit_aot/kit.ts verify "$ELF"
```

Restore uses the retained Linux `fakefsify` binary and imports the portable
archive with new host inode numbers. Run it on a compatible AArch64 Linux host.
Generation extracts the four module files using host `tar`, checks their hashes
and resolves symbols with `nm`; it does not execute the recorder. The resulting
assembly uses the original recording ABI. Unresolved symbols stop generation.

`record RECORDER WRITABLE_ROOT OUTPUT` is available for deliberate retraining.
It writes the targeted recorder result under `OUTPUT/recordings`; pass that
subdirectory to `prepare`. Use a new seed after retraining.

## Build and validate before publishing

```sh
bun tools/jit_aot/kit.ts build "$SEED" "$ELF" "$PRODUCT" > "$WORK/build.log" 2>&1
PENDING=$(sed -n 's/^BUILT_PENDING=//p' "$WORK/build.log")
test -n "$PENDING"
test -f "$PENDING/pending.json"
```

The build creates fresh gadget, AOT release and AOT debug binaries. Both AOT
configurations use `jit_emit=false`. `PENDING` is a staging directory; leave it
at its printed path until validation finishes because Meson stores absolute
paths. Build commands require a clean Git checkout and Clang/Meson/Ninja/Make.

Run the following from the repository root. Each configuration receives a fresh
guest. The relative build path is needed by older Make targets that prepend
`CURDIR`. Evidence directories must be separate for each runner.

```sh
set -eu
mkdir -p "$PENDING/evidence"
for MODE in release debug; do
  bun tools/jit_aot/kit.ts restore "$SEED" "$WORK/test-$MODE"
  TEST_ROOT="$WORK/test-$MODE/root"
  BUILD=$(realpath --relative-to="$PWD" "$PENDING/build-$MODE")
  EVIDENCE_DIR="$PENDING/evidence/linked-$MODE" make test-arm64-linked-aot \
    RELEASE_BUILD_DIR="$BUILD" ROOTFS_DIR="$TEST_ROOT" AOT_RECORD_DIR="$ELF" \
    > "$PENDING/evidence/linked-$MODE.log" 2>&1
  CC=clang HOST_CC=clang EVIDENCE_DIR="$PENDING/evidence/upstream-$MODE" \
    make RELEASE_BUILD_DIR="$BUILD" ROOTFS_DIR="$TEST_ROOT" DEBIAN_ROOTFS_DIR="$TEST_ROOT" \
    test-arm64-upstream > "$PENDING/evidence/upstream-$MODE.log" 2>&1
  CC=clang HOST_CC=clang make RELEASE_BUILD_DIR="$BUILD" \
    ROOTFS_DIR="$TEST_ROOT" DEBIAN_ROOTFS_DIR="$TEST_ROOT" REPORT_DIR="$PENDING/evidence" \
    test-arm64-poll-regular test-arm64-fcvt-vector test-arm64-load64-fault-pc \
    test-arm64-proc-mem-seek test-arm64-lseek-width test-arm64-poke-stress \
    test-arm64-internal-continue-fixtures > "$PENDING/evidence/focused-$MODE.log" 2>&1
  CC=clang EVIDENCE_DIR="$PENDING/evidence/proc-$MODE" make RELEASE_BUILD_DIR="$BUILD" \
    DEBIAN_ROOTFS_DIR="$TEST_ROOT" test-arm64-proc-exit-race \
    > "$PENDING/evidence/proc-$MODE.log" 2>&1
done
EVIDENCE_DIR="$PENDING/evidence/native-restart" make test-arm64-native-emitter \
  NATIVE_BUILD_DIR=build-arm64-recorder > "$PENDING/evidence/native-restart.log" 2>&1
bun tests/arm64/native-aot/execution.ts "$PENDING/build-debug/ish" \
  "$RUN_ROOT" "$PENDING/evidence/execution" > "$PENDING/evidence/execution.log" 2>&1
```

A failed command stops the sequence. Preserve its logs and fix the failure
before publication. `execution.ts` needs GDB and a debug binary. Native restart
checks use a separate emission-capable recorder build.

For a full export identity check, compare offline copies before starting guests:

```sh
mkdir "$WORK/raw-copy"
tar -xzf "$SEED/fakefs-snapshot.tar.gz" -C "$WORK/raw-copy"
bun tools/jit_aot/kit.ts restore "$SEED" "$WORK/identity-copy"
bun tests/arm64/native-aot/fakefs-identity.ts \
  "$WORK/raw-copy" "$WORK/identity-copy/root" "$PENDING/evidence/identity.json"
```

This compares every logical path, guest stat blob, hardlink group and file byte,
ignoring host inode numbers and timestamps. SQLite may update shared-memory
files even on a read-only connection; inspect copies only.

After the commands pass, inspect the pass markers and reports. Write an
acceptance record naming the actual logs and any limits. A minimal record is:

```json
{
  "status": "pass",
  "checks": ["linked-release.log", "linked-debug.log", "upstream-release.log", "upstream-debug.log", "focused-release.log", "focused-debug.log", "proc-release.log", "proc-debug.log", "native-restart.log", "execution.log", "identity.json"],
  "limits": ["Apple SDK, signing and physical-device tests have not run"]
}
```

Save it as `PENDING/evidence/acceptance.json`. `publish` checks only that the
record says `status: pass`; the operator must inspect and retain the evidence.
It deletes non-relocatable build caches. Keep a separate copy first if more
archive-linked tests are needed.

```sh
bun tools/jit_aot/kit.ts publish "$PENDING" "$PRODUCT"
bun tools/jit_aot/kit.ts verify "$PRODUCT"
bun tools/jit_aot/kit.ts run "$PRODUCT" "$RUN_ROOT" release /bin/sh
bun tools/jit_aot/kit.ts run "$PRODUCT" "$RUN_ROOT" off /bin/sh
```

The launcher removes inherited `ISH_JIT*`/`ISH_AOT*` variables and selects the
requested mode. It hashes the executable bundle on each invocation; the writable
guest is outside that manifest. Updated guest programs use normal runtime image
matching and gadget fallback.

## Archive and restore the delivered files

Keep the source checkout or a source archive with the kit. Run a clean-directory
restore/generate/build rehearsal before sharing it. A source archive can use the
explicit Meson commands in the [Linux guide](NATIVE_AOT_BUILD_PLAN.md); the kit's
`build` publication stage requires Git provenance.

```sh
# WORK contains seed/, elf/ and linux/. Retain source independently as well.
tar -czf "$WORK/local-aot.tar.gz" -C "$WORK" seed elf linux
sha256sum "$WORK/local-aot.tar.gz" > "$WORK/local-aot.sha256"
mkdir "$WORK/archive-check"
tar -xzf "$WORK/local-aot.tar.gz" -C "$WORK/archive-check"
for PART in seed elf linux; do
  bun tools/jit_aot/kit.ts verify "$WORK/archive-check/$PART"
done
```

Send the archive checksum through a trusted channel. Do not modify sealed files
or add logs within a completed payload. Retain new run evidence beside it.

The [29 September delivery](reports/audits/AOT_ARTIFACT_KIT_2026-09-29.md) includes
Linux and iOS-input archives made from the prototype. Their hashes and source
revisions identify those original files; the 2.3.1 source release does not
relabel or replace them. The [iOS procedure](NATIVE_AOT_IOS.md) uses the seed's
exact files and records the additional target checks.
