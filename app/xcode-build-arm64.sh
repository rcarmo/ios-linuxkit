#!/bin/sh
set -eu

if [ "${TARGET_NAME:-}" = iSH-ARM64-AOT-Bootstrap ]; then
    "$SRCROOT/app/xcode-meson.sh" --aot-bootstrap
else
    "$SRCROOT/app/xcode-meson.sh"
fi
cd "$MESON_BUILD_DIR"
"$SRCROOT/app/xcode-ninja.sh" $NINJA_TARGETS

mkdir -p "$CONFIGURATION_BUILD_DIR"
for library in libish.a libish_emu.a libfakefs.a; do
    test -f "$MESON_BUILD_DIR/$library"
    ln -sf "$MESON_BUILD_DIR/$library" "$CONFIGURATION_BUILD_DIR/$library"
done
