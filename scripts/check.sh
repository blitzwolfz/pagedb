#!/bin/sh
# Builds the three configurations and runs the tests in each of them.
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

echo "== normal build =="
cmake -S . -B build/normal -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build/normal -j 4
ctest --test-dir build/normal --output-on-failure

echo "== address and undefined sanitizer =="
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DPAGEDB_ASAN=ON > /dev/null
cmake --build build/asan -j 4
ctest --test-dir build/asan --output-on-failure

echo "== thread sanitizer =="
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DPAGEDB_TSAN=ON > /dev/null
cmake --build build/tsan -j 4
ctest --test-dir build/tsan --output-on-failure

echo "all checks passed"
