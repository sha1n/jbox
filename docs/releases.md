# JBox — Release Process

This doc traces the release pipeline end-to-end, with special attention to **every place the version number appears** and how they stay in sync. If you are just cutting a release, skim this, run the commands in the README's ["Releases" section](../README.md#releases), and come back here only if something is out of alignment.

---

## Source of truth: the Git tag

The release flow is designed so **only one thing changes per release: the Git tag**. Everything downstream derives from it — Info.plist fields, DMG filename, DMG volume name, the GitHub Release record, the About dialog inside the app. You don't edit version strings anywhere in the code or docs.

Tag format: `vMAJOR.MINOR.PATCH[-LABEL]`. Examples: `v0.0.1-alpha`, `v0.1.0`, `v1.0.0`, `v1.2.3`. The `v` prefix is required (the release workflow matches tags against `v*`).

The rest of this doc explains what gets set, where, from that one tag.

---

## The version synchronization map

Every place the version surfaces in a shipped release:

| # | Where it appears | Who writes it | How it derives from the tag |
|---|---|---|---|
| 1 | **Git tag** itself (`vX.Y.Z[-label]`) | You (via `git tag`) | Source of truth. |
| 2 | **`JBOX_VERSION` env var** in CI | `.github/workflows/release.yml` | `tag=${GITHUB_REF_NAME}`; `version="${tag#v}"` |
| 3 | **`CFBundleShortVersionString`** in `Jbox.app/Contents/Info.plist` | `scripts/bundle_app.sh` | Templated from `${JBOX_VERSION}`; becomes `X.Y.Z[-label]` |
| 4 | **`CFBundleVersion`** (build number) in `Jbox.app/Contents/Info.plist` | `scripts/bundle_app.sh` | Defaults to a timestamp `YYYYMMDDHHMM`. Override by setting `JBOX_BUILD_NUMBER`. |
| 5 | **macOS About dialog** (JBox menu → About JBox) | macOS, automatically | Reads `CFBundleShortVersionString` + `CFBundleVersion` from the Info.plist. Nothing to do in Swift code. |
| 6 | **Finder → Get Info** on Jbox.app | macOS, automatically | Same Info.plist fields. |
| 7 | **DMG filename** `JBox-<version>.dmg` | `scripts/package_unsigned_release.sh` | `${JBOX_VERSION}` |
| 8 | **DMG volume name** ("JBox 0.0.1-alpha" when mounted) | `scripts/package_unsigned_release.sh` | `${JBOX_VERSION}` |
| 9 | **`READ-THIS-FIRST.txt`** inside the DMG | `scripts/package_unsigned_release.sh` (heredoc) | `${JBOX_VERSION}` |
| 10 | **GitHub Release tag** | `softprops/action-gh-release@v2` | Literally the Git tag (`v*`). |
| 11 | **GitHub Release name** ("JBox X.Y.Z") | `softprops/action-gh-release@v2` | Computed from the version output in the workflow. |
| 12 | **GitHub Release notes** | `softprops/action-gh-release@v2` | `generate_release_notes: true` — auto-compiled from commits since the previous tag. |

What the table does **not** include:

