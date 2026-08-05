#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-2}

cmake -S "$root" -B "$root/build" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DWLONVIF_BUILD_TESTING=ON \
    -DWLONVIF_BUILD_EXAMPLES=ON \
    -DWLONVIF_WARNINGS_AS_ERRORS=ON
cmake --build "$root/build" --parallel "$jobs"
ctest --test-dir "$root/build" --output-on-failure

cmake -S "$root" -B "$root/build-shared" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DWLONVIF_BUILD_TESTING=ON \
    -DWLONVIF_BUILD_EXAMPLES=ON \
    -DWLONVIF_WARNINGS_AS_ERRORS=ON
cmake --build "$root/build-shared" --parallel "$jobs"
ctest --test-dir "$root/build-shared" --output-on-failure

cmake -S "$root" -B "$root/build-sanitize" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DWLONVIF_BUILD_TESTING=ON \
    -DWLONVIF_BUILD_EXAMPLES=ON \
    -DWLONVIF_ENABLE_SANITIZERS=ON \
    -DWLONVIF_WARNINGS_AS_ERRORS=ON
cmake --build "$root/build-sanitize" --parallel "$jobs"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:strict_string_checks=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1} \
ctest --test-dir "$root/build-sanitize" --output-on-failure

prefix="$root/build/install-smoke"
cmake --install "$root/build" --prefix "$prefix"
cmake -S "$root/tests/consumer" -B "$root/build/consumer" \
    -DCMAKE_PREFIX_PATH="$prefix"
cmake --build "$root/build/consumer" --parallel "$jobs"
"$root/build/consumer/wlonvif_consumer"
