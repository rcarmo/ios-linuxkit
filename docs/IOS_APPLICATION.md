# iOS application

The Xcode project contains two shared ARM64 application schemes. Both package the userspace Linux runtime and an AArch64 Alpine rootfs into an iOS application. The current shared version is 2.1.2 with Apple build number 808; [RELEASES.md](RELEASES.md) defines how to change them.

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
| `iSH-ARM64` | `iSH ARM64.app` | Main reference application. |
| `iSH-ARM64-ffmpeg` | `iSH ARM64 ffmpeg.app` | Test target that defines `ISH_FFMPEG_TEST=1` and registers the built-in fake FFmpeg handler. |

Build from Xcode, or use `xcodebuild` with a configured destination and signing identity:

```sh
xcodebuild \
  -project iSH.xcodeproj \
  -scheme iSH-ARM64 \
  -configuration Debug-ApplePleaseFixFB19282108 \
  -destination 'generic/platform=iOS' \
  build
```

The exact signing arguments depend on the developer account. A simulator build can use a simulator destination; device and archive builds require valid signing settings. These Xcode commands have not been run on the Debian validation host.

## Meson libraries

The ARM64 target runs the `Build Meson (ARM64)` shell phase. `app/xcode-meson.sh` creates a Darwin cross file for the active Xcode architectures and configures:

```text
guest_arch=arm64
engine=asbestos
kernel=ish
```

Ninja then builds and links:

- `libish.a` — userspace kernel and filesystems;
- `libish_emu.a` — ARM64 decoder, gadgets and TLB;
- `libfakefs.a` — fakefs metadata handling.

`app/AppARM64.xcconfig` includes `app/App.xcconfig` and `app/GuestARM64.xcconfig`. The latter supplies `GUEST_ARM64=1`, the guest architecture and the rootfs URL.

## Rootfs packaging

`app/download-root.sh` downloads the URL in `ROOTFS_URL` into the application bundle as `root.tar.gz`. Before accepting it, the script extracts `bin/busybox`, runs `file`, and verifies an AArch64 executable. A changed URL must still point to the architecture named by `ROOTFS_ARCH`.

The build downloads from the network. Pin and review a new rootfs URL in `app/GuestARM64.xcconfig`; update package-version statements only after testing the packaged image.

## Terminal frontend

`app/Terminal.m` hosts the terminal in `WKWebView`. The default ARM64 configuration loads `app/terminal/term.html`, which uses the vendored Ghostty Web JavaScript/Wasm frontend. `app/XtermRenderer.xcconfig` defines `USE_XTERM_RENDERER=1` for targets that need the alternative vendored xterm.js page.

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

The FFmpeg scheme uses this path for its test handler. Darwin builds can also map a guest command to a host executable through the `-n NAME=PATH` command-line form used by the shared launcher code. Native handlers execute with the app's host privileges and must not treat guest arguments or paths as trusted.

## Fastlane status

`fastlane/Fastfile` is inherited from upstream iSH. Its `build` lane selects scheme `iSH`, and `upload_build` publishes to upstream identifiers and `ish-app/ish`. Those lanes do not target the `iSH-ARM64` schemes without modification.

Do not use the inherited upload lane for this fork until its scheme, bundle identifiers, signing repository, TestFlight groups and GitHub repository are changed and reviewed. The generated `fastlane/README.md` only lists lane names; it is not a release runbook for `ios-linuxkit`.

## Release boundary

Linux-host validation can establish emulator and guest-runtime behaviour. It cannot validate Xcode compilation, entitlements, signing, installation, background behaviour or App Store processing. Run the relevant Linux gates before handoff, then build and smoke-test the exact iOS archive on macOS and a physical device.
