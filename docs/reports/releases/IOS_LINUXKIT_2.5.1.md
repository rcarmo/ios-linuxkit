# ios-linuxkit 2.5.1 - 4 October 2026

## Fixes

- Fixed `EPERM` errors when Pi cleans up temporary files after installing
  fd and ripgrep.
- Corrected the Xcode scheme's application name.

## Known limitations

The accelerated iPhone build still has a reported crash under investigation.
This release does not claim to fix it. Existing Linux files and installed
packages are preserved when updating the app.
