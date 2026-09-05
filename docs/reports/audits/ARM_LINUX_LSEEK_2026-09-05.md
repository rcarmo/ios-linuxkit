# ARM Linux full-width seek regression — 5 September 2026

## Baseline and execution boundary

Baseline `df8903dd0770dc9a6ad3e270f00a104a53eaee11` was clean. Host:
Orange Pi 6 Plus, CIX P1 (4 Cortex-A520 + 8 Cortex-A720, 12 cores),
16 GB class RAM (14 GiB visible), NVMe/ext4 workspace, Debian Trixie,
AArch64 kernel `6.6.89-cix #15`, Clang 19.1.7, Meson 1.7.0, Ninja 1.12.1.
Host-native process, not a container. Default Meson release/debug settings;
no diagnostic variables or native-offload mappings in correctness runs.

Guest: existing Debian 13.5 fakefs, copied before package-suite experiments.
`ish -f ROOTFS /usr/bin/python3 ...` traverses the Asbestos interpreter,
ARM64 syscall table and fakefs; this is not native command offload.
The native and guest `/usr/bin/python3.13` executables are identical:
SHA-256 `97ffc360c23df9365e9dafcbae3ef55d044937119284cefca378828dd12a3196`.
Guest libc/filesystem configuration differs from the host; the static fixture
below removes the libc difference for the raw-return comparison.

## Reproduction and first divergence

```sh
build-arm64-linux/ish -f debian-arm64-fakefs /usr/bin/python3 -c \
 'import tempfile; f=tempfile.TemporaryFile(); print(f.seek(0xffffffff))'
```

Expected/native: `4294967295`, exit 0. Baseline guest: `PermissionError`, exit 1.
`ISH_SYSTRACE=1` reports syscall 62 returning `0xffffffff` **before** guest
register writeback. `sys_lseek64` and the underlying host seek have succeeded.
`handle_interrupt()` subsequently recognises low-32-bit errno patterns and
sign-extends this valid offset to `0xffffffffffffffff` in guest x0. Libc then
interprets it as `-EPERM`. This is the first incorrect transition, not a
filesystem permission failure. The entire positive range `0xfffff001` through
`0xffffffff` is affected, not just its final byte.

## Fix and evidence

Bypass the legacy 32-bit errno repair for syscall 62, whose implementation
already returns full-width offsets and full-width signed errors. Leave other
syscall handling and filesystem semantics unchanged. The correction is common
to Linux and Darwin hosts; it introduces no executable memory or host API.

`tests/arm64/fs/lseek-width.c` uses raw AArch64 `svc` and libc calls. It checks
SET/CUR/END, both sides of the false-errno interval, offsets above 4 GiB up to
1 TiB, invalid origin/fd, pipe errors and unchanged position after rejection.
No large file is written. Static binary SHA-256 with Clang 19.1.7 `-O2 -static`:
`e7d8bc396afbac8b225ed66eddc3218f059cc4cfc33274af27066cb0106de693`.
The identical bytes pass natively, fail on the saved baseline with
`raw SET: got=0xfffffffffffff001 expected=0xfffff001`, and pass on release/debug
candidates. The real Python integration also fails on the baseline and passes
on both candidates (`lseek-width-gate-ok`).

An initial INT64_MAX regular-file case was removed: native `/tmp` is tmpfs,
whereas fakefs backing is ext4. Native ext4 also rejects this offset with EINVAL;
it is not a return-width failure. The proc-mem gate retains signed-limit tests.

Verified on ARM Linux:

```sh
CC=clang make build-arm64-linux-all
CC=clang make test-arm64-lseek-width test-arm64-proc-mem-seek test-arm64-fcvt-vector
ISH_BIN="$PWD/build-arm64-linux-debug/ish" tests/arm64/fs/run-lseek-width.sh
```

Baseline release/debug FCVT and proc-mem gates passed, as do candidate gates.
The supported broad runtime suite passes four base rows then fails before C
coverage: `detect_platform` removes its status marker but retains the preceding
blank line, so `tail -1` returns empty and package selection uses Alpine's
`build-base` on Debian. This pre-existing harness failure is not a pass for
unrun rows. The older report's claim that build-essential was installed was
not established by its empty `dpkg-query` output and is not relied upon here.

## iOS boundary

Not cross-built for iOS; no Xcode toolchain here. Requires device validation:
run the same sparse-file Python integration through the packaged guest, confirm
invalid-fd/pipe errno behaviour, then check terminal responsiveness and file
cleanup under background/resume and memory pressure. Signing, sandbox storage
limits and App Store acceptance are not established by these Linux results.
No version bump, release, tag or push is part of this work.
