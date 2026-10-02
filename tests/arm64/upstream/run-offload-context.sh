#!/usr/bin/env bash
# Executes the actual portable cooperative dispatcher under a Linux adapter.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT=$(cd "$HERE/../../.." && pwd)
: "${BUILD_DIR:?set BUILD_DIR to a matching candidate build}"
TMP=${EVIDENCE_DIR:-$(mktemp -d)}
mkdir -p "$TMP"
if [[ -z ${EVIDENCE_DIR:-} ]]; then trap 'rm -rf "$TMP"' EXIT; fi
mkdir -p "$TMP/root"/{a,b,mounted}
printf alpha > "$TMP/root/a/item"
printf beta > "$TMP/root/b/item"
printf mounted > "$TMP/root/mounted/value"
ln -sf /b/item "$TMP/root/a/link"
ln -sf /b/item "$TMP/root/b/link"
tar -C "$TMP/root" -czf "$TMP/root.tar.gz" .
"$BUILD_DIR/tools/fakefsify" "$TMP/root.tar.gz" "$TMP/fake"
DEFS=(-D_GNU_SOURCE -DGUEST_ARM64=1 -DENGINE_ASBESTOS=1 -DISH_NATIVE_OFFLOAD_TEST_HANDLERS=1)
if grep -q -- '-DISH_JIT=1' "$BUILD_DIR/compile_commands.json"; then DEFS+=(-DISH_JIT=1); fi
WRAPS=()
for name in malloc calloc strdup free chdir fchdir opendir pipe pthread_create do_exit; do WRAPS+=(-Wl,--wrap=$name); done
"${CC:-clang}" -O2 -Wall -Wextra -Werror -Wno-unused-function "${DEFS[@]}" \
    -I"$PROJECT" -I"$BUILD_DIR" -pthread "$HERE/offload-context.c" "$PROJECT/kernel/native_offload.c" \
    "${WRAPS[@]}" -Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" \
    "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3 -o "$TMP/offload-context"
for type in real fake; do
    root="$TMP/root"; [[ $type == fake ]] && root="$TMP/fake/data"
    timeout -k 3 30 "$TMP/offload-context" "$type" "$root" "$TMP/root/mounted" > "$TMP/$type.log" 2>&1 || { cat "$TMP/$type.log"; exit 1; }
    grep -q '^offload-context-ok ' "$TMP/$type.log" || { cat "$TMP/$type.log"; exit 1; }
    cat "$TMP/$type.log"
done
