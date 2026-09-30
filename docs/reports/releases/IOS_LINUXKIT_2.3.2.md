# ios-linuxkit 2.3.2 / build 815

Source patch release for opt-in route-netlink interface discovery. Based on
2.3.1; no Alpine pin, native/AOT ABI, default backend or app-scheme enablement
changes. This is not a signed iOS binary or App Store upload.

## Changes

Selectively ports the interface-discovery fix submitted as
[OpenMinis PR #46](https://github.com/OpenMinis/ish-arm64/pull/46), rather than
merging the divergent OpenMinis branch:

- bounded `RTM_GETLINK` and `RTM_GETADDR` host snapshots;
- Linux message/source-address ABI, port IDs, scatter/gather, receive flags,
  readiness and explicit malformed/unsupported-request errors;
- nonblocking reply enqueue to avoid self-deadlock on a saturated receive queue;
- opt-in host-backed `SO_BINDTODEVICE`, translating to Darwin interface-index
  options instead of ignoring the request;
- native/guest protocol regressions, encoder sanitizer tests and a real Go
  interface-discovery probe.

`ISH_NETLINK_STUB=1` remains off by default. Requests/replies are bounded at
4096 bytes; oversized snapshots fail explicitly. Route dumps are still empty;
multicast notifications, individual queries and host network mutations are not
implemented. The [guide](../../NETLINK_TAILSCALE.md) gives commands and limits.

## Local validation

Evidence: `/workspace/artifacts/ish-2.3.2/20260930/`.

Host: Orange Pi 6 Plus, CIX P1 ARM64, 12 CPU cores, approximately 14 GiB usable
RAM, NVMe storage, Debian Trixie, kernel `6.6.89-cix`, Clang 19.1.7, 4 KiB host
pages. Guest: independently restored frozen Alpine 3.24.2 fakefs roots.

The build matrix covers gadget and no-emission linked-AOT configurations, each
in release and debug. The retained four image assemblies from the validated
2.3.1 set are reused; this patch does not change translations or their ABI.
All four configurations pass:

- 211 netlink checks and interface-binding tests;
- upstream correctness (52 syscalls, 512 FMOV values, actual anonymous-accounting
  failures/rollback, timers/signals, task lifetime, sleep, OOM and recovery);
- regular-file polling, FCVT vector, precise load fault-PC, proc-mem seek,
  lseek-width and poke/signal stress;
- 14/14 internal-continuation fixtures;
- full procfs stress: native control plus two guest runs, each 25 seconds with
  16 forkers and 6 proc/ps readers, with positive progress and no watchdog failure.

Both AOT configurations pass four-image acceptance, zero-emission and runtime-off
parity. Hardware breakpoints hit musl, BusyBox, Python and zlib native PCs in the
debug build, with no emitter region and normal exit. Image assemblies are
byte-identical to the retained 2.3.1 set. The native O0/O2 harness passes 100 actual
restart faults and 11,296 integer/memory oracle comparisons at each optimisation
level. AOT generator (3 tests/24 assertions), artifact kit (10/41) and doc-style
(2/6) tests pass, as do local Markdown links and the 14 maintained-document style
scan.

A first PC-proof invocation had an insufficient 50-second outer timeout; the
complete rerun passed all four images and is the counted evidence. Initial
validation invocations with incorrect build/script paths or no guest compiler
were corrected; only the complete four-lane run is counted. No failed check is
reported as a pass.

- Encoder: 10,000 malformed-header/capacity cases under GCC ASan+UBSan, plus
  overflow rejection and IPv4/IPv6 family filtering, pass.
- Netlink: 211 message/queue/readiness checks pass in the local guest. The
  inherited native Linux oracle passes 221 checks. Binding tests cover missing
  and known interfaces and preserve host permission checks on unbind.
- Disabled compatibility switch still returns `EAFNOSUPPORT`.

### Official Tailscale

The unmodified official Tailscale 1.102.4 ARM64 archive matches the publisher's
SHA-256:

```text
9dd1e6a592a014bbaea0103167ffe299adeda4ba14e078ce9c2895364f6c4c3f
```

An isolated fakefs daemon uses userspace networking, in-memory state, its own
Unix socket, ephemeral UDP/SOCKS ports and disabled DNS/route acceptance. The
fork's actual Linux CLI builds without the upstream Darwin test adapters.

The daemon reports nonempty IPv4/IPv6 link state, answers `tailscale status`
with `Logged out.`, contacts the official control server and receives a
registration response with `machineAuthorized=false; authURL=true`. The URL was
not followed; the login timeout is expected. The daemon was terminated cleanly.
Host Tailscale configuration was not changed.

This proves interface discovery and control-plane startup, **not authenticated
VPN traffic**. Darwin/Xcode/iOS builds, app permissions, handover and suspension
remain untested. The source list includes the encoder; the app does not enable
the compatibility switch.

## Next work, kept out of this release

Revisit OpenMinis's footprint governor and 16 KiB page clustering against our
logical committed-page ledger. Preserve allocation ownership and fork/CoW
accounting; define Linux policy tests and Apple-device tests separately. Wider
independent AOT workloads should then measure memory as well as speed, before
app AOT enablement or cooperative offload cancellation is promoted.
