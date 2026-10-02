#!/usr/bin/env bash
# Linux adapters execute the portable source and POSIX ownership paths.
# They are not Darwin SDK, dylib, signing or app execution evidence.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT=$(cd "$HERE/../../.." && pwd)
: "${BUILD_DIR:?set BUILD_DIR to a matching candidate build}"
TMP=${EVIDENCE_DIR:-$(mktemp -d)}
mkdir -p "$TMP"
if [[ -z ${EVIDENCE_DIR:-} ]]; then trap 'rm -rf "$TMP"' EXIT; fi
DEFS=(-D_GNU_SOURCE -DGUEST_ARM64=1 -DENGINE_ASBESTOS=1 -DISH_NATIVE_OFFLOAD_TEST_HANDLERS=1)
if grep -q -- '-DISH_JIT=1' "$BUILD_DIR/compile_commands.json"; then DEFS+=(-DISH_JIT=1); fi
LIBS=(-Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3)
WRAPS=()
for name in malloc calloc strdup free pipe fcntl open chdir pthread_create do_exit \
    posix_spawn_file_actions_init posix_spawnattr_init posix_spawn_file_actions_adddup2 \
    posix_spawn_file_actions_addclose posix_spawn_file_actions_addopen \
    posix_spawn_file_actions_addchdir_np posix_spawn; do WRAPS+=(-Wl,--wrap=$name); done
check() {
    local type=$1 mode=$2 log="$TMP/$1-$2.log"; shift 2
    timeout -k 3 30 "$@" > "$log" 2>&1 || { cat "$log"; return 1; }
    grep -q '^offload-setup-ok ' "$log" || { cat "$log"; return 1; }
    cat "$log"
}
for adapter in handler spawn; do
    EXTRA=()
    if [[ $adapter == spawn ]]; then EXTRA=(-DISH_NATIVE_OFFLOAD_TEST_SPAWN=1); fi
    "${CC:-clang}" -O2 -Wall -Wextra -Werror -Wno-unused-function "${DEFS[@]}" "${EXTRA[@]}" \
        -I"$PROJECT" -I"$BUILD_DIR" -pthread "$HERE/offload-setup.c" "$PROJECT/kernel/native_offload.c" \
        "${WRAPS[@]}" "${LIBS[@]}" -o "$TMP/$adapter"
    for mode in alloc pipe thread fcntl open chdir success cloexec lowfd; do
        check "$adapter" "handler-$mode" env -u TEST_SPAWN "$TMP/$adapter" "$mode"
    done
    if [[ $adapter == spawn ]]; then
        for mode in alloc pipe thread fcntl spawn success cloexec lowfd; do
            check "$adapter" "$mode" env TEST_SPAWN=1 "$TMP/$adapter" "$mode"
        done
    fi
done
printf 'offload-setup-gate-ok: transactional handler/spawn adapters; every setup failure position, exact fd refs/CWD/process rollback\n'