- **`JBOX_ENGINE_ABI_VERSION`** in `Sources/JboxEngineC/include/jbox_engine.h`. This is the C-bridge ABI version — an independent lifecycle. Bump it only on breaking C-API changes, and only deliberately. Almost every product-version bump leaves the ABI version unchanged. See [spec.md § 1.6](./spec.md#16-versioning-of-the-bridge-api).
- **`Package.swift`**. Swift Package Manager does not require the package to declare its own version when it is the root package (we don't publish it as a library dependency).
- **Release notes body in workflow**. The `release.yml` workflow sets a static `body:` block that describes what's in the DMG. It's product-release metadata, not the version — update only when the DMG layout changes.

---

## End-to-end flow

```
  you                         CI (.github/workflows/release.yml)
  ───                         ─────────────────────────────────────
  git tag -a vX.Y.Z ... ──►   GITHUB_REF_NAME=vX.Y.Z
  git push origin vX.Y.Z           │
                                   ▼
                              step: Derive version
                                   │
                                   ▼
                              JBOX_VERSION=X.Y.Z
                                   │
                  ┌────────────────┼────────────────┐
                  ▼                ▼                ▼
          swift build -c     bundle_app.sh    package_unsigned_
          release            (Info.plist:     release.sh
                             CFBundle-        (Jbox-X.Y.Z.dmg,
                             ShortVersion-    volume "JBox X.Y.Z",
                             String=X.Y.Z)    README X.Y.Z)
                                   │                │
                                   └────────┬───────┘
                                            ▼
                               softprops/action-gh-release@v2
                               draft pre-release for tag vX.Y.Z
                               with Jbox-X.Y.Z.dmg attached
                                            │
                                            ▼
                               you: review draft → publish
```

---

## Cutting a release step by step

### 1. Pre-flight on `master`

```sh
git checkout master
git pull
./scripts/verify.sh   # runs the full CI pipeline locally (~30s with warm cache)
```

Everything should be green. If not, fix first.

### 2. Decide the version

Product version follows semantic versioning with an optional pre-release label:

- `v0.x.y` — development pre-releases while Phases 3–6 are in progress. The GUI is a placeholder; the CLI is the useful surface.
- `v0.x.y-alpha` / `-beta` / `-rc.N` — pre-release labels for internal checkpoints or shared testing.
- `v1.0.0` — first stable release, after Phase 6 (the real SwiftUI UI).
- `v1.X.Y` / `v2.0.0` — follow semver: PATCH for bugfixes, MINOR for additions, MAJOR for breaking user-visible changes.

**Note on labelled versions and Info.plist.** Apple technically wants `CFBundleShortVersionString` to be strict `X.Y.Z` with no alphabetic suffix. For ad-hoc distributed apps (not Mac App Store), the system tolerates values like `0.0.1-alpha` — they render as-is in the About dialog and Finder Get Info. For App Store distribution we'd need to strip the label at release time; not relevant for v1.

### 3. Tag and push

```sh
git tag -a v0.0.1-alpha -m "v0.0.1-alpha: engine + CLI pre-release"
git push origin v0.0.1-alpha
```

That is literally the only repo change needed. No file edits, no separate version commits, no CHANGELOG updates (auto-generated).

### 4. Watch CI

```sh
gh run watch --repo sha1n/jbox
```

Or in the browser: https://github.com/sha1n/jbox/actions. The "Release" workflow should take 1–2 minutes on the macOS runner.

### 5. Review and publish the draft

```sh
gh release view v0.0.1-alpha --repo sha1n/jbox
# or: open https://github.com/sha1n/jbox/releases
```

The release is created as a draft, pre-release. Review:

- Is the DMG attached? (Download it, mount it, check the volume name and file list.)
- Is the auto-generated release notes body reasonable? Edit if needed.
- Does the version in the title look right?

When happy, click **Publish release** (or use `gh release edit v0.0.1-alpha --draft=false`).

### 6. Verify on another machine (optional but recommended)

Download the DMG on a different Mac, mount, drag to Applications, right-click → Open, confirm:

- Menu bar: **JBox → About JBox** → version and build number match the tag.
- Finder → Applications → right-click Jbox.app → **Get Info** → Version field matches.
- DMG's `Uninstall Jbox.command` runs to completion and reports the right paths.

---

## How the About dialog gets the version automatically

Our SwiftUI `JboxApp` uses the default app menu. macOS auto-generates the **About JBox** menu item, which opens a system-provided window that reads **`CFBundleShortVersionString`** (shown after "Version ") and **`CFBundleVersion`** (shown in parentheses) from `Jbox.app/Contents/Info.plist`. No Swift code touches this.

Consequence: when `bundle_app.sh` templates the Info.plist with the right `JBOX_VERSION`, the About dialog is automatically correct for that build. There is nothing for you to update in `JboxApp.swift` or anywhere else in the Swift code to "make the About page carry the new version."

If you ever want a **custom** About dialog (logo, credits, license text, link to GitHub, etc.), that becomes a SwiftUI `AboutView` attached via the `.commands { CommandGroup(replacing: .appInfo) { ... } }` modifier. That's a Phase 6 concern.

---

## Reading the version at runtime (if you ever need to)

From Swift, either from the app or from the CLI (when the CLI runs from inside the app bundle):

```swift
let v = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "unknown"
```

The CLI's `--version` flag currently prints the **engine ABI version** only. It can be extended to also print the product version by reading `Bundle.main`. That would turn this:

```
$ JboxEngineCLI --version
JBox Engine ABI version: 1
```

into:

```
$ JboxEngineCLI --version
JBox 0.0.1-alpha (build 202604191322)
Engine ABI version: 1
```

Not currently wired up; tracked as a small follow-up.

---

## Redoing a bad release

If a tag was pushed but the build failed, or you realised there's an issue after the draft was created:

```sh
# Local: delete the tag locally and remotely.
git tag -d v0.0.1-alpha
git push origin :refs/tags/v0.0.1-alpha

# GitHub: delete the draft release.
gh release delete v0.0.1-alpha --repo sha1n/jbox

# Fix whatever was wrong, commit to master, then re-tag.
git tag -a v0.0.1-alpha -m "..."
git push origin v0.0.1-alpha
```

**If the release was already published publicly:** do NOT delete or overwrite it. Ship a new version (`v0.0.2-alpha` or `v0.0.1-alpha.1`) instead — tag immutability is the expected contract for consumers.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Release workflow doesn't trigger when you push a tag | Tag doesn't start with `v` | Workflow is scoped to `v*` — re-tag with `vX.Y.Z` |
| Workflow fails at "Create GitHub Release" step | Missing permissions | Ensure `permissions: contents: write` at workflow level (already set in `release.yml`) |
| About dialog shows the wrong version after install | DMG from a previous version still cached on disk | Delete the old Jbox.app from Applications; redownload the new DMG |
| `CFBundleShortVersionString` rejected by a downstream tool | Alphabetic suffix (`-alpha`) | Non-issue for our ad-hoc distribution; would matter only for App Store |
| Two builds produced with the same version have different `CFBundleVersion` | Timestamp-based build number is expected to differ per-build | Not a bug; `CFBundleVersion` is advisory outside the App Store |
| DMG mounts but Finder shows wrong volume name | `hdiutil` cached a previous image under the same filename | `scripts/package_unsigned_release.sh` does `rm -rf` on the old DMG path before creating a new one; this should not happen. If it does, unmount everything and retry. |

---

## What to update *in the code/docs* for a new version

**Tl;dr: nothing.** The tag is the only change.

Exceptions — things that are technically **not** version-derived but that often accompany a release:

- **CHANGELOG.** Auto-generated by the workflow. Override by editing the draft release body before publishing.
- **README status section** (the "Phase X" current state). Update when a phase completes, not on every release.
- **`docs/plan.md` phase statuses.** Update when phases complete.
- **`scripts/bundle_app.sh`'s `NSHumanReadableCopyright`**. Only changes yearly.

None of these require touching the release pipeline itself.
