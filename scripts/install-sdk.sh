#!/bin/bash
# Installs the host tools and the pinned public ounce-devkit into $DEVKITPRO.
# Used by docker/Dockerfile (local builds) and the GitHub Actions workflows, so
# both build with the same SDK. Run as root inside devkitpro/devkita64.
set -euo pipefail

OUNCE_DEVKIT_VERSION=20261115b
OUNCE_DEVKIT_SHA256=e6866198bfb44144c8227288c6315772a6a505cacca54b08cd0e6232a84f4729
: "${DEVKITPRO:=/opt/devkitpro}"

apt-get update
apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake curl git glslang-tools ninja-build \
    pkg-config python3 python3-protobuf protobuf-compiler spirv-tools xz-utils
rm -rf /var/lib/apt/lists/*

archive=$(mktemp --suffix=.tar.xz)
curl --fail --location --retry 3 --output "$archive" \
    "https://github.com/ouncebrew/ounce-devkit/releases/download/devkit-${OUNCE_DEVKIT_VERSION}/ounce-devkit-${OUNCE_DEVKIT_VERSION}.tar.xz"
printf '%s  %s\n' "$OUNCE_DEVKIT_SHA256" "$archive" | sha256sum --check
tar -xJf "$archive" -C "$DEVKITPRO"
rm -f "$archive"
test -f "$DEVKITPRO/libounce/cmake/Switch2.cmake"
cat "$DEVKITPRO/ounce-devkit-version.txt"
