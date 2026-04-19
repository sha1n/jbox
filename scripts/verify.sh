#!/usr/bin/env bash
#
# verify.sh — local equivalent of the CI pipeline.
#
# Runs, in order:
#   1. RT-safety static scan of Sources/JboxEngineC/rt/
#   2. Release build of all SPM targets
#   3. Swift Testing tests (engine wrapper, placeholders)
#   4. C++ tests via Catch2 (with per-test timings)
#   5. C++ tests under ThreadSanitizer (race / data-race detection)
#
# Stops on the first failure (set -e). A successful run of this script
# is functionally equivalent to a green CI run on GitHub.
#
# Usage: ./scripts/verify.sh

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT_DIR}"

STEP=0
TOTAL=5

section() {
    STEP=$((STEP + 1))
    echo ""
    echo "=====================================================>>"
    echo "  [${STEP}/${TOTAL}] $1"
    echo "=====================================================>>"
}

section "RT-safety scan"
./scripts/rt_safety_scan.sh

section "Release build (swift build -c release)"
swift build -c release

section "Swift tests (swift test)"
swift test

section "C++ tests with per-test timings"
# `swift run` forwards unrecognized flags to the target, so Catch2's
# flags can be appended directly (no `--` separator needed — Catch2 v3
# does not accept a literal `--` token).
swift run JboxEngineCxxTests --durations yes

section "C++ tests under ThreadSanitizer"
swift run --sanitize=thread JboxEngineCxxTests

echo ""
echo "All verification steps passed."
