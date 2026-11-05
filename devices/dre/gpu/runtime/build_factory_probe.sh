#!/bin/bash
# SPDX-License-Identifier: MIT
# macOS with Xcode iPhoneOS SDK. Builds a read-only observer; never installs it.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build_factory_probe.sh NEW_OUTPUT_DIRECTORY}
mkdir "$out"
out=$(cd "$out" && pwd)
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -isysroot "$sdk" \
    -std=c11 -O2 -Wall -Wextra -Werror "$here/factory_probe.c" -o "$out/dre-metal-factory-probe"
codesign --force --sign - "$out/dre-metal-factory-probe"
(cd "$out" && shasum -a 256 dre-metal-factory-probe > SHA256SUMS)
echo 'Built read-only discovery observer; no Metal device or acceleration proved'
