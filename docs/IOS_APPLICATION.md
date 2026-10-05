# iOS application

The Xcode project contains two reference ARM64 application schemes and an isolated AOT bootstrap scheme. They package the userspace Linux runtime and an AArch64 Alpine rootfs into an iOS application. The current shared version is 2.5.2 with Apple build number 826; [RELEASES.md](RELEASES.md) defines how to change them.

## Requirements

Use macOS with Xcode and the command-line tools. Meson and Ninja must be visible to Xcode's build scripts. The project also invokes Python 3, `curl`, `tar` and `file` while preparing generated files and the rootfs.

Install the independent build tools with Homebrew if they are absent:

```sh
brew install meson ninja
```

Initialise submodules before opening the project:

```sh
git submodule update --init --recursive
```

Change the upstream default `ROOT_BUNDLE_IDENTIFIER` in `app/iSH.xcconfig` to an identifier owned by your team, then select an Apple development team before signing. The main `iSH-ARM64` target uses that root identifier; the FFmpeg target adds `.arm64.ffmpeg` to its bundle and app-group identifiers. This repository does not contain credentials or provisioning profiles.

## Schemes

| Scheme | Product | Purpose |
|---|---|---|
| `iSH-ARM64` | `LinuxKit.app` | Main reference application. |
| `iSH-ARM64-ffmpeg` | `iSH ARM64 ffmpeg.app` | Test target that defines `ISH_FFMPEG_TEST=1` and registers the built-in fake FFmpeg handler. |
| `iSH-ARM64-AOT-Bootstrap` | `LinuxKit AOT Bootstrap.app` | Experimental acceleration using static translations, with runtime code generation disabled. |

The main product name comes from `app/App.xcconfig`. Packaging must use the evaluated
`PRODUCT_NAME`, `EXECUTABLE_NAME` and bundle identifiers from Xcode build settings.

Build from Xcode, or use `xcodebuild` with a configured destination and signing identity:

```sh
xcodebuild \
  -project iSH.xcodeproj \
  -scheme iSH-ARM64 \
  -configuration Debug-ApplePleaseFixFB19282108 \
  -destination 'generic/platform=iOS' \
  build
```

The exact signing arguments depend on the developer account. A simulator build can use a simulator destination; device and archive builds require valid signing settings.

Xcode 27 requires an iOS deployment target of at least 15.0. Pass
`IPHONEOS_DEPLOYMENT_TARGET=15.0` when building with that toolchain; the inherited
project default remains 11.0 for older Xcode installations.

There is no supported `make ipa`/`ldid` fakesigning workflow in this repository.
Fakesigning alone does not provide the provisioning or app-group entitlements
required for installation on an ordinary non-jailbroken device. App and embedded
extension identifiers, entitlements and signing must agree with the installation
method. No prebuilt IPA or sideloading support is promised.

## Meson libraries

The ARM64 target runs the `Build Meson (ARM64)` shell phase. `app/xcode-meson.sh` creates a Darwin cross file for the active Xcode architectures and configures:

```text
guest_arch=arm64
engine=asbestos
kernel=ish
jit=false
jit_emit=false
cli_aot=[]
```

ARM64 Darwin Meson builds define `_XOPEN_SOURCE=700` and `_DARWIN_C_SOURCE`
to expose the POSIX/Darwin declarations used by host-context code. Other host
combinations do not receive these flags. `make test-darwin-feature-macros` checks
that scope using Meson and a local compiler; it is not an Apple SDK build.

The bridge resets all three native/image settings together in fresh and reused
build directories. `app/xcode-build-arm64.sh` stops the Xcode phase on Meson or
Ninja failure and checks each archive before publishing its linker symlink.
An environment override cannot turn a reference scheme into an AOT target.
Only the guarded bootstrap target uses `jit=true`, still with `jit_emit=false`
and empty CLI images, in its separate `meson-aot-bootstrap` directory. Ninja
then builds and links:

- `libish.a` — userspace kernel and filesystems;
- `libish_emu.a` — ARM64 decoder, gadgets and TLB;
- `libfakefs.a` — fakefs metadata handling.

`app/AppARM64.xcconfig` includes `app/App.xcconfig` and `app/GuestARM64.xcconfig`. The latter supplies `GUEST_ARM64=1`, the guest architecture and the rootfs URL.

## Rootfs packaging

`app/download-root.sh` downloads the URL in `ROOTFS_URL` into a temporary file, verifies the required `ROOTFS_SHA256`, then extracts `bin/busybox`, runs `file`, and verifies an AArch64 executable before atomically installing `root.tar.gz`. Failed fetches or validation leave an existing bundle archive unchanged. A changed URL must still point to the architecture named by `ROOTFS_ARCH` and needs a reviewed checksum. The 2.3.0 source release pins Alpine 3.24.2; it does not automatically upgrade existing installed userlands.

The build downloads from the network. Pin and review a new rootfs URL in `app/GuestARM64.xcconfig`; update package-version statements only after testing the packaged image.

