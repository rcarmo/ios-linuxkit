#!/usr/bin/env bash
# Standalone opt-in gate. Never modify run.sh, build the project, or reuse a unit.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/../../.." && pwd)"
CC="${CC:-clang}"
ISH_BIN="${1:-${ISH_BIN:-$PROJECT/build-arm64-linux/ish}}"
BUILD_DIR="${2:-${BUILD_DIR:-$(dirname "$ISH_BIN")}}"
RESERVE_MIB="${SAFETY_RESERVE_MIB:-4}"
TIMEOUT_S="${TIMEOUT_S:-90}"
TMP="${EVIDENCE_DIR:-$(mktemp -d /tmp/ish-memory-pressure-evidence-XXXXXX)}"
mkdir -p "$TMP"; TMP="$(cd "$TMP" && pwd)"
UNIT="ish-memory-pressure-cgroup-$(id -u)-$$-${RANDOM}.service"
MODE=user
cleanup() {
    if [[ "$MODE" == user ]]; then
        systemctl --user stop "$UNIT" >/dev/null 2>&1 || true
        systemctl --user reset-failed "$UNIT" >/dev/null 2>&1 || true
    else
        sudo -n systemctl stop "$UNIT" >/dev/null 2>&1 || true
        sudo -n systemctl reset-failed "$UNIT" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT
blocker() { printf 'BLOCKER: %s\nEvidence: %s\n' "$1" "$TMP" >&2; exit 77; }
[[ "$(uname -m)" == aarch64 ]] || blocker 'native AArch64 host required'
[[ "$RESERVE_MIB" =~ ^[0-8]$ ]] || { echo 'SAFETY_RESERVE_MIB must be 0..8' >&2; exit 2; }
[[ "$TIMEOUT_S" =~ ^[0-9]+$ ]] && ((TIMEOUT_S > 0 && TIMEOUT_S <= 90)) || { echo 'TIMEOUT_S must be 1..90' >&2; exit 2; }
[[ -x "$ISH_BIN" ]] || blocker "missing candidate ISH_BIN: $ISH_BIN"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
DEFS=(-DGUEST_ARM64=1 -DENGINE_ASBESTOS=1)
if [[ -f "$BUILD_DIR/compile_commands.json" ]] && grep -q -- '-DISH_JIT=1' "$BUILD_DIR/compile_commands.json"; then DEFS+=(-DISH_JIT=1); fi
LIBS=(-Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3)
{
    printf 'host=%s arch=%s uid=%s unit=%s reserve_mib=%s\n' "$(hostname)" "$(uname -m)" "$(id -u)" "$UNIT" "$RESERVE_MIB"
    printf 'candidate=%s builddir=%s\n' "$ISH_BIN" "$BUILD_DIR"
    printf 'link-inputs (thin archives may reference candidate object files):\n'
    sha256sum "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a"
    printf 'command: '; printf '%q ' "$CC" -O2 -Wall -Wextra -Werror "${DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread "$HERE/memory-pressure-cgroup.c" "${LIBS[@]}" -o "$TMP/memory-pressure-cgroup"; printf '\n'
} > "$TMP/build-receipt.txt"
"$CC" -O2 -Wall -Wextra -Werror "${DEFS[@]}" -I"$PROJECT" -I"$BUILD_DIR" -pthread \
    "$HERE/memory-pressure-cgroup.c" "${LIBS[@]}" -o "$TMP/memory-pressure-cgroup" > "$TMP/build.log" 2>&1 || {
    tail -n 80 "$TMP/build.log" >&2; exit 1;
}
# This read-only probe requests delegation in another disposable service. It
# neither enables controllers nor writes controls in existing ancestor groups.
PROBE='p=/sys/fs/cgroup$(awk -F: '\''$1==0 {print $3}'\'' /proc/self/cgroup); test -w "$p/cgroup.procs" && test -w "$p/cgroup.subtree_control" && grep -qw memory "$p/cgroup.controllers"'
RUN=(systemd-run --user)
if ! timeout 10 "${RUN[@]}" --quiet --wait --pipe -p Delegate=yes /bin/sh -c "$PROBE" > "$TMP/delegation.log" 2>&1; then
    [[ "${ALLOW_SUDO_DELEGATE:-0}" == 1 ]] || blocker 'rootless user delegation unavailable (see delegation.log); optional ALLOW_SUDO_DELEGATE=1 uses a new system unit'
    MODE=system; RUN=(sudo -n systemd-run --uid="$(id -un)")
    timeout 10 "${RUN[@]}" --quiet --wait --pipe -p Delegate=yes /bin/sh -c "$PROBE" >> "$TMP/delegation.log" 2>&1 || blocker 'neither user nor explicitly allowed sudo delegation available'
fi
printf 'delegation_mode=%s\n' "$MODE" >> "$TMP/build-receipt.txt"
sha256sum "$HERE/memory-pressure-cgroup.c" "$HERE/run-memory-pressure-cgroup.sh" "$TMP/memory-pressure-cgroup" >> "$TMP/build-receipt.txt"
# Hash thin-archive members too: hashing the ar index alone is not provenance.
for archive in "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a"; do
    while IFS= read -r member; do
        if [[ "$member" != /* ]]; then member="$(dirname "$archive")/$member"; fi
        if [[ -f "$member" ]]; then sha256sum "$member"; fi
    done < <(ar t "$archive")
done > "$TMP/candidate-objects.sha256"
set +e
timeout -k 2 "$TIMEOUT_S" "${RUN[@]}" --unit="$UNIT" --wait --pipe \
    -p Delegate=yes -p MemoryHigh=160M -p MemoryMax=192M -p MemorySwapMax=0 \
    -p RuntimeMaxSec=90 -p TimeoutStopSec=2 -p KillMode=control-group \
    "$TMP/memory-pressure-cgroup" --delegated "$UNIT" "$RESERVE_MIB" > "$TMP/test.log" 2>&1
rc=$?
set -e
printf 'exit_status=%s\n' "$rc" >> "$TMP/build-receipt.txt"
# Capture unit controls/status before stop; the C log contains memory.events.
if [[ "$MODE" == user ]]; then
    systemctl --user show "$UNIT" -p Result -p ControlGroup -p MemoryPeak -p ExecMainStatus > "$TMP/unit-receipt.txt" 2>&1 || true
else
    sudo -n systemctl show "$UNIT" -p Result -p ControlGroup -p MemoryPeak -p ExecMainStatus > "$TMP/unit-receipt.txt" 2>&1 || true
fi
tail -n 100 "$TMP/test.log"
printf 'Evidence: %s\n' "$TMP"
((rc == 0)) || exit "$rc"
! grep -Eq 'BLOCKER|SKIP|FAIL' "$TMP/test.log" || exit 1
grep -Fq 'memory-pressure-cgroup-actual-bridge-ok tests=11' "$TMP/test.log"
echo 'memory-pressure-cgroup-gate-ok tests=11'
