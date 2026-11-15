# ios-linuxkit 2.6.0 — 15 November 2026

## Changes

- Updated the experimental accelerated filesystem to Bun 1.4.4.
- Reduced native fault-checkpoint work and shared common translated-code paths
  to reduce AOT image size.
- Added module-relative execution profiling and tools for selecting hot code
  for smaller AOT images.
- Expanded ARM64 vector instruction coverage and added software SHA512 fallback
  for hosts without the required hardware instructions.
- Improved translated-memory cache invalidation during remapping and process
  cleanup.
- Fixed descriptor ownership, socketpair resource-limit failures, concurrent
  polling cleanup, finite poll deadlines and edge-triggered event rearming.
- Expanded instruction, fault-recovery, ownership and concurrency regression
  coverage.

## Compatibility

Ordinary application builds still use the gadget interpreter without runtime
code generation. Accelerated builds remain optional, and unmatched code falls
back to the interpreter.

Translated-code conventions have changed. Generate fresh AOT images against the
matching runtime and guest files; older images are rejected rather than reused.
Existing Linux files and installed packages are preserved. Updating the app
does not automatically replace Bun or Go inside an existing filesystem.

## Known limitations

This is a source release, not a prebuilt signed application. Apple application
and physical-device validation have not been performed for this release.

The accelerated build remains experimental. Linux regression and no-emitter
checks do not establish iPhone/iPad stability or application performance. The
reported Bun/Pi iPhone crash and accelerated Mac Go compilation crashes remain
under investigation. First-time Go builds can still take several minutes;
arbitrary newly compiled programs use the interpreter unless matching
translations exist.
