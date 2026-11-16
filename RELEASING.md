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
- Promote RC to stable by copying the exact approved APK and checksums. Do not rebuild or edit its embedded RC version name/code just for promotion. Record both GitHub stable version and embedded Android version in the release notes.
- **Debug keys only for Testing, RC and Stable.** Both Gradle debug and release variants explicitly use `signingConfigs.debug`; no separate production signing key is used. A release variant signed with a debug key is not necessarily a debuggable app.
- Reuse and privately back up the same maintainer debug keystore (`~/.android/debug.keystore`) for published updates. Another machine's automatically generated debug key will differ: Android refuses an in-place update signed by a different certificate. Verify the certificate against the previous APK and publish its SHA-256 fingerprint. Never commit/upload the keystore or expose its private key; the standard debug credentials are not a security boundary. Do not recommend uninstalling an existing game just to bypass a signing mismatch, because that deletes app data.
- **Every future release APK must include both `arm64-v8a` and `x86_64` runtime libraries.** Build without `-PdevCode`; never publish private game code/assets. Inspect the packaged ABIs before uploading. Both ABIs also carry the on-device compiler/import tools (from 0.2.0-testing.3; earlier releases compile on ARM64 only). The corresponding-source bundle must cover both pinned toolchain package sets (`tools/on-device-probe/packages-arm64-v8a.json` and `packages.json`). Do not replace older published APKs to add an ABI; publish a new version.
- **The Windows builder is retired from 0.3.0-testing.1.** Releases attach only the APK, corresponding source and SHA-256 sums; do not build or publish `SCDA-Android-<version>-Windows-Builder.zip` again. Players compile on the device (ARM64 and x86_64). Earlier builder assets stay on their releases, marked retired. Builder ZIPs carry `scda-version.txt`; the app refuses a versioned ZIP whose version differs from the app (from 0.3.0 every versioned builder ZIP), unversioned older ZIPs are still accepted.
- Creating a tag does not build, install, upload or approve binaries. Publishing and stable promotion are separate, manually approved actions. No automatic publishing on pushes/tags is configured.

## Gates

### Testing

Before uploading an APK: successful production build containing both ARM64 and x86_64 runtimes, host tests, APK identity/content inspection (including both ABIs and absence of embedded game code), basic launch check, signing validation and distribution-license/source requirements. Use the isolated setup app for destructive import/storage experiments; test production-package upgrades separately without clearing user data. Notes must distinguish isolated-app results from production APK validation.

A **source-only prerelease checkpoint** is allowed while update-certificate validation or distribution gates remain unresolved. Label it explicitly: no installable build, no claim of binary-release readiness. GitHub source archives must not contain private game files or generated code.

### RC

Complete the following on the actual signed release candidate; features are frozen. Any code, dependency, packaging or signing change requires another RC and relevant retests.

- [ ] Fresh direct ISO/XISO import and automatic compilation/launch.
- [ ] Archive import (ZIP, 7z, RAR/RAR5, TAR/compressed TAR) and extracted-folder picker; disclose untested variants.
- [ ] A ZIP from a retired Windows builder is refused with a clear message pointing to on-device compilation.
- [ ] Back cancellation, verified-object resume, force-stop/reopen; no incomplete library publication.
- [ ] Manual picker cancellation/errors and storage-failure cleanup without destroying existing game files.
- [ ] Production-package upgrade preserves game files, saves/settings, launcher entry and signing compatibility.
- [ ] Controller, save/load, background/resume and mission transition checks; publish tested devices/GPUs/API levels.
- [ ] Correct runtime/compiler packaging, no private assets/XBE/generated game code, licenses, corresponding source/build recipes and SHA-256 checksums.
- [ ] Known risks documented, including untested 16-KiB devices, GPUs and campaign areas.

### Stable

- [ ] RC gates passed within the documented supported scope; no known release-blocking regression.
- [ ] Human explicitly approves stable promotion.
- [ ] Stable tag points to approved RC source commit; APK/checksum bytes are unchanged.
- [ ] Publish setup/update instructions, supported game hash, known limitations, device coverage, embedded APK version/code and signing fingerprint.
- [ ] Mark stable Latest; retain earlier stable releases and identify the last known-good release when withdrawing a broken one.

## Release history

| Version | Code | Summary |
|---|---|---|
| 0.1.0-testing.1 | — | Source-only checkpoint |
| 0.1.0-testing.2 | 4 | First APK, debug-only signing, ARM64 |
| 0.1.0-testing.3 | 5 | First versioned Windows builder |
| 0.1.0-testing.4 | 6 | On-screen touch controls |
| 0.1.0-testing.5 | 7 | Experimental Android 10 / API 29 |
| 0.1.0-testing.6 | 8 | ARM64 and x86_64 runtimes in the APK |
| 0.1.0-testing.7 | 9 | Phone gyro aiming |
| 0.2.0-testing.1 | 10 | Optional universal Turnip v4.5 |
| 0.2.0-testing.2 | 11 | Share device report |
| 0.2.0-testing.3 | 12 | On-device compilation on x86_64 |
| 0.3.0-testing.1 | 13 | Anti-aliasing, 4x anisotropic default, update check, Windows builder retired |
| 0.3.0-testing.2 | 14 | Error log ZIP export |
| 0.4.0-testing.1 | 15 | Mali narrow barrier default, GPU SYNC option, row help |
| 0.4.0-testing.2 | 16 | Menu music fix (issue #4). GitHub release removed; tag `v0.4.0-testing.2` stays |
| 1.0.0-rc.1 | 17 | First release candidate |
| 1.0.0 | 17 | Stable: the 1.0.0-rc.1 APK promoted unchanged, tag `v1.0.0` on the RC commit `8c2324b`, debug certificate `1be501578a55867ac0b1fc488d516d33c07d14cfab2b43a69b8ace81e9cb8233`. Approved by the maintainer 2026-11-16 |

GitHub keeps only 1.0.0 and 1.0.0-rc.1 as releases. The earlier versions' tags remain; their GitHub releases were removed.
