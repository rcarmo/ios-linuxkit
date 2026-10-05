# Versioning and releases

The current ARM64 app version is **2.5.2**, with Apple build number **826**.
[Release notes](reports/releases/IOS_LINUXKIT_2.5.2.md) describe the changes and
known limitations.

## Version settings

| Value | Location | Purpose |
|---|---|---|
| App version | `MARKETING_VERSION` in `app/AppARM64.xcconfig` | The version shown to users. |
| Apple build number | All four `CURRENT_PROJECT_VERSION` entries in `iSH.xcodeproj/project.pbxproj` | Identifies each build of the app. |
| Git tag | Annotated tag `v<version>` | Identifies the source used for a release. |

The ARM64 configurations share the same app version. The inherited non-ARM64
version in `app/Project.xcconfig` is separate. Dated runtime tags and upstream
build tags are not app release versions.

## Choose a version

- Increase the patch version for compatible fixes.
- Increase the minor version for new compatible features.
- Increase the major version for incompatible changes.

Use a new Apple build number for each distributed build, including a rebuild
of the same app version. Do not reuse a number uploaded to App Store Connect.

## Prepare the release

1. Update the app version and all four build-number entries.
2. Update the version in the README and iOS guide.
3. Write release notes describing visible changes, fixes and known limitations.
4. Run the tests relevant to the changes and the documentation checks.
5. Commit and push the release changes, excluding unrelated local edits.

Keep commands and configuration requirements in build guides. Release notes
should not contain internal work tracking, implementation history, hashes,
temporary log paths or test-run narration.

## Test and build

Use [Validation](VALIDATION.md) for runtime tests and
[iOS application](IOS_APPLICATION.md) for signing and device checks.
Memory, signal and translated-execution changes need both release and debug
testing. Test AOT changes with matching guest files and a build that cannot
generate executable code at runtime.

Before distribution, check the signed app on a physical device: startup,
terminal input and scrolling, repeated workloads, background/resume behavior,
upgrades and memory use. A successful build alone is insufficient.

## Tag the release

Create the tag on the intended committed revision:

```sh
git push origin master
git tag -a v2.5.2 -m 'ios-linuxkit 2.5.2'
git push origin v2.5.2
git rev-list -n 1 v2.5.2
git ls-remote origin refs/tags/v2.5.2 'refs/tags/v2.5.2^{}'
```

Replace the version in these commands for later releases. Do not move an
already published tag; commit documentation corrections normally.

Publish release notes against the verified tag when creating a GitHub release.
The inherited Fastlane upload configuration targets upstream iSH; review and
change it before using it for this app.
