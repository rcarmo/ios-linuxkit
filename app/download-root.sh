#!/bin/sh
set -eu

: "${ROOTFS_URL:?ROOTFS_URL is required}"
: "${ROOTFS_SHA256:?ROOTFS_SHA256 is required}"
: "${BUILT_PRODUCTS_DIR:?BUILT_PRODUCTS_DIR is required}"
: "${CONTENTS_FOLDER_PATH:?CONTENTS_FOLDER_PATH is required}"

rootfs_arch="${ROOTFS_ARCH:-aarch64}"
output="$BUILT_PRODUCTS_DIR/$CONTENTS_FOLDER_PATH/root.tar.gz"

case "$ROOTFS_SHA256" in
    *[!0-9a-fA-F]*) echo 'Invalid ROOTFS_SHA256' >&2; exit 1 ;;
esac
[ "${#ROOTFS_SHA256}" -eq 64 ] || { echo 'Invalid ROOTFS_SHA256 length' >&2; exit 1; }

# Stage next to the destination so only a fully validated archive is atomically
# installed. A failed fetch/check must not replace a previously good bundle.
mkdir -p "$(dirname "$output")"
tmpdir="$(mktemp -d "$(dirname "$output")/.linuxkit-root.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT
trap 'exit 1' INT TERM
archive="$tmpdir/root.tar.gz"
curl --fail --location --retry 2 "https://$ROOTFS_URL" --output "$archive"
if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$archive" | cut -d ' ' -f 1)"
else
    actual="$(shasum -a 256 "$archive" | cut -d ' ' -f 1)"
fi
expected="$(printf '%s' "$ROOTFS_SHA256" | tr 'A-F' 'a-f')"
[ "$actual" = "$expected" ] || { echo 'Rootfs SHA-256 mismatch' >&2; exit 1; }

tar -xzf "$archive" -C "$tmpdir" ./bin/busybox 2>/dev/null || tar -xzf "$archive" -C "$tmpdir" bin/busybox
description="$(file "$tmpdir/bin/busybox")"

case "$rootfs_arch:$description" in
    aarch64:*"ARM aarch64"*) ;;
    arm64:*"ARM aarch64"*) ;;
    *)
        echo "Refusing to package non-$rootfs_arch rootfs: $description" >&2
        exit 1
        ;;
esac
mv -f "$archive" "$output"
