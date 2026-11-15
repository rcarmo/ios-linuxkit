#!/usr/bin/env bash
# Focused, bounded gate. Build candidates first via make -j2 ... NINJA='ninja -j2'.
set -euo pipefail
PROJECT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$PROJECT/tests/arm64/upstream"
BUILD_DIR=${BUILD_DIR:-$PROJECT/build-arm64-linux}
CC=${CC:-clang}
TIMEOUT_S=${TIMEOUT_S:-30}
TMP=${EVIDENCE_DIR:-$(mktemp -d)}
mkdir -p "$TMP"
if [[ -z ${EVIDENCE_DIR:-} ]]; then trap 'rm -rf "$TMP"' EXIT; fi
test "$(uname -m)" = aarch64 || { echo 'requires AArch64 Linux host' >&2; exit 1; }
for platform in linux darwin nonarm; do
    defs=()
    [[ $platform != darwin ]] || defs+=(-DTEST_DARWIN=1)
    [[ $platform != nonarm ]] || defs+=(-DTEST_NONARM=1)
    timeout -k 2 "$TIMEOUT_S" "$CC" -O2 -Wall -Wextra -Werror "${defs[@]}" \
        -I"$HERE/sha512-fallback-include" "$HERE/sha512-fallback-detect.c" -o "$TMP/detect-$platform"
    timeout -k 2 "$TIMEOUT_S" "$TMP/detect-$platform"
done
# Cross-assemble actual Mach-O gadgets too, checking portable names/relocations.
# Cross-target assembly needs Clang even when the host C compiler is GCC/cc.
timeout -k 2 "$TIMEOUT_S" "${CLANG:-clang}" -target arm64-apple-macos11 -c -I"$BUILD_DIR" \
    "$PROJECT/asbestos/guest-arm64/gadgets-aarch64/crypto.S" -o "$TMP/crypto-macho.o"
# Link the actual make-built candidate objects, not a copy of the gadgets.
timeout -k 2 "$TIMEOUT_S" "$CC" -O2 -Wall -Wextra -Werror -I"$BUILD_DIR" \
    "$HERE/sha512-fallback.c" "$HERE/sha512-fallback.S" \
    "$BUILD_DIR/libish_emu.a.p/asbestos_guest-arm64_crypto_helpers.c.o" \
    "$BUILD_DIR/libish_emu.a.p/asbestos_guest-arm64_gadgets-aarch64_crypto.S.o" \
    -o "$TMP/sha512-fallback"
timeout -k 2 "$TIMEOUT_S" env ISH_HOST_SHA512=0 "$TMP/sha512-fallback" forced | tee "$TMP/forced.log"
timeout -k 2 "$TIMEOUT_S" env -u ISH_HOST_SHA512 "$TMP/sha512-fallback" auto | tee "$TMP/auto.log"
cmp "$TMP/forced.log" "$TMP/auto.log"
timeout -k 2 "$TIMEOUT_S" env ISH_HOST_SHA512=0 "$TMP/sha512-fallback" forced no-native-oracle
# Override values other than exactly "0" must remain feature-detected.
timeout -k 2 "$TIMEOUT_S" env ISH_HOST_SHA512=1 "$TMP/sha512-fallback" auto > "$TMP/env-one.log"
cmp "$TMP/auto.log" "$TMP/env-one.log"
# Adversarial ABI lane: the actual helpers run, then legal caller-saved
# register/NZCV/FPSR clobbers prove the gadget really restores live state.
wraps=()
for helper in sha512su0 sha512su1 sha512h sha512h2; do
    wraps+=("-Wl,--wrap=${helper}_helper")
done
timeout -k 2 "$TIMEOUT_S" "$CC" -O2 -Wall -Wextra -Werror -DSHA512_CLOBBER_HELPERS=1 \
    -I"$BUILD_DIR" "$HERE/sha512-fallback.c" "$HERE/sha512-fallback.S" \
    "$BUILD_DIR/libish_emu.a.p/asbestos_guest-arm64_crypto_helpers.c.o" \
    "$BUILD_DIR/libish_emu.a.p/asbestos_guest-arm64_gadgets-aarch64_crypto.S.o" \
    "${wraps[@]}" -o "$TMP/sha512-fallback-clobber"
timeout -k 2 "$TIMEOUT_S" env ISH_HOST_SHA512=0 "$TMP/sha512-fallback-clobber" forced no-native-oracle
# End-to-end decoder/gadget dispatch through a disposable realfs, no shared root.
mkdir -p "$TMP/root"
timeout -k 2 "$TIMEOUT_S" "$CC" -O2 -static -Wall -Wextra -Werror -DSHA512_GUEST=1 \
    -I"$BUILD_DIR" "$HERE/sha512-fallback.c" "$HERE/sha512-fallback.S" -o "$TMP/root/sha512"
timeout -k 2 "$TIMEOUT_S" env ISH_HOST_SHA512=0 "$BUILD_DIR/ish" -r "$TMP/root" /sha512 > "$TMP/guest-forced.log" 2>&1
timeout -k 2 "$TIMEOUT_S" env -u ISH_HOST_SHA512 "$BUILD_DIR/ish" -r "$TMP/root" /sha512 > "$TMP/guest-auto.log" 2>&1
for mode in forced auto; do
    grep -q '^sha512-fallback-guest-ok instructions=1024' "$TMP/guest-$mode.log"
    grep '^sha512-fallback-guest-ok ' "$TMP/guest-$mode.log"
done
# Never run the ISA fixture natively on a host that lacks FEAT_SHA512.
if grep -q 'native-oracle=1' "$TMP/auto.log"; then
    timeout -k 2 "$TIMEOUT_S" "$TMP/root/sha512"
fi
echo 'sha512-fallback-gate-ok: detectors Linux/Darwin/non-ARM; Mach-O; helpers; all register aliases; ABI/NZCV/FPSR clobbers; forced/auto; guest decoder'
