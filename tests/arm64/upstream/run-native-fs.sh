#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT=$(cd "$HERE/../../.." && pwd)
: "${BUILD_DIR:?set BUILD_DIR to matching candidate build}"
TMP=${EVIDENCE_DIR:-$(mktemp -d)}
mkdir -p "$TMP"
if [[ -z ${EVIDENCE_DIR:-} ]]; then trap 'rm -rf "$TMP"' EXIT; fi
mkdir -p "$TMP/root"/{a,b,jail/inside,mounted}
printf alpha > "$TMP/root/a/item"
printf beta > "$TMP/root/b/item"
printf jailed > "$TMP/root/jail/value"
printf mounted > "$TMP/root/mounted/value"
ln -sf /b/item "$TMP/root/a/link"
ln -sf /value "$TMP/root/jail/inside/absolute"
tar -C "$TMP/root" -czf "$TMP/root.tar.gz" .
"$BUILD_DIR/tools/fakefsify" "$TMP/root.tar.gz" "$TMP/fake"
DEFS=(-DGUEST_ARM64=1 -DENGINE_ASBESTOS=1)
if grep -q -- '-DISH_JIT=1' "$BUILD_DIR/compile_commands.json"; then DEFS+=(-DISH_JIT=1); fi
"${CC:-clang}" -O2 -Wall -Wextra -Werror "${DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/native-fs.c" -Wl,--wrap=malloc -Wl,--start-group "$BUILD_DIR/libish.a" \
    "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3 -o "$TMP/native-fs"
for type in real fake; do
    root="$TMP/root"; [[ $type == fake ]] && root="$TMP/fake/data"
    timeout -k 3 30 "$TMP/native-fs" "$type" "$root" > "$TMP/$type.log" 2>&1 || { cat "$TMP/$type.log"; exit 1; }
    grep -q '^native-fs-context-ok ' "$TMP/$type.log" || { cat "$TMP/$type.log"; exit 1; }
    cat "$TMP/$type.log"
done
