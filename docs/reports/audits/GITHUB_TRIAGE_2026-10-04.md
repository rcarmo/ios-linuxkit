# Open issue and PR triage — 4 October 2026

Reviewed the three open submissions against master `66f19003` (2.4.1/build818).
No release/version change, native/AOT activation or Apple validation is implied.

## PR #2: ARM Mac compilation

[PR #2](https://github.com/rcarmo/ios-linuxkit/pull/2) by joshrad-dev adds
`_XOPEN_SOURCE=700` and `_DARWIN_C_SOURCE` only for Darwin/AArch64 Meson hosts.
Those flags were absent on current master. Applied that narrowly scoped change
with contributor attribution; Linux and Darwin/x86_64 do not receive the flags.

`make test-darwin-feature-macros` extracts the actual project preamble, configures
three Meson cross descriptions and compiles local probes. It checks both macro
presence on Darwin/AArch64 and absence on the other combinations. The compiler
is local Linux Clang, not an Apple SDK compiler: no claim that Xcode/macOS/iOS
compilation is now verified. Existing gadget Xcode bridge tests still pass.

Disposition: close after incorporating the change. No broader Mac support or
follow-up commitment.

## PR #3: fakesigned IPA target

[PR #3](https://github.com/rcarmo/ios-linuxkit/pull/3) proposes `make ipa`, separate
app/extension builds, hand-written entitlement plists and `ldid` fakesigning.
Fakesigning alone does not supply device provisioning or app-group authorisation
on ordinary non-jailbroken iOS. The proposal provides no verified installation
method or exact device/build evidence to establish that this is a supported path.
Independent builds, hard-coded product/executable names and generated entitlements
also need checking against the evaluated Xcode settings and complete nested bundle.
No Apple tooling/signing/device environment is available on this host.

The current main `PRODUCT_NAME` is `LinuxKit` via `app/App.xcconfig`; historical
scheme metadata still says `iSH ARM64.app`. Corrected the maintained guide's
product table and told packaging callers to use evaluated build settings rather
than the historical label. The PR's `LinuxKit` default is not itself a proven
product-name bug. No IPA target or fake signing procedure was imported.

Disposition: close as unsupported/unvalidated as submitted, politely recognising
the contribution. No promise of IPA distribution or sideloading support.

## Issue #6: successful command with empty captured output

[Issue #6](https://github.com/rcarmo/ios-linuxkit/issues/6) shows `ls /` exiting0
and zero-byte output through `ISHShellExecutor`, `LinuxShellRuntime` and
`TerminalView`. None of those wrapper classes is in this repository. The report
supplies no source revision, reference-scheme reproduction or descriptor/capture
implementation. It does not establish where output was lost.

Linux gadget release/debug builds each passed a bounded smoke run exercising
stdout, stderr, `ls /` and a shell pipe. Each capture contains92stdout bytes and
10stderr bytes. Both builds also pass the actual-source upstream regression gate.
This does not reproduce or disprove an Apple/external-wrapper bug.

Disposition: explain the evidence gap and close the non-reproducible custom
integration report. A minimal reproduction on the reference app, or the missing
wrapper/descriptor source and revision/rootfs/device details, is needed for an
in-scope report; no promised investigation or fix. Do not suggest delayed reads
or a descriptor workaround as a proven repair.

## Evidence and limits

Orange Pi6Plus, CIX P1, eight Cortex-A720/four Cortex-A520 cores,16GB-class RAM,
NVMe, host-native AArch64 Debian Trixie with4KiB pages. Runtime/profile unchanged:
`/workspace` resolves to `/home/agent/workspace`; `piclaw.service` remains active.

Evidence: `/workspace/artifacts/ish-triage-20261004/`:
- gadget release/debug incremental configuration/build and upstream gates pass;
- both stdout/stderr/pipe smoke captures pass;
- three Meson feature-scope probes, three Xcode bridge tests and docs/style tests
  pass; local link and whitespace checks pass;
- no Xcode, Apple SDK, signing, archive, fakesigned IPA or device gate run.

PR #3 read-only delegated review timed out after90s and contributes no evidence.
Source review and dispositions were completed locally. Replies thank contributors,
state present evidence/limits and make no feature, merge, support or timeline
commitment. Historical release tags and source evidence are unchanged.
