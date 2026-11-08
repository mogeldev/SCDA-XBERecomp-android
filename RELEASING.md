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
- **Every future release APK must include both `arm64-v8a` and `x86_64` runtime libraries.** Build without `-PdevCode`; never publish private game code/assets. Inspect the packaged ABIs before uploading. On-device compilation is ARM64-only; x86_64 uses the matching Windows builder's dual-ABI game ZIP; the importer selects the library for the app's process ABI. Do not replace older published APKs to add an ABI; publish a new version.
- **The Windows builder follows the APK version.** `tools/android-builder/package.ps1` reads `versionName` from `android/build.gradle` and creates `SCDA-Android-<version>-Windows-Builder.zip`; the GUI shows that version. Build both from the same commit and publish them together under the same version. Every game ZIP carries `scda-version.txt`; the app refuses a prebuilt ZIP from another builder version (ZIPs from older unversioned builders are still accepted).
- Creating a tag does not build, install, upload or approve binaries. Publishing and stable promotion are separate, manually approved actions. No automatic publishing on pushes/tags is configured.

## Gates

### Testing

Before uploading an APK: successful production build containing both ARM64 and x86_64 runtimes, host tests, APK identity/content inspection (including both ABIs and absence of embedded game code), basic launch check, signing validation and distribution-license/source requirements. Use the isolated setup app for destructive import/storage experiments; test production-package upgrades separately without clearing user data. Notes must distinguish isolated-app results from production APK validation.

A **source-only prerelease checkpoint** is allowed while update-certificate validation or distribution gates remain unresolved. Label it explicitly: no installable build, no claim of binary-release readiness. GitHub source archives must not contain private game files or generated code.

### RC

Complete the following on the actual signed release candidate; features are frozen. Any code, dependency, packaging or signing change requires another RC and relevant retests.

- [ ] Fresh direct ISO/XISO import and automatic compilation/launch.
- [ ] Archive import (ZIP, 7z, RAR/RAR5, TAR/compressed TAR) and extracted-folder picker; disclose untested variants.
- [ ] Windows-builder prebuilt ZIP from the same version imports; a ZIP from another builder version is refused with a clear message.
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

Source-only prerelease. No APK or Windows-builder binary was attached. Its immutable tag remains unchanged.

## Published testing version: 0.1.0-testing.2

The debug-only signing policy was adopted after `v0.1.0-testing.1` was published. Its immutable tag stays unchanged. `v0.1.0-testing.2` publishes the production ARM64 APK built from the tested source commit, its SHA-256 checksum, and corresponding compiler/toolchain source and build materials. Android source version is `0.1.0-testing.2` / code 4. The same debug certificate is retained for compatible updates.

The phone workflow—install, select an ISO, on-device compilation, automatic launch and gameplay—was user-confirmed on Redmi Note 13 Pro 5G / Adreno 710. The maintainer confirms the bundled compiler/toolchain corresponding-source and licensing obligations are complete. Production-package upgrade-data preservation, low-storage stress, background/resume checks, Mali coverage and 16-KiB page-device testing remain open; this is not an RC or stable release.

## Published testing version: 0.1.0-testing.3

Android `0.1.0-testing.3` / code 5, same debug certificate. First release with the versioned Windows builder (`SCDA-Android-0.1.0-testing.3-Windows-Builder.zip`), the launcher screen and the on-request GitHub update check. Assets: APK, builder, corresponding source (toolchain libraries unchanged from testing.2) and SHA-256 sums. Phone upgrade in place, launcher, update check and game start were confirmed on the Adreno 710 phone; the full builder smoke test passed. Same open items as testing.2.

## Published testing version: 0.1.0-testing.4

Android `0.1.0-testing.4` / code 6, same debug certificate. Adds on-screen touch controls (shown while no controller is connected; CONTROLLER page: TOUCH CONTROLS AUTOMATIC/ON/OFF and opacity) and the app icon. Assets: APK, builder, corresponding source (toolchain libraries unchanged from testing.3) and SHA-256 sums. Touch controls and the icon were confirmed on the Adreno 710 phone; the full builder smoke test passed. Same open items as testing.2.

## Published testing version: 0.1.0-testing.5

Android `0.1.0-testing.5` / code 7, same debug certificate. Adds experimental Android 10 / API 29 support, including compiler/import-tool targets and a linked game-code unwinder, and an automatic shader-compiler workaround limited to ARM Mali-G51 driver `0x04800000` (r18p0). Other GPUs/drivers retain the original rendering path. Full first-run tablet compilation, game launch, gameplay and good-looking visuals were user-confirmed on Huawei AGS3K / Mali-G51; the same experimental build was user-tested on Android 16 / Adreno 710 with no apparent regressions. Assets: APK, matching Windows builder, corresponding source/notices and SHA-256 sums. Extended stability, broader import formats, other Mali drivers and 16-KiB devices remain open; this is not an RC or stable release.

## Published testing version: 0.1.0-testing.6

Android `0.1.0-testing.6` / code 8, same debug certificate. The release APK includes ARM64 and x86_64 runtimes. The matching Windows builder compiles both ABIs into one private game ZIP; import selects the device's library and normalizes it to the existing installed layout, retaining legacy single-library ZIP support. On-device compilation remains ARM64-only. Android 10 and the driver-specific Mali-G51 workaround remain experimental. Assets: dual-ABI APK, matching builder, corresponding source and SHA-256 sums. testing.5 assets remain unchanged.

For local build/test commands see `README.md`, `android/tests/README.md` and `tools/on-device-setup/README.md`.
