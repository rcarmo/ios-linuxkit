# Opt-in route-netlink and Tailscale

The Linux CLI can report host interfaces through read-only route-netlink
snapshots when started with `ISH_NETLINK_STUB=1`. The switch is off by default.
This is a selective port of [OpenMinis PR #46](https://github.com/OpenMinis/ish-arm64/pull/46),
not a merge of OpenMinis's kernel or app changes.

## Behaviour

`RTM_GETLINK` and `RTM_GETADDR` dumps report host interface indices, names,
flags, MTUs, hardware addresses where available, IPv4/IPv6 addresses and prefix
lengths. The replies use Linux message layouts and kernel sender addresses, with
per-socket port IDs and the request sequence. Darwin flags and address families
are translated; embedded link-local IPv6 scope bytes are cleared.

The socketpair receive queue supplies poll/epoll readiness. Send/receive,
scatter/gather, `read`/`write`, `MSG_PEEK`, `MSG_TRUNC` and nonblocking operations
use the same compatibility path. Reply enqueue never waits for the sending guest
to drain its own queue; a full queue returns `EAGAIN`.

Both requests and complete replies are bounded at **4096 bytes**. Oversized
requests return `EMSGSIZE`; a snapshot that will not fit becomes
`NLMSG_ERROR(-ENOBUFS)`, never a partial successful dump. Unsupported operations
receive errors. Batched requests and individual interface queries are unsupported.

**Routes remain empty and multicast notifications are not generated.** Group
masks are recorded, but no live network-change events arrive. This is not a full
Linux netlink implementation. Wi-Fi/cellular handover and suspension/resume need
separate testing. Host routes, addresses and interfaces cannot be changed through
this path; the reported interfaces are not an isolated guest network namespace.

When the switch is enabled, guest `SO_BINDTODEVICE` is also supported for
Tailscale's outbound networking. Linux uses the host option; Darwin uses
`IP_BOUND_IF` or `IPV6_BOUND_IF`. Invalid interfaces fail, and host permission
checks still apply. Unlike the upstream experiment, the option is **not silently
ignored**. With the switch off, existing socket-option behaviour is unchanged.

## Start an isolated userspace daemon

Use a disposable Alpine ARM64 fakefs. Do not use the host daemon or a running
app filesystem. Import a guest tar containing the official ARM64 `tailscale` and
`tailscaled` binaries with `tools/fakefsify`; copying new files into fakefs `data/`
does not create the required metadata records. A non-root realfs cannot create
the guest socket inode needed by the local API.

Build the normal gadget CLI and start the guest with the switch:

```sh
make build-arm64-linux CC=clang
ISH_NETLINK_STUB=1 build-arm64-linux/ish -f /absolute/test-fakefs /bin/sh
```

Inside that guest:

```sh
# No TUN, fixed UDP/SOCKS port, persistent node state or host route changes.
tailscaled --tun=userspace-networking --state=mem: \
  --socket=/tmp/ish-netlink-test.sock --port=0 \
  --socks5-server=127.0.0.1:0 >/tmp/ish-netlink-test.log 2>&1 &
daemon=$!
trap 'kill "$daemon" 2>/dev/null; wait "$daemon" 2>/dev/null' EXIT INT TERM
sleep 8
tailscale --socket=/tmp/ish-netlink-test.sock status
# Request an authentication URL without changing guest DNS or accepting routes.
# Unless someone authorises this node in a browser, the timeout is expected.
tailscale --socket=/tmp/ish-netlink-test.sock up --accept-dns=false \
  --accept-routes=false --hostname=ish-netlink-smoke --timeout=35s
grep 'authURL=true' /tmp/ish-netlink-test.log
```

`status` should say `Logged out.`. A successful control-plane probe has nonempty
`link state` and a registration response with `authURL=true`. Inspect URLs locally;
do not publish them. Obtaining a URL is **not account authentication or proof of
working tunnel traffic**.

## Tests

The C socket test requires Linux UAPI headers and a static ARM64 Linux compiler.
Use a disposable realfs for this test; the runner checks the output marker, not
just the CLI's exit status. An up non-loopback host interface is required.

```sh
make test-arm64-netlink CC=clang CC_GUEST=aarch64-linux-musl-gcc \
  NETLINK_REALFS=/absolute/guest-root

# Standalone Linux encoder under sanitizers:
gcc -I. -std=gnu11 -O1 -g -fsanitize=address,undefined \
  tests/regress/netlink-snapshot.c fs/netlink.c -o /tmp/netlink-snapshot
/tmp/netlink-snapshot

# Real Go interface/address discovery; stage into a disposable realfs:
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build \
  -o /absolute/guest-root/tmp/interfaces tests/regress/netlink_interfaces.go
ISH_NETLINK_STUB=1 build-arm64-linux/ish -r /absolute/guest-root /tmp/interfaces
```

Unlike the upstream test worktree, our CLI builds directly on Linux: no generated
Darwin adapters are needed. `CPPFLAGS` can provide Linux UAPI include paths for a
native `musl-gcc` installation. On the Debian AArch64 validation host, the
wrapper's default include search omits Linux UAPI headers. Use them after musl's
own headers, so glibc headers do not replace musl's definitions:

```sh
CPPFLAGS='-idirafter /usr/include -idirafter /usr/include/aarch64-linux-gnu' \
  make test-arm64-netlink CC=clang CC_GUEST=aarch64-linux-musl-gcc \
  NETLINK_REALFS=/absolute/guest-root
```

The opt-in netlink backend is independent of [native offload](NATIVE_OFFLOAD.md).
Enabling interface snapshots does not enable cooperative handlers, admit arbitrary
socket types as handler stdio or bypass the offload stream limits. Source 2.4.1
retains the same default-off switch and packaged Alpine pin.

## Evidence and remaining gates

The [2.3.2 release report](reports/releases/IOS_LINUXKIT_2.3.2.md) records the
local results. The official Tailscale 1.102.4 ARM64 archive was checked against
the publisher's SHA-256:

```text
9dd1e6a592a014bbaea0103167ffe299adeda4ba14e078ce9c2895364f6c4c3f
```

An isolated daemon reported real interfaces, answered the local API and received
an authentication URL from the official control server. No URL was followed;
the login timeout was expected. No account, authenticated traffic, TUN support,
Darwin/Xcode/iOS build or device behaviour has been validated. The Xcode source
list includes the encoder, but no iOS scheme enables this runtime switch.
