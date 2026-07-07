#!/bin/bash
# build.sh - exact build command for the macOS shell. One command, no Xcode
# project. Produces build/iRate.app, ad-hoc signed so TCC/permission prompts and
# security-scoped bookmarks behave. Run from the mac/ directory: ./build.sh
#
# Toolchain: Apple clang (Xcode command line tools). core.h is shared verbatim
# from ../source and compiles clean under clang.
set -euo pipefail
cd "$(dirname "$0")"

APP="build/iRate.app"
MACOS="$APP/Contents/MacOS"
RES="$APP/Contents/Resources"

rm -rf "$APP"
mkdir -p "$MACOS" "$RES"
cp Info.plist "$APP/Contents/Info.plist"
[ -f iRate.icns ] && cp iRate.icns "$RES/iRate.icns"

clang++ -std=c++17 -fobjc-arc -O2 -Wall -Wextra \
    -x objective-c++ main.mm -o "$MACOS/iRate" \
    -framework Cocoa -framework ImageIO -framework CoreGraphics \
    -framework CoreServices -framework UniformTypeIdentifiers

# Ad-hoc sign with the entitlements. Non-sandboxed by default (see the
# entitlements file); ad-hoc identity ("-") is enough for a personal build.
codesign --force --sign - --entitlements iRate.entitlements "$APP"

echo "built $APP"
