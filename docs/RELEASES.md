# Versioning and releases

The ARM64 application uses semantic release versions, monotonically increasing Apple build numbers and matching annotated Git tags. The current source version is **2.1.3**, Apple build **809**, tagged as `v2.1.3` after validation. See the [2.1.3 source release record](reports/releases/IOS_LINUXKIT_2.1.3.md).

## Version sources

| Value | Source | Used by |
|---|---|---|
| ARM64 release version | `MARKETING_VERSION` in `app/AppARM64.xcconfig` | `CFBundleShortVersionString` in both ARM64 application schemes. |
| Apple build number | `CURRENT_PROJECT_VERSION` in the four project build configurations in `iSH.xcodeproj/project.pbxproj` | `CFBundleVersion` in the same products. |
| Source tag | annotated Git tag `v<release version>` | Revision lookup and release provenance. |

`app/Project.xcconfig` retains the inherited non-ARM64 version. `AppARM64.xcconfig` overrides it for the main ARM64 app and is included by `AppARM64-ffmpeg.xcconfig`, so both shared ARM64 schemes receive the same version.

The `arm64-openjdk21-prod-*` tags identify dated Linux runtime baselines. They are not app release versions. The existing `v2.0.0` tag points to the divergent OpenMinis release commit and is retained for provenance; it is not an ancestor of this branch.

## Choose the next version

Use semantic versioning for the shipped application:

- increment the patch component for compatible fixes without a new user-visible capability;
- increment the minor component for compatible runtime, terminal or integration capabilities;
- increment the major component for incompatible app, rootfs or embedding changes.

Increment the Apple build number for every uploaded build, including rebuilds of the same release version. Never reuse a build number already uploaded to App Store Connect.

## Update the source

1. Change `MARKETING_VERSION` in `app/AppARM64.xcconfig`.
2. Change every `CURRENT_PROJECT_VERSION` entry in `iSH.xcodeproj/project.pbxproj` to the same new integer.
3. Update the current version in `README.md`, `docs/IOS_APPLICATION.md` and this file.
4. Add a dated report under `docs/reports/releases/` for source-release validation, an archive or a distribution run. State which kind of evidence it records; source validation alone does not establish iOS archive or device behaviour.

Verify the fields:

```sh
grep -n 'MARKETING_VERSION' app/Project.xcconfig app/AppARM64.xcconfig
grep -n 'CURRENT_PROJECT_VERSION = ' iSH.xcodeproj/project.pbxproj
```

All project build configurations must print the same build number.

## Validate the release commit

On the AArch64 Linux validation host:

```sh
CC=clang make build-arm64-linux-all
CC=clang make test-arm64-fcvt-vector
CC=clang make test-arm64-proc-mem-seek
CC=clang make test-arm64-lseek-width test-arm64-poke-stress
CC=clang make test-arm64-load64-fault-pc
make check-docs
git diff --check
git status --short
```

Run the runtime and workload gates required by the changed code as described in [VALIDATION.md](VALIDATION.md). Package-manager or repository failures must be recorded separately from emulator results.

Linux validation does not establish Xcode, signing or device behaviour. On macOS, build the two ARM64 schemes that will be distributed and smoke-test the exact archive on a physical device. The inherited Fastlane upload lane targets upstream iSH and must not be used without the changes listed in [IOS_APPLICATION.md](IOS_APPLICATION.md#fastlane-status).

## Commit and tag

Commit the version, documentation and release evidence together. Push the commit, create an annotated tag on that exact commit, then push the tag:

```sh
git push origin master
git tag -a v2.1.3 -m 'ios-linuxkit 2.1.3'
git push origin v2.1.3
```

Replace `2.1.3` with the version in `app/AppARM64.xcconfig`. Verify all three references:

```sh
git rev-parse HEAD
git rev-parse origin/master
git rev-list -n 1 v2.1.3
git ls-remote origin refs/heads/master refs/tags/v2.1.3 'refs/tags/v2.1.3^{}'
```

A Git tag records source provenance. It does not prove that an iOS archive was signed, installed or uploaded.
