# Documentation review — 30 September 2026

Thirteen maintained documents were reviewed for source accuracy, runnable
procedures and plain technical prose while preparing 2.3.1/build814. The review
used the packaged `technical-docs` skill: factual openings, British English,
concrete language, exact commands and removal of rhetorical filler. Its
anti-trope rules also cover manufactured contrasts, sincerity claims, vague
status language, repeated caveats and commentary about what prose implies.

## Coverage and changes

| Document | Review result |
|---|---|
| `README.md` | Updated version, explicit Clang builds, task links and current AOT behaviour; moved repeated release history to the report index. |
| `SECURITY.md` | Retained the outer-sandbox warning and contacts; added artifact trust/checksum limits. |
| `docs/README.md` | Split navigation between Linux AOT, artifact operations and iOS integration. |
| `docs/ARCHITECTURE.md` | Corrected gadget-only description; added optional native/AOT execution, image matching, ownership/invalidation and checkpoint recovery. |
| `docs/LINUX_DEVELOPMENT.md` | Corrected stale Alpine3.24.0 sentence, explicit Clang selection and quiescent fakefs export requirements. |
| `docs/IOS_APPLICATION.md` | Updated version and current app state; consolidated repeated release/device caveats into device checks and AOT implementation links. |
| `docs/NATIVE_AOT_BUILD_PLAN.md` | Replaced the completed-project checklist with a Linux runbook: dependencies, guest preparation, recording, linking, release/debug validation, rollback, timing and troubleshooting. |
| `docs/NATIVE_AOT_ARTIFACT_KIT.md` | Defined every input/output, clean-tree requirements, path rules, failure retention, independent guest restoration, build staging, validation and publication. |
| `docs/NATIVE_AOT_IOS.md` | Added target contract fields and their sources, Mach-O generation, missing app integration, signing and physical-device acceptance. |
| `docs/VALIDATION.md` | Removed the obsolete statement that native/AOT was not imported; separated CLI synthetic contexts from actual native restart tests and condensed historical evidence. |
| `docs/LIMITATIONS.md` | Added image identity, binary/RSS cost, Python regression and Apple integration constraints. |
| `docs/CONTRIBUTING.md` | Added AOT guide locations and durable writing rules; retained test/failure requirements. |
| `docs/RELEASES.md` | Updated 2.3.1/build814 and tag commands; retained source/signing distinction. |

`docs/reports/README.md` was updated as an index. Existing dated reports, legacy
translations, third-party terminal documents and generated Fastlane text were
left intact. They were checked as reference/link targets where used; this review
did not reassess every historical result or rewrite vendor prose.

## Source discrepancies repaired

- **Alpine package inventory:** `targeted.ts` used `apk info -v NAME`, which APK3
  prints as descriptions. It now uses `apk list --installed` so recording logs
  contain versions. The real restored guest reports the expected four packages;
  the recorder test checks the command passed to its guest.
- **Refused publication:** `publish` used to remove build caches and `pending.json`
  before discovering that its destination existed. It now refuses that output
  first. A regression checks all three build directories and pending metadata
  survive the failed call.
- **Source provenance:** seed/build publication requires a clean Git checkout;
  offline source-archive rebuilding uses explicit Meson/Make. The guides now
  distinguish these procedures and keep output directories outside the checkout.
- **Apple reuse:** the wrapper checks a thin ARM64 Mach-O header, defined symbols
  and a binary-bound observed contract. It does not inspect SDK load commands,
  signing or entitlement validity. The new guide assigns those checks to the
  actual Apple build and device workflow.
- **Acceptance:** the publisher checks `status: pass` in an operator-written
  record. The guide supplies the full test sequence and states that the operator
  must inspect evidence before creating that record.

## Writing checks

`scripts/check-docs-style.ts` scans maintained Markdown prose for selected
patterns. `make check-docs-style` runs it; `make test-docs-style` checks rule
behaviour. It skips fenced code, quotes and tables, and excludes dated/vendor
files by default. Review tables and multi-line constructions manually: a line
scanner cannot assess all rhetorical patterns or technical meaning.

The checker complements `make check-docs`, which verifies local link targets.
The link checker does not validate heading fragments or fetch external pages.
No result from either tool substitutes for reading the referenced code.

## Verification

- 13 maintained documents pass the selected-pattern scan; style tests pass
  2 cases /6 assertions.
- All 52 shell blocks parse with `bash -n`; documented test/record Make targets
  exist. Commands needing sudo, moving repositories, credentials or Apple SDKs
  were checked against source and prerequisites without running those external
  operations during the prose pass.
- Kit tests pass 10 cases /41 assertions; generator tests pass 3 /24. Atomic
  rootfs download tests pass.
- Fresh gadget and AOT release/debug builds pass upstream, focused, 14/14
  continuation and full procfs stress on independently restored Alpine guests.
- Four images regenerate byte-identically from the retained seed. Linked AOT
  release/debug gates pass; all four image PCs execute with emitter region NULL.
- Native O0/O2 fault/restart and oracle tests pass. The [release record](../releases/IOS_LINUXKIT_2.3.1.md)
  gives denominators, paths and platform limits.

A delegated read-only command review timed out after 120s and returned no usable
review. The source comparison and tests above were performed locally. An outer
600s shell limit interrupted the first image-PC sequence after runtime lanes
finished; the independent rerun completed all four image checks. The interrupted
sequence is retained and excluded from passing evidence.
