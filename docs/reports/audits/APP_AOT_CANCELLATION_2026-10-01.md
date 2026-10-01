# App AOT boundary and cooperative cancellation — 1 October 2026

## Scope

Changes remain on `prototype/memory-pressure-policy`, based on `38114081`.
Master stays at 2.3.2/build815 (`ec2d0846`). No release, memory-policy activation,
app AOT scheme, native emitter or real FFmpeg integration is included.

Host: Orange Pi 6 Plus, CIX P1 (CD8180/CD8160), eight Cortex-A720 and four
Cortex-A520 cores, 16 GB-class RAM (about 14 GiB visible), NVMe/ext4, native
Debian Trixie AArch64, Linux 4 KiB pages, Clang 19, Bun 1.4.2. Xcode, an Apple
SDK and a physical Apple test device are unavailable. Linux adapters cannot
establish Apple ABI, signing, 16 KiB protection, footprint or jetsam behaviour.

## App AOT audit and guard

The existing app starts guest execution from `app/AppDelegate.m`, while native
precise fault recovery is in the CLI's `main.c` crash handler. The app has no
corresponding native fault-context integration. `cli_aot` adds assembly to the
CLI, not app build membership. Existing Xcode targets also lack matched
`ISH_JIT` shared-structure definitions, observed Apple ABI contracts and image
constructor retention evidence. Linking Linux images into the app is not safe.

The held-out [workload evaluation](AOT_HELDOUT_2026-10-01.md) found two Python
regressions and more than twice their peak RSS. It does not justify app enablement.

`app/xcode-meson.sh` now sets `jit=false`, `jit_emit=false` and empty `cli_aot`
explicitly for both fresh and reused Meson directories. Reconfiguration changes
all three together: setting only `jit=false` with stale AOT images would fail
Meson's dependency check. Configuration errors propagate to Xcode. There is no
environment variable bypass or new AOT scheme; a future isolated scheme needs a
separate validated bridge. Normal app guest files and user data are unchanged.

`make test-xcode-gadget-guard` executes the real bridge with a fake Meson CLI:
reused native options, fresh setup, and configuration failure. Three tests and
10 assertions pass. A separate actual Linux Meson setup with native emission
and one AOT image reconfigures atomically to `jit=false`, `jit_emit=false`,
`cli_aot=[]`. This checks option handling, not Apple compilation.

The [iOS handoff guide](../../NATIVE_AOT_IOS.md) remains the implementation and
physical-device acceptance boundary. No accelerated archive/device proof exists.

## Opt-in handler contract

`native_offload_add_cooperative_handler()` is separate from legacy handler
registration. Standard Linux builds retain unsupported registration stubs;
Apple builds only use this path when a handler is explicitly registered.
The only migrated caller is `FakeFFmpeg.m`, gated by `ISH_FFMPEG_TEST`.
Registry setup must finish before guest execution; dynamic registration is not
thread-safe. Legacy handlers and macOS spawned-process signal forwarding retain
their existing contracts.

The caller owns a stack `native_cancel` token. `native_cancel_begin()` publishes
it under `sighand->lock`; eligible already-pending signals are mirrored. Requests
use that same lock, with an additional check inside ordinary signal queuing.
The latter closes the interval between an unsuccessful early request and token
publication. Requests never retain a pointer after releasing the lock.

Handlers poll `native_cancel_signal()` with acquire semantics. Requests are
sticky: the first eligible signal wins, except SIGKILL overrides it. Eligible
signals are SIGINT, SIGTERM, SIGHUP and SIGQUIT when unblocked and not ignored;
SIGKILL is unconditional. Other signals follow the ordinary guest queue.
Accepted requests do not send a host kill, pthread cancellation or host signal.
They do not asynchronously interrupt blocking I/O.

The token stays published while handler resources close, output forwarding
threads drain/join, fd references release, CWD restores and files register.
`native_cancel_finish()` withdraws it under the signal lock and returns the
winning signal. Guest termination uses signal wait status, not shell-style
`128+signal`. Without cancellation the return value is masked to an 8-bit exit
code, avoiding undefined negative left shifts. A request after withdrawal takes
the ordinary guest signal path; it cannot access the expired stack token.

Task creation clears the token and native proxy state rather than copying an
execution's ownership into a forked task. Cooperative exec refuses `_EBUSY` if
the guest thread group has siblings or is already exiting, before applying exec
semantics. Existing guest exec does not retire siblings; its exit-group timeout
can detach a blocked sibling's sighand/resources. That unsupported combination
must not enter the token lifecycle. Legacy paths are not broadened by this guard.

