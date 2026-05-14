#!/usr/bin/env bash
# Cross-compile OpenSSL for Quest 3 (arm64-v8a, API 29).
#
# Prerequisites:
#   - Android NDK r25 or later installed
#   - ANDROID_NDK_ROOT env var pointing at the NDK
#   - OpenSSL source extracted somewhere (passed as first arg)
#
# Usage:
#   ./scripts/build_openssl_android.sh /path/to/openssl-3.2.1
#
# Output:
#   android/prebuilt/arm64-v8a/{lib,include}/

set -euo pipefail

OPENSSL_SRC="${1:?usage: $0 /path/to/openssl-source}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PREFIX="$PROJECT_ROOT/android/prebuilt/arm64-v8a"
API=29

if [[ -z "${ANDROID_NDK_ROOT:-}" ]]; then
    echo "ANDROID_NDK_ROOT is not set" >&2
    exit 1
fi

HOST_TAG="linux-x86_64"
if [[ "$(uname -s)" == "Darwin" ]]; then
    HOST_TAG="darwin-x86_64"
fi
TOOLCHAIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG"
export PATH="$TOOLCHAIN/bin:$PATH"
export ANDROID_NDK_ROOT

mkdir -p "$PREFIX"

pushd "$OPENSSL_SRC" > /dev/null
make clean 2>/dev/null || true

# android-arm64 target has been in OpenSSL since 1.1.
# no-shared -> static .a output (no .so to ship)
# no-tests  -> skip test binaries
# no-asm    -> drop arch-specific asm (slower but always builds clean;
#              remove this once you verify the build works)
./Configure android-arm64 \
    -D__ANDROID_API__=$API \
    --prefix="$PREFIX" \
    no-shared no-tests no-asm

make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
make install_sw  # headers + libs only, skip docs

popd > /dev/null

echo
echo "OpenSSL installed to: $PREFIX"
ls -la "$PREFIX/lib/"libssl.a "$PREFIX/lib/"libcrypto.a
