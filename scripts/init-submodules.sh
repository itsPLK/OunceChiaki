#!/bin/bash
# Checks out the submodules the build needs, and only those: chiaki-ng's own
# submodules include curl, Android's oboe and another Borealis, which we don't use.
set -euo pipefail
cd "$(dirname "$0")/.."
git submodule update --init third_party/chiaki-ng extern/borealis
git -C third_party/chiaki-ng submodule update --init --depth 1 \
    third-party/nanopb third-party/jerasure third-party/gf-complete
git -C extern/borealis submodule update --init --depth 1 library/lib/extern/SDL3
