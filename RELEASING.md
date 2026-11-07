# Release policy

The first release cycle is **0.1.0**, beginning with `0.1.0-testing.1`. Development stays on `main`; channels use versioned GitHub releases and immutable annotated tags, not permanent channel branches.

| Stage | Example tag | GitHub status | Purpose |
|---|---|---|---|
| Testing | `v0.1.0-testing.1` | Prerelease, not Latest | Features may change; find problems and report exact coverage |
| Release candidate | `v0.1.0-rc.1` | Prerelease, not Latest | Feature freeze; validate the exact intended stable APK |
| Stable | `v0.1.0` | Release, Latest | Recommend the approved RC artifacts without rebuilding |

## Version and tag rules

- Never move, delete or reuse a published `v0.*` or `v1.*`-style version tag. GitHub's version-tag ruleset blocks tag updates/deletions. Legacy uppercase `V1` predates this policy; do not reuse it.
- Fixes create a new version/tag (`testing.2`, `rc.2`, `0.1.1`, etc.). Tags may share a commit: the stable tag points at the approved RC commit.
- Never silently replace published assets. Publish SHA-256 checksums, source commit and signing-certificate fingerprint. A withdrawn build gets a prominent warning; its version is never reused.
- Increment Android `versionCode` for every new APK build/version. It must not go backwards even though the version name is restarting at 0.1.0. The first testing build uses code 3; codes 1 and 2 were already used during development.
- Promote RC to stable by copying the exact approved APK and matching builder/checksums. Do not rebuild or edit its embedded RC version name/code just for promotion. Record both GitHub stable version and embedded Android version in the release notes.
- **Debug keys only for Testing, RC and Stable.** Both Gradle debug and release variants explicitly use `signingConfigs.debug`; no separate production signing key is used. A release variant signed with a debug key is not necessarily a debuggable app.
- Reuse and privately back up the same maintainer debug keystore (`~/.android/debug.keystore`) for published updates. Another machine's automatically generated debug key will differ: Android refuses an in-place update signed by a different certificate. Verify the certificate against the previous APK and publish its SHA-256 fingerprint. Never commit/upload the keystore or expose its private key; the standard debug credentials are not a security boundary. Do not recommend uninstalling an existing game just to bypass a signing mismatch, because that deletes app data.
- Creating a tag does not build, install, upload or approve binaries. Publishing and stable promotion are separate, manually approved actions. No automatic publishing on pushes/tags is configured.

## Gates

### Testing

Before uploading an APK: successful production build, host tests, APK identity/content inspection, basic launch check, signing validation and distribution-license/source requirements. Use the isolated setup app for destructive import/storage experiments; test production-package upgrades separately without clearing user data. Notes must distinguish isolated-app results from production APK validation.

A **source-only prerelease checkpoint** is allowed while update-certificate validation or distribution gates remain unresolved. Label it explicitly: no installable build, no claim of binary-release readiness. GitHub source archives must not contain private game files or generated code.

### RC

Complete the following on the actual signed release candidate; features are frozen. Any code, dependency, packaging or signing change requires another RC and relevant retests.

- [ ] Fresh direct ISO/XISO import and automatic compilation/launch.
- [ ] Archive import (ZIP, 7z, RAR/RAR5, TAR/compressed TAR) and extracted-folder picker; disclose untested variants.
- [ ] Existing Windows-builder prebuilt ZIP compatibility.
- [ ] Back cancellation, verified-object resume, force-stop/reopen; no incomplete library publication.
- [ ] Manual picker cancellation/errors and storage-failure cleanup without destroying existing game files.
- [ ] Production-package upgrade preserves game files, saves/settings, launcher entry and signing compatibility.
- [ ] Controller, save/load, background/resume and mission transition checks; publish tested devices/GPUs/API levels.
- [ ] Correct runtime/compiler packaging, no private assets/XBE/generated game code, licenses, corresponding source/build recipes and SHA-256 checksums.
- [ ] Known risks documented, including untested 16-KiB devices, GPUs and campaign areas.

### Stable

- [ ] RC gates passed within the documented supported scope; no known release-blocking regression.
- [ ] Human explicitly approves stable promotion.
- [ ] Stable tag points to approved RC source commit; APK/builder/checksum bytes are unchanged.
- [ ] Publish setup/update instructions, supported game hash, known limitations, device coverage, embedded APK version/code and signing fingerprint.
- [ ] Mark stable Latest; retain earlier stable releases and identify the last known-good release when withdrawing a broken one.

## First checkpoint: 0.1.0-testing.1

Source-only prerelease. The isolated app's XISO → compile → gameplay flow was user-confirmed, and host/emulator tests passed. Production integration builds, but production-package device/upgrade/manual-picker testing, update-certificate compatibility and LGPL corresponding-source compliance remain open. No APK or Windows-builder binary should be uploaded under this checkpoint until those gates are satisfied; instead publish the next testing version once ready.

## Next development version: 0.1.0-testing.2

The debug-only signing policy was adopted after `v0.1.0-testing.1` was published. Its immutable tag stays unchanged. Current Android source is `0.1.0-testing.2` / code 4; this does not create or publish a new release automatically. The signing-key choice is settled, but compatibility with existing installed certificates still needs validation.

For local build/test commands see `README.md`, `android/tests/README.md` and `tools/on-device-setup/README.md`.