### Handler obligations and limitations

A cooperative handler must poll at bounded intervals, manage its own I/O
deadlines, close descriptors and join every worker that can access the token
before returning. Cancellation latency includes all cleanup and output drain.
SIGKILL is a request here, not permission to asynchronously destroy a host thread.
Guest stop/continue and other signal actions are not newly implemented for
native execution. This API is not a claim of full Linux process semantics.

Existing portable offload scaffolding has unchecked allocation/pipe-thread
failure paths, process-wide CWD changes, no general interruption of blocked
reads/writes and possible blocking guest output. These were not repaired in this
bounded tranche. Real handlers need failure injection, bounded I/O/output and
concurrency/CWD isolation before production admission. Do not ship real FFmpeg
on these tests alone.

FakeFFmpeg polls at 100 ms simulated-progress intervals and between file-copy
operations. It closes resources when asked; a cancellation during copy can leave
a partial output. It does not pretend to transcode or delete outputs transactionally.
File I/O can still block; the 100 ms sleep interval is not an end-to-end deadline.

## Verification

Evidence: `/workspace/artifacts/ish-app-cancellation-20261001/`.
Final four build lanes: gadget release/debug and no-emitter AOT release/debug.
All builds and `*-upstream-verified.rc` are 0:

- actual-kernel token lifecycle: 1,000 signal/completion races per lane, nested
  begin rejection, pending-before-start, blocked/ignored handling, SIGKILL
  priority, reset/fork isolation and a linker-controlled signal/publication race;
- real portable handler source compiled under the explicit Linux fixture macro:
  normal return, SIGTERM, SIGKILL priority, pending cancellation without invocation,
  output EOF/fd-reference cleanup, cancellation during drain, sibling refusal and
  already-exiting refusal — eight modes per lane;
- actual test-only FakeFFmpeg source: exact normal 4 KiB copy, cancellation during
  progress before output creation, and cancellation between copy read and poll;
- upstream compatibility/accounting/recovery gates including the existing
  46-case synthetic clustering suite in every lane.

`*-proc-final.rc` are 0: native control plus two guest runs per lane, each 25 s,
16 forkers and six readers. Both `*-linked-verified.rc` are 0: four frozen AOT
images used, runtime-off output parity and zero emission. These checks preserve
the earlier ABI/image contract; adding the task token did not invalidate it.

All `*-continuation-checked.rc` are 0 with 14/14 cases per lane. The earlier
`*-continuation-verified.rc` are 1 because the minimal seed lacks guest GCC;
no fixture runtime had executed. The corrected invocation supplies `HOST_CC=clang`
and retains reports separately. Failed harness attempts remain in evidence.

The earliest token fixture also needed corrections: `become_new_init_child()`
returns 0 on success, and a synthetic task must set its actual pthread before
ordinary cross-thread signal delivery. Those fixture failures are retained, not
attributed to the candidate. Output-drain assertions now allow cancellation to
arrive immediately after writing, rather than assuming the forwarder has not run.

Generator/kit tests pass 13 tests and 65 assertions. Documentation link/style
checks and `git diff --check` pass. A delegated race review timed out after 90 s;
there is no independent-review claim.

## Reproduction and remaining gates

```sh
make test-xcode-gadget-guard test-aot-generator test-aot-kit
CC=clang EVIDENCE_DIR=/absolute/new-upstream make test-arm64-upstream \
  RELEASE_BUILD_DIR=build-memory-policy-release DEBIAN_ROOTFS_DIR=/absolute/test-root
CC=clang HOST_CC=clang make test-arm64-internal-continue-fixtures \
  RELEASE_BUILD_DIR=build-memory-policy-release ROOTFS_DIR=/absolute/test-root \
  REPORT_DIR=/absolute/new-continuation
```

Use matching definitions/libraries for each lane; the upstream runner derives
`ISH_JIT` from compile commands. The Linux handler macro is fixture-only and must
not be set on normal builds.

Keep app AOT and memory policy disabled. Future work needs actual Apple
fault-path/shared-layout integration, target-bound Mach-O generation, final
constructor/import/mapping inspection, signing and physical-device stress.
Actual native handlers also need the blocking-I/O/output/failure/concurrency
gates above. Neither missing platform evidence nor those broader handler gates
are replaced by this host proof.
