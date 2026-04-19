#!/usr/bin/env bash
#
# build_release.sh — convenience wrapper: release-mode build + .app bundle.
#
# Phase 1: minimal implementation (delegates to swift build + bundle_app.sh).
# Phase 8 extends with version stamping from git-describe, cleanup of
# stale build artifacts, and release-note hooks.
#
# See docs/plan.md § Phase 8.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "build_release: compiling in release mode..."
swift build --package-path "${ROOT_DIR}" -c release

echo "build_release: bundling app..."
"${ROOT_DIR}/scripts/bundle_app.sh"

echo "build_release: done."
