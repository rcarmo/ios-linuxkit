# ios-linuxkit 2.5.2 - 5 October 2026

## Changes

- Accelerated builds now include Alpine Go 1.26.8, with the compiler,
  formatter, assembler, linker and vet tools.
- Improved first-time Go compilation performance.
- Fixed a crash during process shutdown.

## Known limitations

The accelerated build remains experimental. First-time Go builds can still
take several minutes. The reported Bun/Pi iPhone crash remains under
investigation.

Updates preserve existing Linux files and installed packages. Existing
filesystems do not gain Go automatically; use the bundled filesystem or install
Go separately. CGo requires additional Alpine packages.
