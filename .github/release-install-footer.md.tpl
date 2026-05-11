<!-- jbox-install-footer -->

---

## Installation

1. Download `Jbox-${VERSION}.dmg` from the assets below.
2. Open the DMG; read `READ-THIS-FIRST.txt` inside.
3. Drag `Jbox.app` to `/Applications` (or anywhere you like).
4. **First launch:** this build is ad-hoc signed and not notarized. macOS will block the app — right-click → Open once to bypass Gatekeeper. Grant the microphone (audio-input) permission prompt that appears on first device enumeration.

The bundled `Uninstall Jbox.command` removes everything the installer deploys.

## Reporting issues

Please file bugs at <${ISSUES_URL}> with:

- macOS version
- audio interface model(s) involved
- relevant lines from `~/Library/Logs/Jbox/Jbox.log` (or `JboxEngineCLI.log` if you used the CLI)
