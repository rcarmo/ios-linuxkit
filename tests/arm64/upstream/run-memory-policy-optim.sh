#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT/build-arm64-linux}"
TMP="${EVIDENCE_DIR:-$(mktemp -d)}"
mkdir -p "$TMP"
if [[ -z "${EVIDENCE_DIR:-}" ]]; then trap 'rm -rf "$TMP"' EXIT; fi
FLAGS=(-O2 -Wall -Wextra -Werror -DGUEST_ARM64=1 -DENGINE_ASBESTOS=1)
if grep -q -- '-DISH_JIT=1' "$BUILD_DIR/compile_commands.json"; then FLAGS+=(-DISH_JIT=1); fi
LIBS=()
if [[ "${SANITIZE:-0}" == 1 ]]; then
    FLAGS+=(-O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    if [[ "$(uname -s)" == Linux ]]; then LIBS+=(-latomic); fi
fi
"${CC:-clang}" "${FLAGS[@]}" \
    -I"$PROJECT" -I"$BUILD_DIR" -pthread "$HERE/memory-policy-optim.c" \
    -Wl,--wrap=mprotect -Wl,--wrap=malloc -Wl,--wrap=munmap -Wl,--wrap=sched_yield \
    -Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" \
    -Wl,--end-group -lrt -lm -ldl -lsqlite3 "${LIBS[@]}" -o "$TMP/memory-policy-optim"
if [[ "${1:-}" != bench ]]; then timeout 20 "$TMP/memory-policy-optim"; fi
timeout 20 "$TMP/memory-policy-optim" bench
