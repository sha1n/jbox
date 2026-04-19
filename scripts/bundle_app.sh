#!/usr/bin/env bash
#
# bundle_app.sh — wrap the SPM-built JboxApp executable into a macOS .app bundle.
#
# Produces build/Jbox.app from the release-mode JboxApp executable.
# Ad-hoc signs the bundle so macOS will run it locally without
# Gatekeeper warnings on the user's own Mac.
#
# Prerequisites: run `swift build -c release` before this script.
#
# See docs/spec.md § 5.2 and § 5.5.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
APP_NAME="Jbox"
APP_BUNDLE="${BUILD_DIR}/${APP_NAME}.app"
EXECUTABLE_NAME="JboxApp"
BUNDLE_ID="dev.sha1n.Jbox"

# Locate the built executable. SPM places it under .build/release/ on
# command-line builds, or under an arch-specific path on some configs.
if [[ -x "${ROOT_DIR}/.build/release/${EXECUTABLE_NAME}" ]]; then
  SRC_BINARY="${ROOT_DIR}/.build/release/${EXECUTABLE_NAME}"
elif [[ -x "${ROOT_DIR}/.build/arm64-apple-macosx/release/${EXECUTABLE_NAME}" ]]; then
  SRC_BINARY="${ROOT_DIR}/.build/arm64-apple-macosx/release/${EXECUTABLE_NAME}"
else
  echo "bundle_app: release binary ${EXECUTABLE_NAME} not found." >&2
  echo "            Run 'swift build -c release' first." >&2
  exit 1
fi

# Version and build-number derivation.
# Phase 1 defaults; Phase 8 refines with git-describe based versioning.
VERSION="${JBOX_VERSION:-0.1.0-dev}"
BUILD_NUMBER="${JBOX_BUILD_NUMBER:-$(date -u +%Y%m%d%H%M)}"

echo "bundle_app: building ${APP_BUNDLE} (version ${VERSION}, build ${BUILD_NUMBER})"

# Fresh bundle.
rm -rf "${APP_BUNDLE}"
mkdir -p "${APP_BUNDLE}/Contents/MacOS"
mkdir -p "${APP_BUNDLE}/Contents/Resources"

# Copy executable.
cp "${SRC_BINARY}" "${APP_BUNDLE}/Contents/MacOS/${APP_NAME}"
chmod +x "${APP_BUNDLE}/Contents/MacOS/${APP_NAME}"

# Generate Info.plist.
cat > "${APP_BUNDLE}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>${APP_NAME}</string>
    <key>CFBundleIdentifier</key>
    <string>${BUNDLE_ID}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>${APP_NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${APP_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundleVersion</key>
    <string>${BUILD_NUMBER}</string>
    <key>LSMinimumSystemVersion</key>
    <string>15.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>Jbox needs access to your audio devices to route audio between them.</string>
</dict>
</plist>
PLIST

# Ad-hoc sign with Hardened Runtime so the bundle runs cleanly on the
# user's own Mac. For distribution, a Developer ID signature would
# replace the `-` with an identity name.
codesign --sign - --force --options runtime "${APP_BUNDLE}" >/dev/null

echo "bundle_app: done — ${APP_BUNDLE}"
