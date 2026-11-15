#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/../../.." && pwd)"
CC="${CC:-clang}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
FLAGS=(-std=c11 -Wall -Wextra -Werror -I"$PROJECT")
# SANITIZE=1 enables both ASan and UBSan. No project build or emulator needed.
if [[ "${SANITIZE:-0}" == 1 ]]; then
    FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
for opt in -O0 -O2; do
    "$CC" "${FLAGS[@]}" "$opt" "$PROJECT/kernel/memory_pressure_linux.c" \
        "$HERE/memory-pressure-linux.c" -o "$TMP/sampler"
    mkdir "$TMP/fixture"
    timeout 20 "$TMP/sampler" "$TMP/fixture"
    rm -rf "$TMP/fixture"
done
# Explicit read-only live probe. Disabled in deterministic fixture runs;
# unsupported/delegated hosts may legitimately reject instead of falling back.
if [[ "${LIVE:-0}" == 1 ]]; then
    timeout 20 "$TMP/sampler" --live
fi
