# ios-linuxkit 2.6.1 — 17 November 2026

## Changes

- Reduced experimental AOT memory use with training-profile-selected images,
  retaining interpreter fallback for code omitted from an image.
- Improved ARM64 block-hash distribution without increasing cache capacity.
- Compacted an existing ARM64 instruction fusion and updated AOT recording,
  selection and profiling workflows.
- Added opt-in Linux memory-pressure sampling and conservative memory admission,
  with fixes for protection changes, allocation rollback and backing accounting.
  Sampling remains disabled by default.
- Reduced memory-policy bookkeeping overhead and repeated publication work;
  shared anonymous backing is accounted separately from logical aliases.
- Added phase-aligned AOT diagnostics and a read-only monotonic-clock endpoint
  for correlating runtime measurements with host profiling.
- Expanded memory-failure, concurrency, fault-recovery and native-execution
  regression coverage.

## Compatibility

Ordinary builds still use the gadget interpreter without runtime code
generation. Accelerated builds remain optional. Existing Linux files and
installed packages are preserved; updating the app does not replace packages
inside an existing filesystem.

The translated-code ABI is unchanged from 2.6.0. Existing compatible images can
fall back to the interpreter for changed instruction-stream keys. Generate
fresh recordings against the matching runtime and guest files for current
coverage; do not relabel older recordings or bypass compatibility checks.

## Known limitations

This is a source release, not a prebuilt signed application. Apple SDK builds,
signing and physical-device validation have not been performed for this
release. Linux tests do not establish real 16KiB-host, lifecycle, memory-pressure
or jetsam behaviour on iPhone or iPad.

Selected AOT images use less memory than full images, but still exceed gadget
execution in the measured workloads. Timing gains are mixed, and no AOT
break-even has been demonstrated through the measured work count of 64.

The accelerated build remains experimental. The reported Bun/Pi iPhone crash
and accelerated Mac Go compilation crashes remain under investigation. Newly
compiled or otherwise unmatched code uses the interpreter unless matching
translations exist. Linux pressure sampling is conservative observation, not
an allocation reservation or a guarantee against host out-of-memory events.
