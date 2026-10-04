# Prepare AOT images for iOS

The repository can generate Mach-O assembly from retained PIC recordings.
An isolated `iSH-ARM64-AOT-Bootstrap` target now compiles the native backend
with no runtime emitter, retains its symbols and runs gadgets only. The iOS app
installs a native-checkpoint-only fault adapter and observes its no-emitter ABI.
It can also link checked static musl/BusyBox/Bun images when explicitly enabled.
The separate
[shared-recovery preparation](reports/audits/SHARED_NATIVE_RECOVERY_2026-10-02.md)
contains the original Linux-validated source scaffolding. Real Darwin fault
tests, static CLI execution and simulator app startup are now covered; physical
device execution and pi stability remain separate acceptance gates. See the
[Apple integration report](reports/audits/APPLE_AOT_2026-10-04.md).

Use this procedure on an Apple Silicon Mac after reproducing the
[Linux AOT build](NATIVE_AOT_BUILD_PLAN.md). Keep the existing gadget-only schemes
available throughout the work. Source release 2.5.0 includes the handoff tools,
Apple static-image integration, native fault adapter and Bun/pi emulator fixes.
Reference app execution stays on gadgets. Code version 12 requires newly
recorded images bound to the observed target contract; version-11 images
cannot validate this release.

## Inputs and tools

Retain a verified [seed](NATIVE_AOT_ARTIFACT_KIT.md) containing raw PIC JSONL,
exact guest module files, workload, recorder hash, source and package inventory.
The recorded native words include structure offsets and register conventions;
they require comparison with the actual target build.

On the Mac, install Xcode/command-line tools, Bun, Python3, Meson and Ninja.
Generation uses Apple `nm` and host `tar`. Record the Mac model/SoC/RAM,
macOS/Xcode/SDK versions, and target iPhone/iPad model/SoC/RAM/iOS version.
The first bootstrap build and device-install results are recorded in the
[Bun/pi test-build report](reports/benchmarks/PI_BUN_DARWIN_2026-10-04.md).

```sh
brew install meson ninja python
git clone --recurse-submodules https://github.com/rcarmo/ios-linuxkit.git
cd ios-linuxkit
git checkout v2.5.0
git submodule update --init --recursive
```

