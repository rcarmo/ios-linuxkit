#!/bin/sh
# Offline regression for the actual app downloader. Mock only network transfer;
# retain real hash, archive extraction and architecture validation.
set -eu
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
project=$(CDPATH= cd -- "$here/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/source/bin" "$tmp/products/App"
# Minimal ELF headers are enough for file(1); no executable fixture is run.
printf '\177ELF\002\001\001\000' > "$tmp/source/bin/busybox"
dd if=/dev/zero bs=1 count=56 >> "$tmp/source/bin/busybox" 2>/dev/null
printf '\002\000\267\000\001\000\000\000' | dd of="$tmp/source/bin/busybox" bs=1 seek=16 conv=notrunc 2>/dev/null
file "$tmp/source/bin/busybox" | grep -q 'ARM aarch64'
tar -C "$tmp/source" -czf "$tmp/arm.tar.gz" bin/busybox
printf '\076\000' | dd of="$tmp/source/bin/busybox" bs=1 seek=18 conv=notrunc 2>/dev/null
tar -C "$tmp/source" -czf "$tmp/x86.tar.gz" bin/busybox
cat > "$tmp/bin/curl" <<'MOCK'
#!/bin/sh
set -eu
while [ "$#" -gt 0 ]; do
 case "$1" in --output) shift; output=$1 ;; esac
 shift
done
if [ "${FAIL_FETCH:-0}" = 1 ]; then printf partial > "$output"; exit 22; fi
cp "$FIXTURE_ARCHIVE" "$output"
MOCK
chmod +x "$tmp/bin/curl"
export PATH="$tmp/bin:$PATH" ROOTFS_URL=example.invalid/root.tar.gz ROOTFS_ARCH=aarch64
export BUILT_PRODUCTS_DIR="$tmp/products" CONTENTS_FOLDER_PATH=App
export FIXTURE_ARCHIVE="$tmp/arm.tar.gz"
ROOTFS_SHA256=$(sha256sum "$FIXTURE_ARCHIVE" | cut -d ' ' -f 1); export ROOTFS_SHA256
sh "$project/app/download-root.sh"
cmp "$FIXTURE_ARCHIVE" "$tmp/products/App/root.tar.gz"
cp "$tmp/products/App/root.tar.gz" "$tmp/good"
reject() {
 if "$@" > "$tmp/reject.log" 2>&1; then echo 'unexpected downloader acceptance' >&2; exit 1; fi
 cmp "$tmp/good" "$tmp/products/App/root.tar.gz"
 test -z "$(find "$tmp/products/App" -name '.linuxkit-root.*' -print)"
}
reject env FAIL_FETCH=1 sh "$project/app/download-root.sh"
reject env ROOTFS_SHA256=0000000000000000000000000000000000000000000000000000000000000000 sh "$project/app/download-root.sh"
reject env ROOTFS_SHA256=bad sh "$project/app/download-root.sh"
reject env -u ROOTFS_SHA256 sh "$project/app/download-root.sh"
x86_hash=$(sha256sum "$tmp/x86.tar.gz" | cut -d ' ' -f 1)
reject env FIXTURE_ARCHIVE="$tmp/x86.tar.gz" ROOTFS_SHA256="$x86_hash" sh "$project/app/download-root.sh"
printf 'rootfs-downloader-ok: good archive, failed fetch/hash/arch, missing/malformed pin, old bundle preserved\n'
