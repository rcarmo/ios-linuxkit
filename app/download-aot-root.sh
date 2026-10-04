#!/bin/sh
set -eu

[ "${TARGET_NAME:-}" = iSH-ARM64-AOT-Bootstrap ] || { echo 'Bun rootfs is for the isolated AOT bootstrap only' >&2; exit 2; }
output="$BUILT_PRODUCTS_DIR/$CONTENTS_FOLDER_PATH/root.tar.gz"
mkdir -p "$(dirname "$output")"
stage=$(mktemp -d "$(dirname "$output")/.linuxkit-aot-root.XXXXXX")
trap 'rm -rf "$stage"' EXIT
trap 'exit 1' INT TERM

BUILT_PRODUCTS_DIR="$stage" CONTENTS_FOLDER_PATH=base sh "$SRCROOT/app/download-root.sh"
if [ -n "${BUN_ARCHIVE_PATH:-}" ]; then
    bun_archive=$BUN_ARCHIVE_PATH
else
    bun_archive="$stage/bun.zip"
    curl --fail --location --retry 2 --output "$bun_archive" \
        https://github.com/oven-sh/bun/releases/download/bun-v1.4.2/bun-linux-aarch64-musl.zip
fi
for name in libgcc libstdc++; do
    curl --fail --location --retry 2 --output "$stage/$name.apk" \
        "https://dl-cdn.alpinelinux.org/alpine/v3.24/main/aarch64/$name-15.2.0-r5.apk"
done
python3 "$SRCROOT/scripts/package-bun-rootfs.py" \
    "$stage/base/root.tar.gz" "$bun_archive" "$output" "$ROOTFS_SHA256" \
    "$stage/libgcc.apk" "$stage/libstdc++.apk"
