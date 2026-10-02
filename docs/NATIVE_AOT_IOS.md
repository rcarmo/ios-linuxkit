# Prepare AOT images for iOS

The repository can generate Mach-O assembly from retained PIC recordings.
The iOS app still needs backend configuration, actual fault-adapter installation
and image linkage before those images can execute. The separate
[shared-recovery preparation](reports/audits/SHARED_NATIVE_RECOVERY_2026-10-02.md)
contains Linux-validated source scaffolding. No existing app scheme installs
that adapter, and no accelerated Apple archive has been built or device-tested.

Use this procedure on an Apple Silicon Mac after reproducing the
[Linux AOT build](NATIVE_AOT_BUILD_PLAN.md). Keep the existing gadget-only schemes
available throughout the work. Source release 2.4.1 includes the handoff tools,
shared recovery and read-only layout diagnostics; it leaves the app's execution
settings unchanged.

## Inputs and tools

Retain a verified [seed](NATIVE_AOT_ARTIFACT_KIT.md) containing raw PIC JSONL,
exact guest module files, workload, recorder hash, source and package inventory.
The recorded native words include structure offsets and register conventions;
they require comparison with the actual target build.

On the Mac, install Xcode/command-line tools, Bun, Python3, Meson and Ninja.
Generation uses Apple `nm` and host `tar`. Record the Mac model/SoC/RAM,
macOS/Xcode/SDK versions, and target iPhone/iPad model/SoC/RAM/iOS version. These
Apple hardware and software versions have not yet been selected or tested.

```sh
brew install meson ninja python
git clone --recurse-submodules https://github.com/rcarmo/ios-linuxkit.git
cd ios-linuxkit
git checkout v2.4.1
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

The following changes require code/build work on the Mac; the named scheme and
app linkage do not exist yet.

1. Create a separate AOT scheme/configuration. Keep the reference schemes on
   gadgets. Add a separate build bridge for `jit=true` and **`jit_emit=false`**;
   `app/xcode-meson.sh` deliberately resets existing schemes to gadgets, no
   emitter and no CLI images. Use separate Meson build directories.
2. Apply matching `ISH_JIT` and guest/frame definitions to every app and library
   translation unit using shared structures. Compare compiler flags, structure
   offsets, pinned registers, entry/prologue and TLB/context constants.
3. Connect native precise-fault recovery to the app's actual Apple fault path.
   `main.c` uses the shared core with CLI gadget replay. The prepared
   `ish_app_native_fault_recover` adapter accepts only native checkpoints and is
   not installed by existing schemes. Integrate it with the real app fault path;
   fail-stop on FATAL and preserve the prior policy on UNHANDLED. Preserve the
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
normal target initialisation; `ready=0` is not a usable contract. If a no-image
bootstrap cannot select conventions normally, use matching target recorder/build
observations rather than copying recording values. A macOS observation applies
to that macOS build; collect a separate iOS contract before app linkage.

`requireAppleBinary` checks the thin Mach-O ARM64 header and the declared
platform name. It does not inspect SDK load commands, entitlements or signing.
The operator must verify those against the build record.

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
runs on Linux and uses GNU `timeout`; a Darwin recorder needs its own verified
runner and export tools. `targeted.ts … macho` uses the recorder's symbols and
does not perform the target checks above.

## Package the guest and sign

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
