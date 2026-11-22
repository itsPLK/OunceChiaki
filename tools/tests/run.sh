#!/bin/bash
# Host tests for code that doesn't need the console. Needs a C++20 compiler.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
c++ -std=c++20 -O1 -Wall -DFMT_HEADER_ONLY -I"$root/src/video" -I"$root/extern/borealis/library/lib/extern/fmt/include" \
    "$root/tools/tests/sps_test.cpp" "$root/src/video/h264_sps.cpp" -o "$out/sps_test"
"$out/sps_test"
c++ -std=c++20 -O1 -Wall -I"$root/src/video" "$root/tools/tests/frame_generation_test.cpp" -o "$out/frame_generation_test"
"$out/frame_generation_test"
c++ -std=c++20 -O1 -Wall -I"$root/src/app" "$root/tools/tests/overlay_metrics_test.cpp" -o "$out/overlay_metrics_test"
"$out/overlay_metrics_test"
c++ -std=c++20 -O1 -Wall -I"$root/src/app" "$root/tools/tests/input_test.cpp" -o "$out/input_test"
"$out/input_test"
