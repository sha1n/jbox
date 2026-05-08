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
# APP_NAME is the on-disk filename ("Jbox.app", binary "Jbox" inside
# Contents/MacOS, icon "Jbox.icns"). It is also the bundle id leaf and
# the log directory name — all stable identifiers.
APP_NAME="Jbox"
# APP_DISPLAY_NAME is what macOS shows the user (Dock, menu bar, About
# box, Get Info). Stylized capitalization is allowed to diverge from
# the on-disk filename.
APP_DISPLAY_NAME="JBox"
APP_BUNDLE="${BUILD_DIR}/${APP_NAME}.app"
EXECUTABLE_NAME="JboxApp"
CLI_NAME="JboxEngineCLI"
BUNDLE_ID="dev.sha1n.Jbox"

# Resolve a built binary from the release tree. SPM places binaries
# under .build/release/ on command-line builds, or under an
# arch-specific path on some configurations.
find_release_binary() {
  local name="$1"
  if [[ -x "${ROOT_DIR}/.build/release/${name}" ]]; then
    echo "${ROOT_DIR}/.build/release/${name}"
  elif [[ -x "${ROOT_DIR}/.build/arm64-apple-macosx/release/${name}" ]]; then
    echo "${ROOT_DIR}/.build/arm64-apple-macosx/release/${name}"
  else
    return 1
  fi
}

if ! SRC_BINARY=$(find_release_binary "${EXECUTABLE_NAME}"); then
  echo "bundle_app: release binary ${EXECUTABLE_NAME} not found." >&2
  echo "            Run 'swift build -c release' first." >&2
  exit 1
fi
if ! SRC_CLI_BINARY=$(find_release_binary "${CLI_NAME}"); then
  echo "bundle_app: release binary ${CLI_NAME} not found." >&2
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

# Copy executables. JboxEngineCLI is shipped inside the app bundle so
# it travels with the app and is cleaned up by a single drag-to-Trash.
cp "${SRC_BINARY}" "${APP_BUNDLE}/Contents/MacOS/${APP_NAME}"
chmod +x "${APP_BUNDLE}/Contents/MacOS/${APP_NAME}"
cp "${SRC_CLI_BINARY}" "${APP_BUNDLE}/Contents/MacOS/${CLI_NAME}"
chmod +x "${APP_BUNDLE}/Contents/MacOS/${CLI_NAME}"

# Slice the 2048x2048 source PNG into the standard iconset and emit
# Contents/Resources/Jbox.icns. Uses only macOS built-ins (sips,
# iconutil) so this works on a clean Mac without extra tooling.
ICON_SRC="${ROOT_DIR}/assets/jbox-icon.png"
if [[ ! -f "${ICON_SRC}" ]]; then
    echo "bundle_app: icon source ${ICON_SRC} not found." >&2
    exit 1
fi
ICONSET_DIR="${BUILD_DIR}/Jbox.iconset"
rm -rf "${ICONSET_DIR}"
mkdir -p "${ICONSET_DIR}"
for spec in "16 icon_16x16.png" \
            "32 icon_16x16@2x.png" \
            "32 icon_32x32.png" \
            "64 icon_32x32@2x.png" \
            "128 icon_128x128.png" \
            "256 icon_128x128@2x.png" \
            "256 icon_256x256.png" \
            "512 icon_256x256@2x.png" \
            "512 icon_512x512.png" \
            "1024 icon_512x512@2x.png"; do
    size="${spec% *}"
    name="${spec#* }"
    sips -z "${size}" "${size}" "${ICON_SRC}" \
        --out "${ICONSET_DIR}/${name}" >/dev/null
done
iconutil -c icns "${ICONSET_DIR}" -o "${APP_BUNDLE}/Contents/Resources/Jbox.icns"
rm -rf "${ICONSET_DIR}"

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
    <key>CFBundleIconFile</key>
    <string>Jbox</string>
    <key>CFBundleIconName</key>
    <string>Jbox</string>
    <key>CFBundleIdentifier</key>
    <string>${BUNDLE_ID}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>${APP_DISPLAY_NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${APP_DISPLAY_NAME}</string>
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
    <string>JBox needs access to your audio devices to route audio between them.</string>
    <key>NSHumanReadableCopyright</key>
    <string>Copyright © 2026 Shai Nagar. Licensed under the Apache License, Version 2.0.</string>
</dict>
</plist>
PLIST

# Entitlements plist — required under Hardened Runtime for Core Audio
# input access. Emitted inline via heredoc, same pattern as Info.plist
# above; if this ever grows past a single key, move it to a template
# file under Sources/JboxApp/Resources/. See docs/spec.md § 1.5.
ENTITLEMENTS_PATH="${BUILD_DIR}/Jbox.entitlements"
cat > "${ENTITLEMENTS_PATH}" <<ENTITLEMENTS
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.device.audio-input</key>
    <true/>
</dict>
</plist>
ENTITLEMENTS

# Ad-hoc sign with Hardened Runtime so the bundle runs cleanly on the
# user's own Mac. For distribution, a Developer ID signature would
# replace the `-` with an identity name.
codesign --sign - --force --options runtime \
    --entitlements "${ENTITLEMENTS_PATH}" \
    "${APP_BUNDLE}" >/dev/null

# Post-sign verification. Under Hardened Runtime, Core Audio silently
# delivers zero-filled buffers when the bundle doesn't claim
# `com.apple.security.device.audio-input` — the IOProc still fires and
# frames_produced still advances, so the bug is invisible except via
# signal meters. Fail loudly here instead so a future edit to the
# signing line can't reintroduce the all-silent bug. See docs/spec.md
# § 1.5.
REQUIRED_ENTITLEMENT="com.apple.security.device.audio-input"
ENTITLEMENTS_DUMP=$(codesign -d --entitlements - --xml "${APP_BUNDLE}" 2>/dev/null || true)
if ! printf '%s' "${ENTITLEMENTS_DUMP}" | grep -q "${REQUIRED_ENTITLEMENT}"; then
    echo "bundle_app: post-sign check failed — ${REQUIRED_ENTITLEMENT} is not claimed." >&2
    echo "            Hardened Runtime will silence audio input. Fix codesign args." >&2
    exit 1
fi

echo "bundle_app: done — ${APP_BUNDLE}"
