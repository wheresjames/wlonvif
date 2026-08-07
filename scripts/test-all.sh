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

# ThreadSanitizer needs its own tree: it cannot be combined with AddressSanitizer,
# and the races it finds on the completion paths are invisible to the others.
cmake -S "$root" -B "$root/build-tsan" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DWLONVIF_BUILD_TESTING=ON \
    -DWLONVIF_ENABLE_THREAD_SANITIZER=ON \
    -DWLONVIF_WARNINGS_AS_ERRORS=ON
cmake --build "$root/build-tsan" --parallel "$jobs"
TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=1} \
ctest --test-dir "$root/build-tsan" --output-on-failure

prefix="$root/build/install-smoke"
cmake --install "$root/build" --prefix "$prefix"
cmake -S "$root/tests/consumer" -B "$root/build/consumer" \
    -DCMAKE_PREFIX_PATH="$prefix"
cmake --build "$root/build/consumer" --parallel "$jobs"
"$root/build/consumer/wlonvif_consumer"

# Fuzz smoke run. Short by design: this is a regression gate, not a fuzzing
# campaign. Both targets read untrusted device and network input.
if command -v clang++ >/dev/null 2>&1; then
    fuzz_seconds=${WLONVIF_FUZZ_SECONDS:-30}
    cmake -S "$root" -B "$root/build-fuzz" \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_BUILD_TYPE=Debug \
        -DWLONVIF_BUILD_TESTING=OFF \
        -DWLONVIF_BUILD_FUZZERS=ON
    cmake --build "$root/build-fuzz" --parallel "$jobs"
    # libFuzzer writes newly discovered inputs into the FIRST corpus directory it
    # is given. That directory therefore has to be scratch space under the build
    # tree; the checked-in seeds are passed afterwards and are only ever read.
    corpus="$root/build-fuzz/corpus"
    mkdir -p "$corpus/xml" "$corpus/xml_scan"
    "$root/build-fuzz/wlonvif_xml_fuzzer" \
        -max_total_time="$fuzz_seconds" -max_len=8192 \
        "$corpus/xml" "$root/tests/fuzz/seeds/xml"
    "$root/build-fuzz/wlonvif_xml_scan_fuzzer" \
        -max_total_time="$fuzz_seconds" -max_len=8192 \
        "$corpus/xml_scan" "$root/tests/fuzz/seeds/xml_scan"
else
    echo "clang++ not found; skipping fuzz smoke run"
fi
