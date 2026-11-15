#!/usr/bin/env bash
# Dedicated deterministic host gate; leave upstream run.sh and Makefile alone.
set -euo pipefail
PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT/build-arm64-linux}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$PROJECT"
# Make's -j2 alone does not constrain its nested ninja invocation.
timeout -k 2 90 make build-arm64-linux RELEASE_BUILD_DIR="$BUILD_DIR" NINJA='ninja -j2' -j2
DEFS=(-DGUEST_ARM64=1 -DENGINE_ASBESTOS=1)
if grep -q -- '-DISH_JIT=1' "$BUILD_DIR/compile_commands.json"; then DEFS+=(-DISH_JIT=1); fi
LIBS=(-Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3)
for name in poll-lifetime poll-deadline poll-edge; do
    WRAPS=()
    if [[ "$name" == poll-lifetime ]]; then
        symbols=(epoll_wait epoll_create1 epoll_ctl pipe close malloc free user_read user_write pthread_mutex_lock pthread_mutex_unlock pthread_mutex_trylock)
    elif [[ "$name" == poll-edge ]]; then
        symbols=(epoll_wait)
    else
        symbols=(epoll_wait clock_gettime nanosleep sockrestart_begin_listen_wait sockrestart_end_listen_wait_count pthread_mutex_lock send_signal)
    fi
    for symbol in "${symbols[@]}"; do WRAPS+=("-Wl,--wrap=$symbol"); done
    "${CC:-clang}" -O2 -Wall -Wextra -Werror "${DEFS[@]}" -I. -I"$BUILD_DIR" -pthread \
        "tests/arm64/upstream/$name.c" "${WRAPS[@]}" "${LIBS[@]}" -o "$TMP/$name"
    timeout -k 2 20 "$TMP/$name"
done
echo 'poll-fixes-ok'
