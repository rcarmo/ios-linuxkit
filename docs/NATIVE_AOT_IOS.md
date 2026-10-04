# Build an accelerated iOS app

Ahead-of-time compilation (AOT) packages translated Linux code with the iOS
app. The app does not generate executable code while running. Code without a
matching translation uses the normal interpreter.

The `iSH-ARM64-AOT-Bootstrap` scheme builds a separate test app containing
Alpine 3.24.2 and Bun 1.4.2. xterm is the default terminal; Ghostty remains
available. The reference ARM64 schemes keep the normal interpreter.

## Requirements

Use an Apple Silicon Mac with Xcode, command-line tools, Bun, Python3, Meson
and Ninja. A signed app requires your development team, bundle identifier and
a connected, unlocked iPhone or iPad.

Check out the release and its dependencies:

```sh
git clone --recurse-submodules https://github.com/rcarmo/ios-linuxkit.git
cd ios-linuxkit
git checkout v2.5.1
git submodule update --init --recursive
```

For the recording step, prepare a Darwin ARM64 recorder and a fakefs containing
the exact musl, BusyBox and Bun files bundled with the app. See
[Linux development](LINUX_DEVELOPMENT.md) for fakefs basics and
[AOT build instructions](NATIVE_AOT_BUILD_PLAN.md) for recording tools.
Linux recorder executables cannot run directly on macOS.

## Build without acceleration first

Build a signed Release app with translations disabled:

```sh
sh scripts/build-ios-aot-bootstrap.sh /absolute/path/to/observation-build \
  DEVELOPMENT_TEAM=YOUR_TEAM ROOT_BUNDLE_IDENTIFIER=YOUR_IDENTIFIER \
  CODE_SIGN_IDENTITY='Apple Development' -allowProvisioningUpdates \
  CURRENT_PROJECT_VERSION=824 AOT_IMAGE_EXECUTION=0
```

Use fresh build numbers for later builds. Install and launch this app on the
target device. Its bundle identifier ends in `.aot-bootstrap`.

Startup writes `Documents/aot-layout.json`, which describes the configuration
required by translated code. Copy that file from the device and retain the
unchanged observation app binary separately. Do not substitute a layout file
from an older app, a Mac or a simulator.

## Generate matching translations

The generator checks the app configuration, guest files and exported symbols.
Create the configuration file from the observed device layout:

```sh
bun tools/jit_aot/apple.ts contract "$APPLE_SYMBOL_BINARY" \
  /absolute/path/to/observed-layout.json /absolute/path/to/new-contract.json \
  'Observed on the target device using the corresponding app build'
bun tools/jit_aot/record-apple.ts "$DARWIN_RECORDER" "$FAKEFS" \
  "$APPLE_SYMBOL_BINARY" /absolute/path/to/new-contract.json \
  /absolute/path/to/new-apple-images
```

Use a new output directory. A mismatch is a build error: rebuild the recorder
or use the matching guest files and record again. Never change compatibility
values to make an old recording pass. Version 2.5.0 requires fresh translations
after the Bun/Pi runtime fixes.

## Build the accelerated app

```sh
sh scripts/build-ios-aot-bootstrap.sh /absolute/path/to/final-build \
  DEVELOPMENT_TEAM=YOUR_TEAM ROOT_BUNDLE_IDENTIFIER=YOUR_IDENTIFIER \
  CODE_SIGN_IDENTITY='Apple Development' -allowProvisioningUpdates \
  CURRENT_PROJECT_VERSION=825 AOT_IMAGE_EXECUTION=1 \
  AOT_IMAGES_DIR=/absolute/path/to/new-apple-images
```

The build script checks that runtime code generation is disabled and that all
three translation sets are linked. Sign the app normally; no JIT entitlement
or downloaded executable code is required.

Startup checks the translations against the running app and refuses an
incompatible set. `/proc/ish/version` identifies the installed app and image
set when troubleshooting.

## Filesystem and packages

Updating the app preserves existing Linux files and installed packages.
The bundled filesystem includes Bun but not Pi. Install Pi separately in the
guest, or import a prepared filesystem. Existing filesystems without Bun are
not changed automatically.

Package upgrades may replace files used by the translations. Unmatched code
uses the interpreter. Do not overwrite user files to force a match.
`BUN_ARCHIVE_PATH` can supply a previously downloaded Bun archive; its checksum
is still verified. Run `make test-bun-rootfs` to check packaging.

## Check on the device

Test startup, typing, text selection, scrolling with the keyboard visible,
foreground/background transitions and repeated Bun/Pi workloads. Compare
startup time, sustained performance and memory use with the ordinary app
using the same filesystem.

The accelerated build is experimental. Mac tests and successful signing do
not establish stability or performance on a physical device. Keep an ordinary
interpreter build available until those checks pass.
