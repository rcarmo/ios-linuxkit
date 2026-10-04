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

xcodebuild -project "$repo_dir/iSH.xcodeproj" -scheme iSH-ARM64-AOT-Bootstrap \
    -configuration Release -destination 'generic/platform=iOS' \
    -derivedDataPath "$derived_data" IPHONEOS_DEPLOYMENT_TARGET=15.0 "$@" build

bun "$repo_dir/tools/jit_aot/apple.ts" inspect \
    "$derived_data/Build/Products/Release-iphoneos/meson-aot-bootstrap" \
    "$derived_data/Build/Products/Release-iphoneos/LinuxKit AOT Bootstrap.app/LinuxKit AOT Bootstrap"
