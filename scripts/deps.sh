#!/bin/bash
# Builds the C libraries OunceChiaki links (mbedTLS, Opus) for libounce into
# build/deps/prefix. Archives are pinned by SHA-256 (scripts/deps/sources.sha256).
# Each library is built once; delete build/deps to rebuild. Run inside the build
# image (scripts/build.sh does).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
src="$root/build/deps/src"
work="$root/build/deps/work"
prefix="$root/build/deps/prefix"
jobs=${OUNCE_JOBS:-$(nproc)}
toolchain="$DEVKITPRO/libounce/cmake/Switch2.cmake"
[ -f "$toolchain" ] || { echo "deps.sh: no libounce toolchain in DEVKITPRO=$DEVKITPRO" >&2; exit 1; }
mkdir -p "$src" "$work" "$prefix"

fetch() {
    local name=$1 url=$2 archive checksum download
    archive="$src/$name.tar"
    checksum=$(awk -v file="$name.tar" '$2 == file { print $1 }' "$root/scripts/deps/sources.sha256")
    [ -n "$checksum" ] || { echo "deps.sh: no checksum for $name" >&2; exit 1; }
    if [ ! -f "$archive" ] || ! printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status; then
        download=$(mktemp "$archive.XXXXXX")
        curl --fail --location --retry 3 --silent --show-error "$url" -o "$download"
        printf '%s  %s\n' "$checksum" "$download" | sha256sum --check --status ||
            { echo "deps.sh: SHA-256 mismatch for $name" >&2; rm -f "$download"; exit 1; }
        mv -f "$download" "$archive"
    fi
    if [ ! -d "$src/$name" ]; then
        mkdir -p "$src/$name"
        tar -xf "$archive" -C "$src/$name" --strip-components=1
    fi
}

lib() {
    local name=$1
    shift
    [ -f "$work/$name.done" ] && return
    echo "deps.sh: building $name"
    if ! { cmake -S "$src/$name" -B "$work/$name" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF "$@" &&
           cmake --build "$work/$name" --target install -j "$jobs"; } >"$work/$name.log" 2>&1; then
        tail -n 40 "$work/$name.log" >&2
        exit 1
    fi
    touch "$work/$name.done"
}

# mbedTLS: entropy comes from libounce's csrng service (scripts/deps/mbedtls_entropy.c).
fetch mbedtls https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v2.28.10.tar.gz
if [ ! -f "$src/mbedtls/.ounce-entropy" ]; then
    printf '\n#define MBEDTLS_NO_PLATFORM_ENTROPY\n#define MBEDTLS_ENTROPY_HARDWARE_ALT\n' \
        >>"$src/mbedtls/include/mbedtls/config.h"
    sed -i -E 's@^#define (MBEDTLS_TIMING_C|MBEDTLS_HAVEGE_C|MBEDTLS_NET_C)$@// #define \1@' \
        "$src/mbedtls/include/mbedtls/config.h"
    printf '\ntarget_sources(mbedcrypto PRIVATE "%s/scripts/deps/mbedtls_entropy.c")\n' "$root" \
        >>"$src/mbedtls/library/CMakeLists.txt"
    touch "$src/mbedtls/.ounce-entropy"
fi
lib mbedtls -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF

fetch opus https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
lib opus -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF

echo "deps.sh: libraries ready in $prefix"
