#!/bin/bash
# SPDX-License-Identifier: MIT
# Requires a macOS Xcode installation with the iPhoneOS SDK. Does not run/flash.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build_metal_probe.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
out=$(cd "$out" && pwd)
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -isysroot "$sdk" \
    -fobjc-arc -fblocks -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations \
    "$here/metal_probe.m" -framework Foundation -framework Metal -framework IOSurface \
    -o "$out/dre-metal-probe"
xcrun --sdk iphoneos metal -target air64-apple-ios16.0 -isysroot "$sdk" \
    -std=ios-metal2.3 -c "$here/metal_probe.metal" -o "$out/dre-metal-probe.air"
xcrun --sdk iphoneos metallib "$out/dre-metal-probe.air" -o "$out/dre-metal-probe.metallib"
cp "$here/metal_probe.metal" "$out/dre-metal-probe.metal"
codesign --force --sign - "$out/dre-metal-probe"
(cd "$out" && shasum -a 256 dre-metal-probe dre-metal-probe.metallib dre-metal-probe.metal > SHA256SUMS)
echo 'Built iOS Metal acceptance executable; this does not establish Metal support'