Install Bun using its [installation instructions](https://bun.sh/docs/installation).
The retained recorder and `fakefsify` binaries are Linux executables. Do not run
them on macOS. Mach-O generation extracts only the four hash-checked guest
files from `rootfs.tar.gz` and reads the Apple binary's symbol table.

## Build the gadget reference app

Follow [iOS application](IOS_APPLICATION.md) to set your bundle identifier,
app groups and signing team. First build and run the existing `iSH-ARM64` scheme.
The inherited Fastlane upload lane targets upstream iSH; change and review its
scheme, identifiers, repository and distribution settings before uploading.

```sh
xcodebuild -project iSH.xcodeproj -scheme iSH-ARM64 \
  -configuration Debug-ApplePleaseFixFB19282108 \
  -destination 'generic/platform=iOS' build
```

Signing arguments depend on the developer account. Confirm terminal startup,
rootfs creation, process execution and foreground/background behaviour on the
reference device. Simulator compilation does not test device signing or memory
policy.

## Implement an isolated AOT configuration

The bootstrap is available for Apple compilation and symbol inspection:

```sh
sh scripts/build-ios-aot-bootstrap.sh /absolute/path/to/new-derived-data \
  CODE_SIGNING_ALLOWED=NO
```

For a signed bootstrap, replace `CODE_SIGNING_ALLOWED=NO` with the configured
`DEVELOPMENT_TEAM`, `ROOT_BUNDLE_IDENTIFIER` and `-allowProvisioningUpdates`.
Its `.aot-bootstrap` bundle suffix and private application-support filesystem
keep it separate from the reference app. It has no file-provider extension and
does not require an app-group entitlement. The script uses iOS 15 as the minimum
for Xcode 27.
Set `IOS_DESTINATION='id=DEVICE_UDID'` to provision for a connected device.

The bootstrap rootfs adds Bun 1.4.2 for Linux AArch64 musl to a fresh Alpine
3.24.2 archive. Alpine, Bun and the required libgcc/libstdc++ payloads are
SHA-256 pinned. Packaging preserves base ownership, modes and links and
publishes atomically. The runtime library payloads are not registered as APK
packages; provenance is stored in `/usr/share/linuxkit/bun.json`. Reference
schemes still use the unmodified Alpine rootfs. Existing bootstrap userlands
are not overwritten; import the new bundled filesystem to get Bun.

Run `make test-bun-rootfs` for archive-preservation and failure tests.
`BUN_ARCHIVE_PATH` can supply a previously downloaded, checksum-verified Bun
zip to Xcode. `AOT_INCLUDE_BUN=1 make record-arm64-aot` records Bun as an
optional fifth module after checking its version and running arithmetic/JSON.
The guest must already contain the baseline Python/zlib package set. This
does not yet prove Bun native-image execution on Apple devices.

The [Bun/pi test-build report](reports/benchmarks/PI_BUN_DARWIN_2026-10-04.md)
describes the separately prepared pi filesystem, launcher and repeatable
offline workload. pi is not part of the default bundled rootfs. The initial
device installation is for testing; intermittent guest crashes remain unresolved.

The guarded bridge allows `jit=true` only for this target, plain ARM64,
matching `GUEST_ARM64`, `ISH_JIT`, `ISH_JIT_NO_EMIT`, `ISH_AOT_BOOTSTRAP`
definitions and a separate `meson-aot-bootstrap` directory. Emission and CLI
images are always cleared together. Default startup keeps image execution off.
The installed adapter forwards non-native faults to the previous signal policy
and stops unmatched native faults; it does not enable gadget replay.

`tools/jit_aot/apple.ts inspect` checks the built backend options/definitions,
known code-mapping/protection symbols, required defined app symbols and the
actual Mach-O SDK/platform. Non-executable context-data `mmap` is allowed.
These static checks do not prove runtime memory policy or device correctness.
Explicit `jit_aot_prepare_layout` publishes immutable PIC/pinning conventions
without enabling execution, accepting images or allocating executable memory.
Startup saves the actual `ready=1` layout in `Documents/aot-layout.json`.
`make test-apple-aot` exercises actual statically linked Mach-O faults at O0/O2.

The accelerated app still requires the following work:

1. Derive an accelerated target from the bootstrap after the recovery and
   image gates below pass. Keep the reference schemes on gadgets and retain
   separate Meson directories. Remove the bootstrap's runtime-off guard only
   when actual native fault recovery and validated image membership exist.
2. Apply matching `ISH_JIT` and guest/frame definitions to every app and library
   translation unit using shared structures. Compare compiler flags, structure
   offsets, pinned registers, entry/prologue and TLB/context constants.
3. Connect native precise-fault recovery to the app's actual Apple fault path.
   `main.c` uses the shared core with CLI gadget replay. The prepared
   `ish_app_native_fault_recover` adapter accepts only native checkpoints and is
   installed by bootstrap startup. Verify it on physical devices as well as
   Darwin; it fail-stops on FATAL and preserves prior policy on UNHANDLED. Preserve the
   real signal/Mach context, exact guest PC, registers, FP/SIMD state and checkpoint lifetime. Unmatched native faults must stop execution.
4. Build a bootstrap app binary with the target ABI and exported gadget/backend
   symbols. Do not strip the symbol binary used for generation. `cli_aot` affects
   the CLI only; add generated assembly to app build membership separately.
5. Link static image constructors so dead stripping cannot remove registration.
   Assemble using the same SDK/architecture as the app. Rebuild the final binary
   and check that ABI and symbols still match the bootstrap used for generation.

The Linux ABI at code version10 is `3f650e41` for the tested pinned/PIC layout.
A matching hash checks selected offsets and conventions. Also review Apple
calling conventions, reserved x18, arm64e/pointer authentication, page sizes,
cache/invalidation and fault-context handling. Targeting plain arm64 does not
remove the need for those checks.

## Record the observed target contract

`kit.ts generate … macho` requires a thin ARM64 Mach-O symbol binary and a JSON
contract tied to its SHA-256. Universal binaries must first be reduced to the
intended ARM64 slice with the Apple tools. Collect fields from that build:

| Field | Type | Source |
|---|---|---|
| `binarySha256` | String | SHA-256 of the exact symbol binary supplied to generation. |
| `abi` | Integer | `jit_layout_read` or `/proc/ish/jit-layout` with `ready=1` on the target. |
| `prologue_words` | Integer | Ready target layout diagnostic after normal initialisation. |
| `entry_off` | Integer | Ready target layout diagnostic for the same configuration. |
| `n_pinned` | Integer | Ready target register-pinning count. |
| `arch` | String | `aarch64`. |
| `endian` | String | `little`. |
| `pointerBits` | Integer | `64`. |
| `platform` | String | `ios`, `ios-simulator` or `macos`; choose the build actually observed. |
| `evidence` | String | Paths/references to the target logs, debugger observations and build options. |

Do not copy the recording header into the contract. Observe the target with its
actual SDK and flags. Native builds expose `jit_layout_read` and
read-only `/proc/ish/jit-layout`. It never initialises the backend or creates
executable mappings. Accept its convention fields only with `ready=1` after
normal target initialisation or explicit no-emitter preparation; `ready=0` is not
a usable contract. Explicit preparation is independent of linked images and does
not copy recording values. A macOS observation applies
to that macOS build; collect a separate iOS contract before app linkage.

`requireAppleBinary` checks the thin Mach-O ARM64 header and the declared
platform name. It does not inspect SDK load commands, entitlements or signing.
The operator must verify those against the build record.

After collecting an actual ready/no-emitter/PIC target layout, create a
binary-bound contract without copying the recording's values:

```sh
bun tools/jit_aot/apple.ts contract "$APPLE_SYMBOL_BINARY" \
  /absolute/path/to/observed-layout.json /absolute/path/to/new-contract.json \
  'Target device, exact build and debugger/log evidence for this observation'
```

This command reads the SDK/platform from Mach-O load commands and refuses
`ready=0`, compiled emission, non-PIC conventions and an existing output file.
The evidence must refer to the same binary; the operator collects that evidence.

## Generate and inspect Mach-O assembly

Set these paths to existing inputs and a new output directory:

```sh
SEED=/absolute/path/to/seed
APPLE_SYMBOL_BINARY=/absolute/path/to/bootstrap-app-binary
CONTRACT=/absolute/path/to/observed-target.json
MACHO=/absolute/path/to/new-macho-images
bun tools/jit_aot/kit.ts verify "$SEED"
bun tools/jit_aot/kit.ts generate "$SEED" "$MACHO" macho \
  "$APPLE_SYMBOL_BINARY" "$CONTRACT"
bun tools/jit_aot/kit.ts verify "$MACHO"
```

The wrapper compares every recording header with the contract, checks the
binary hash, requires defined gadget/backend symbols and resolves Mach-O leading
underscores. Unresolved code or gadget-key symbols fail generation. The output
contains `aot_musl.S`, `aot_busybox.S`, `aot_python.S`, `aot_zlib.S`, logs and a
manifest. Inspect translation counts and constructor/linker output before
executing the linked app.

If fields differ, stop. Do not supply `--abi` to relabel a recording or remove the
runtime rejection. Build a recorder with matching Darwin/target layouts and
retrain the exact guest modules. The current automated recording/snapshot path
runs on Linux; recorder command deadlines now use Bun's subprocess timeout, not
GNU `timeout`. A Darwin recorder still needs verified export tools.
`targeted.ts … macho` uses the recorder's symbols and
does not perform the target checks above.

## Package the guest and sign

For the bundled Apple/Bun userland (without the Linux kit's Python/zlib workload),
train a native Darwin recorder against the exact guest files and an observed
target contract:

```sh
bun tools/jit_aot/record-apple.ts "$DARWIN_RECORDER" "$FAKEFS" \
  "$APPLE_SYMBOL_BINARY" "$CONTRACT" /absolute/path/to/new-apple-images
sh scripts/build-ios-aot-bootstrap.sh /absolute/path/to/derived-data \
  AOT_IMAGE_EXECUTION=1 AOT_IMAGES_DIR=/absolute/path/to/new-apple-images \
  DEVELOPMENT_TEAM=YOUR_TEAM ROOT_BUNDLE_IDENTIFIER=YOUR_IDENTIFIER \
  -allowProvisioningUpdates
```

The image directory is hash/mode sealed and its SDK contract must match the
destination. Xcode compiles static assembly with its actual SDK, force-loads
constructors, and retains emission-disabled backend definitions. Startup refuses
missing/rejected images or mismatched conventions. Untrained blocks use gadgets.
`aot-build.json` records the image manifest hash, source revision and dirty state;
the app logs its version/build/image identity and exposes it in `/proc/ish/version`.
Do not confuse image installation counters with sampled native-PC coverage or
claim pi stability from the arithmetic/crypto training workload.

Images serve specific guest module bytes. The stock Alpine minirootfs includes
musl and BusyBox; the trained Python/zlib package set must also be installed for
those images to match. Existing app userlands can contain different versions.
Keep fallback working after package upgrades and never replace users' files to
force image acceptance.

Link translations into the application before signing. Enforce `jit_emit=false`
in the compiled backend and inspect final imports and runtime mappings. Do not
use a JIT entitlement, RWX mapping or downloaded executable image as a substitute.
Use normal app signing and verify the archive's entitlements on a physical device.

## Device acceptance and distribution

For the exact signed archive, retain:

- image acceptance/rejection logs and actual execution PCs inside each image;
- zero runtime-emission evidence and final binary/guest hashes;
- native-oracle outputs, precise fault/retry checks, chained entry, invalidation,
  OOM, thread lifecycle and full procfs/fork/exec/exit stress;
- missing/stale-image fallback and an accessible gadget-only rollback;
- terminal input/resize, foreground/background, memory pressure and package
  upgrade checks;
- startup, sustained workloads, peak memory, app size, thermals and energy
  compared with the same gadget-only app and guest.

Evaluate musl and BusyBox first. Include Python/zlib only when matched target
workloads justify their timing, footprint and app-size cost; the Linux Python
regression is a reason to measure rather than assume a benefit. Assign a fresh Apple build number to every
upload. Publish archive/signing/device evidence with the release record after
these checks pass.
