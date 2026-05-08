#!/usr/bin/env bash
#
# package_unsigned_release.sh — build a Jbox-<version>.dmg for distribution.
#
# The DMG contains:
#   Jbox.app                   — GUI app + bundled JboxEngineCLI
#   Applications (symlink)     — drag-to-install target
#   Uninstall Jbox.command     — double-click to remove all deployed files
#   READ-THIS-FIRST.txt        — first-launch Gatekeeper + CLI usage
#
# The `.app` is ad-hoc signed via `scripts/bundle_app.sh` (no paid
# Apple Developer Program required). Recipients on other Macs will
# get a one-time Gatekeeper warning on first launch; the workaround
# is in READ-THIS-FIRST.txt.
#
# Versioning: set the JBOX_VERSION environment variable (CI pulls
# this from the git tag). Defaults to "0.0.0-local".
#
# Prerequisites: a release-mode build and a bundled .app. Run
# `scripts/build_release.sh` first (or rely on the CI release
# workflow that chains everything).

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
APP_BUNDLE="${BUILD_DIR}/Jbox.app"

VERSION="${JBOX_VERSION:-0.0.0-local}"
DMG_NAME="Jbox-${VERSION}.dmg"
DMG_PATH="${BUILD_DIR}/${DMG_NAME}"
VOLUME_NAME="JBox ${VERSION}"
STAGING="${BUILD_DIR}/dmg-staging"

if [[ ! -d "${APP_BUNDLE}" ]]; then
  echo "package_unsigned_release: ${APP_BUNDLE} not found." >&2
  echo "            Run scripts/build_release.sh first." >&2
  exit 1
fi

echo "package_unsigned_release: staging ${DMG_NAME} (volume name: ${VOLUME_NAME})"

# Fresh staging directory.
rm -rf "${STAGING}" "${DMG_PATH}"
mkdir -p "${STAGING}"

# 1. The app bundle (already ad-hoc signed).
cp -R "${APP_BUNDLE}" "${STAGING}/Jbox.app"

# 2. Applications symlink — the traditional drag-to-install affordance.
ln -s /Applications "${STAGING}/Applications"

# 3. Uninstaller .command script — double-click in Finder to run in Terminal.
#
# Paths are listed literally rather than templated because this file
# is shipped to users; we want it auditable as-is.
cat > "${STAGING}/Uninstall Jbox.command" <<'UNINSTALLER'
#!/bin/bash
# Uninstall Jbox.command — removes all files deployed or created by Jbox.
#
# Double-click this file to run the uninstaller. macOS will launch it
# in Terminal.app. Requires no elevated privileges; asks for confirmation
# before removing anything.

set -eu

cat <<'HEADER'
JBox Uninstaller
================
HEADER
echo ""

paths=(
    "/Applications/Jbox.app"
    "$HOME/Library/Application Support/Jbox"
    "$HOME/Library/Logs/Jbox"
    "$HOME/Library/Preferences/dev.sha1n.Jbox.plist"
)

existing=()
for p in "${paths[@]}"; do
    if [[ -e "$p" ]] || [[ -L "$p" ]]; then
        existing+=("$p")
    fi
done

if [[ ${#existing[@]} -eq 0 ]]; then
    echo "Nothing to uninstall — JBox does not appear to be installed."
    echo ""
    read -p "Press Enter to close this window..." _
    exit 0
fi

echo "The following will be removed:"
for p in "${existing[@]}"; do
    echo "   $p"
done
echo ""

read -p "Proceed? [y/N] " reply
case "$reply" in
    y|Y|yes|YES) ;;
    *)
        echo "Cancelled."
        read -p "Press Enter to close this window..." _
        exit 0
        ;;
esac

for p in "${existing[@]}"; do
    echo "Removing $p"
    rm -rf -- "$p"
done

echo ""
echo "Done. JBox has been uninstalled."
echo ""
echo "Note: audio device permissions granted under"
echo "  System Settings > Privacy & Security > Microphone"
echo "remain until you remove them manually. Reinstalling JBox will"
echo "prompt for access again."
echo ""
read -p "Press Enter to close this window..." _
UNINSTALLER

chmod +x "${STAGING}/Uninstall Jbox.command"

# 4. User-facing README.
cat > "${STAGING}/READ-THIS-FIRST.txt" <<README
JBox ${VERSION}
==============

JBox is a native macOS audio routing utility. This disk image contains:

  Jbox.app                     the application bundle
  Applications (shortcut)      drag Jbox.app onto this to install
  Uninstall Jbox.command       double-click to remove all files
  READ-THIS-FIRST.txt          this file


INSTALL
-------

1. Drag Jbox.app onto the Applications shortcut in this window.


FIRST LAUNCH (one-time Gatekeeper workaround)
---------------------------------------------

This build is ad-hoc signed, not notarized by Apple. On first launch
macOS will show:

    "JBox" cannot be opened because Apple cannot check it for
    malicious software.

To proceed:

1. Open /Applications in Finder.
2. Right-click Jbox.app (or Ctrl-click) and choose Open.
3. In the new dialog, click Open.
4. You only need to do this once — subsequent launches work normally.

If you prefer the CLI (currently the only interface that exposes
routing controls in this build), see USING THE CLI below.


USING THE CLI
-------------

The command-line tool ships inside the .app bundle at:

    /Applications/Jbox.app/Contents/MacOS/JboxEngineCLI

Usage:

    /Applications/Jbox.app/Contents/MacOS/JboxEngineCLI --list-devices
    /Applications/Jbox.app/Contents/MacOS/JboxEngineCLI --route '<src-uid>@1,2-><dst-uid>@3,4'

Channel numbers in the CLI are 1-indexed.

If macOS blocks the CLI on first run ("cannot verify"), clear the
quarantine bit:

    xattr -d com.apple.quarantine /Applications/Jbox.app


UNINSTALL
---------

Double-click "Uninstall Jbox.command" on this disk image. It lists
and removes:

    /Applications/Jbox.app
    ~/Library/Application Support/Jbox
    ~/Library/Logs/Jbox
    ~/Library/Preferences/dev.sha1n.Jbox.plist

Audio device permissions granted in System Settings > Privacy &
Security can be revoked manually if desired.


SUPPORT
-------

https://github.com/sha1n/jbox
README

# 5. Build the DMG. UDZO = read-only, zlib-compressed.
echo "package_unsigned_release: running hdiutil..."
hdiutil create \
    -volname "${VOLUME_NAME}" \
    -srcfolder "${STAGING}" \
    -ov \
    -format UDZO \
    "${DMG_PATH}" >/dev/null

# Tidy up the staging directory.
rm -rf "${STAGING}"

echo "package_unsigned_release: done — ${DMG_PATH}"
ls -lh "${DMG_PATH}"
