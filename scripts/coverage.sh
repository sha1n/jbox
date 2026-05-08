#!/usr/bin/env bash
#
# coverage.sh — generate Swift + C++ code-coverage reports as lcov files.
#
# Two passes, because Jbox's C++ engine is exercised from two places:
#
#   1. `swift test --enable-code-coverage` — instruments every C/C++/Swift
#      target reached by the Swift test bundles. Covers the JboxEngineC
#      paths reached through the public C bridge plus all Swift code.
#
#   2. `JboxEngineCxxTests` — an SPM executable target (Catch2 runner) that
#      directly exercises C++ classes not exposed through the bridge. SPM's
#      `swift test --enable-code-coverage` does not run executable targets,
#      so we rebuild this binary with explicit `-fprofile-instr-generate
#      -fcoverage-mapping` and run it with LLVM_PROFILE_FILE set.
#
# Outputs:
#   test-results/swift-coverage.lcov   (Swift + bridge-reachable C++)
#   test-results/cxx-coverage.lcov     (engine internals via Catch2)
#   test-results/swift-junit.xml       (Swift Testing xunit)
#   test-results/cxx-junit.xml         (Catch2 JUnit)
#
# CI uploads both lcov files to Codecov; locally use `make coverage` to
# inspect them. Both pipelines call this script.
#
# Usage: ./scripts/coverage.sh

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT_DIR}"

mkdir -p test-results
rm -f test-results/*.profraw test-results/*.profdata test-results/*-coverage.lcov

# ---------------------------------------------------------------------------
# Pass 1 — Swift test bundles (with bridge-reachable C++).
# ---------------------------------------------------------------------------

echo ""
echo "=====================================================>>"
echo "  [1/2] Swift tests with coverage"
echo "=====================================================>>"

swift test --parallel --enable-code-coverage \
    --xunit-output test-results/swift-junit.xml

BIN_PATH="$(swift build --show-bin-path)"
SWIFT_TEST_BIN="${BIN_PATH}/JboxPackageTests.xctest/Contents/MacOS/JboxPackageTests"
SWIFT_PROFDATA="${BIN_PATH}/codecov/default.profdata"

xcrun llvm-cov export -format=lcov \
    "${SWIFT_TEST_BIN}" \
    -instr-profile="${SWIFT_PROFDATA}" \
    -ignore-filename-regex='(Tests|ThirdParty|\.build)/' \
    > test-results/swift-coverage.lcov

# ---------------------------------------------------------------------------
# Pass 2 — Catch2 C++ engine tests.
# ---------------------------------------------------------------------------

echo ""
echo "=====================================================>>"
echo "  [2/2] C++ engine tests with coverage"
echo "=====================================================>>"

# `-Xcc -fprofile-instr-generate` instruments the compile step but does
# not pull in the profiling runtime at link time (swiftc's `-Xcc` only
# applies to clang during compile). We resolve the archive path via
# `clang -print-resource-dir` and link it explicitly.
PROFILE_RUNTIME="$(xcrun clang -print-resource-dir)/lib/darwin/libclang_rt.profile_osx.a"

swift build --product JboxEngineCxxTests \
    -Xcc -fprofile-instr-generate -Xcc -fcoverage-mapping \
    -Xlinker "${PROFILE_RUNTIME}"

CXX_BIN="${BIN_PATH}/JboxEngineCxxTests"

LLVM_PROFILE_FILE="${ROOT_DIR}/test-results/cxx.profraw" \
    "${CXX_BIN}" \
    --reporter console \
    --reporter "junit::out=test-results/cxx-junit.xml"

xcrun llvm-profdata merge -sparse \
    test-results/cxx.profraw \
    -o test-results/cxx.profdata

xcrun llvm-cov export -format=lcov \
    "${CXX_BIN}" \
    -instr-profile=test-results/cxx.profdata \
    -ignore-filename-regex='(Tests|ThirdParty|\.build)/' \
    > test-results/cxx-coverage.lcov

# ---------------------------------------------------------------------------
# Local convenience summary — Codecov ignores this; humans like it.
# ---------------------------------------------------------------------------

echo ""
echo "Coverage summaries (excluding tests + ThirdParty):"
echo ""
echo "Swift + bridge-reachable C++:"
xcrun llvm-cov report \
    "${SWIFT_TEST_BIN}" \
    -instr-profile="${SWIFT_PROFDATA}" \
    -ignore-filename-regex='(Tests|ThirdParty|\.build)/' \
    | tail -n 1 | sed 's/^/  /'

echo ""
echo "C++ engine internals (Catch2):"
xcrun llvm-cov report \
    "${CXX_BIN}" \
    -instr-profile=test-results/cxx.profdata \
    -ignore-filename-regex='(Tests|ThirdParty|\.build)/' \
    | tail -n 1 | sed 's/^/  /'

echo ""
echo "lcov files written:"
echo "  test-results/swift-coverage.lcov"
echo "  test-results/cxx-coverage.lcov"
