#!/usr/bin/env bash
# Focused host-linked ownership gate. Does not modify the upstream run.sh.
set -euo pipefail
PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT/build-arm64-linux}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$PROJECT"
timeout -k 2 60 make build-arm64-linux RELEASE_BUILD_DIR="$BUILD_DIR" NINJA='ninja -j2' -j2
DEFS=(-DGUEST_ARM64=1 -DENGINE_ASBESTOS=1)
if grep -q -- '-DISH_JIT=1' "$BUILD_DIR/compile_commands.json"; then DEFS+=(-DISH_JIT=1); fi
WRAPS=()
for symbol in close socket socketpair pipe accept adhoc_fd_create f_install f_install_owned malloc calloc; do
    WRAPS+=("-Wl,--wrap=$symbol")
done
"${CC:-clang}" -O2 -Wall -Wextra -Werror "${DEFS[@]}" -I. -I"$BUILD_DIR" -pthread \
    tests/arm64/upstream/socket-ownership.c "${WRAPS[@]}" \
    -Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" \
    "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3 -o "$TMP/socket-ownership"
timeout -k 2 20 "$TMP/socket-ownership"
