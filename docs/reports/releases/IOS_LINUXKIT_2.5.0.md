# ios-linuxkit 2.5.0 - 4 October 2026

## What's new

- xterm is now the default terminal. Ghostty remains available as an alternative.
- Select terminal text by touch and copy it to the iOS clipboard.
- The accelerated iPhone test build includes Bun 1.4.2 for running Pi.

## Fixes

- Corrected terminal scrolling beyond its bounds and content being obscured by
  the keyboard.
- Fixed runtime errors that could crash Bun/Pi or cause secure downloads to fail
  with `BAD_DECRYPT` or misleading certificate errors.
- Fixed `EPERM` errors when Pi removes temporary files after installing
  fd and ripgrep.

## Known limitations

The accelerated iPhone build is still being tested. Stability and performance
on physical devices are not yet fully verified. Existing Linux files and
installed packages are preserved when updating the app.
