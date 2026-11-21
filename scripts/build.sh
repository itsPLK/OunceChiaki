#!/bin/bash
# Builds build/switch2/OunceChiaki.nro.
#
# On the Mac (or any host with Docker) this re-runs itself in the build image,
# building the image from docker/Dockerfile on first use. Inside the image, or
# in CI where DEVKITPRO already has the SDK, it builds directly.
#
#   scripts/build.sh            # incremental build
#   scripts/build.sh --clean    # remove build/switch2 first (keeps build/deps)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
image=ounce-chiaki-build

if [ ! -f "${DEVKITPRO:-/nonexistent}/libounce/cmake/Switch2.cmake" ]; then
    # Rebuild the image when the files that define it change.
    recipe=$(cat scripts/install-sdk.sh docker/Dockerfile | shasum -a 256 | cut -c1-16)
    if [ "$(docker image inspect "$image" --format '{{index .Config.Labels "ounce.recipe"}}' 2>/dev/null)" != "$recipe" ]; then
        docker build -t "$image" --label "ounce.recipe=$recipe" -f docker/Dockerfile .
    fi
    tty=()
    [ -t 1 ] && tty=(-t)
    exec docker run --rm ${tty[@]+"${tty[@]}"} -v "$root:/repo" -w /repo -e OUNCE_JOBS "$image" scripts/build.sh "$@"
fi

if [ "${1:-}" = --clean ]; then
    rm -rf build/switch2
fi
git config --global --add safe.directory '*' 2>/dev/null || true
scripts/deps.sh
cmake -S . -B build/switch2 -G Ninja -DPLATFORM_SWITCH2=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_PREFIX_PATH="$root/build/deps/prefix" -DCMAKE_FIND_ROOT_PATH="$root/build/deps/prefix"
cmake --build build/switch2 --target OunceChiaki.nro -j "${OUNCE_JOBS:-$(nproc)}"
ls -l build/switch2/OunceChiaki.nro