## Terminal frontend

`app/Terminal.m` hosts the terminal in `WKWebView`. All ARM64 schemes default to vendored xterm.js 6.0.0 with the Canvas renderer and ligatures, loading `app/terminal/xterm-term.html`. `app/AppARM64.xcconfig` includes `app/XtermRenderer.xcconfig`, which defines `USE_XTERM_RENDERER=1` while preserving the guest preprocessor definitions.

Ghostty Web 0.9.3 is also bundled and available as an alternative JavaScript/Wasm frontend. To select it, pass `TERMINAL_RENDERER_DEFINES=USE_XTERM_RENDERER=0` to `xcodebuild`; this loads `app/terminal/term.html`. Both renderers use the same native bridge.

Terminal preferences pass the palette, font family, font size, cursor colour, blink setting and cursor shape into the web frontend. The bundle includes JetBrains Mono and Fira Code Nerd Font Mono files. The native bridge registers `load`, `log`, `sendInput`, `resize` and `propUpdate` message handlers; changes to its JavaScript messages must be checked against the corresponding Objective-C handler.

`app/DebugServer.c` currently implements `debug_server_start()` as a no-op. Port 1234 is not a JSON-RPC or remote-debug interface in this revision.

## Host integration

### Bind mounts

`fakefs_bind_mount()` maps a host directory into the guest. The iOS wrapper in `app/iOSFS.m` calls this API when the app exposes host-managed files. The mount remains inside the iOS sandbox and honours the read-only argument in fakefs path resolution.

```c
int fakefs_bind_mount(const char *linux_path,
                      const char *host_path,
                      bool read_only);
```

Validate both paths before accepting them from an external caller. A bind mount bypasses ordinary fakefs storage boundaries for the selected host directory.

### Native offload

`native_offload_add_handler()` registers an in-process command handler. `kernel/exec.c` checks registered names during guest `execve`; signal and wait handling remain coupled to the guest task.

```c
native_offload_add_handler("ffmpeg", fake_ffmpeg_main);
```

Generic `ffmpeg`/`ffprobe` handlers select only exact `/bin`, `/usr/bin` or
`/usr/local/bin` paths. Relative paths and private binaries fall through to
emulated exec; readable shebang wrappers are not replaced. Set `NO_OFFLOAD=1`
(or upstream-compatible `MINIS_NO_FFMPEG_OFFLOAD=1`) in the guest environment to
disable these generic offloads. Synthetic Apple-only commands are unaffected.
The safeguard controls exec selection. Host path validation and sandbox controls
are still required.

The FFmpeg scheme uses this legacy path for its fake test handler. It retains
host-path translation, process-wide CWD and pipe-backed output; its terminal
writes have no bounded cancellation guarantee. It has not been converted to the
cooperative API. The test-only local-copy handler is never linked or registered
by the app. See [offload contracts](NATIVE_OFFLOAD.md) for the separate raw-argv,
VFS, token and restricted TCP contract.

Darwin builds can also map a guest command to a host executable through the `-n NAME=PATH` command-line form used by the shared launcher code. Native handlers execute with the app's host privileges and must not treat guest arguments or paths as trusted.

## Fastlane status

`fastlane/Fastfile` is inherited from upstream iSH. Its `build` lane selects scheme `iSH`, and `upload_build` publishes to upstream identifiers and `ish-app/ish`. Those lanes do not target the `iSH-ARM64` schemes without modification.

Do not use the inherited upload lane for this fork until its scheme, bundle identifiers, signing repository, TestFlight groups and GitHub repository are changed and reviewed. The generated `fastlane/README.md` only lists lane names; it is not a release runbook for `ios-linuxkit`.

## Device checks

Build and smoke-test the exact signed archive on a physical device before
distribution. Linux tests exercise emulator behaviour; Xcode compilation,
entitlements, installation, background operation and App Store processing need
Apple tools and hardware.

Check terminal and upgrade-session creation, native-offload wrappers and opt-outs,
timers and interrupted sleeps, repeated fork/exec/exit with concurrent procfs/ps
scans, memory pressure, precise load faults and foreground/background transitions.
The [source release records](reports/README.md) retain the Linux tests and
source-review results for earlier app changes.

## AOT app integration

The `iSH-ARM64-AOT-Bootstrap` scheme builds a separate test app with Bun.
Acceleration is off by default. Set `AOT_IMAGE_EXECUTION=1` and provide matching
translations through `AOT_IMAGES_DIR` to enable it. The app cannot generate
executable code at runtime. Reference schemes use the normal interpreter.

`sh scripts/build-ios-aot-bootstrap.sh DERIVED_DATA CODE_SIGNING_ALLOWED=NO`
builds an unsigned device binary and inspects its actual SDK/platform, native
compiler definitions, required symbols and absence of known emitter primitives.
Signing and physical-device testing are also required. The
[Apple AOT instructions](NATIVE_AOT_IOS.md) cover generating matching
translations, building the app and checking it on a device.
