#!/usr/bin/env bash
#
# run_app.sh — build, bundle, and launch Jbox.app locally.
#
# Convenience target for iterating on the app during development.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

"${ROOT_DIR}/scripts/build_release.sh"

open "${ROOT_DIR}/build/Jbox.app"
