#!/bin/sh
set -eu

if [ "$#" -lt 1 ]; then
    echo 'Usage: sh scripts/build-ios-aot-bootstrap.sh DERIVED_DATA [Xcode signing settings...]' >&2
    exit 2
fi
derived_data=$1
shift
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(dirname "$script_dir")

build_app() {
    xcodebuild -project "$repo_dir/iSH.xcodeproj" -scheme iSH-ARM64-AOT-Bootstrap \
        -configuration Release -destination "${IOS_DESTINATION:-generic/platform=iOS}" \
        -derivedDataPath "$derived_data" IPHONEOS_DEPLOYMENT_TARGET=15.0 "$@"
}
build_app "$@" build

# Resolve actual products from Xcode; simulator destinations use a different SDK
# directory and product names can be overridden by the caller.
settings=$(mktemp "${TMPDIR:-/tmp}/linuxkit-aot-settings.XXXXXX")
trap 'rm -f "$settings"' EXIT HUP INT TERM
build_app "$@" -showBuildSettings -json > "$settings"
bun "$repo_dir/tools/jit_aot/apple.ts" inspect-settings "$settings"
