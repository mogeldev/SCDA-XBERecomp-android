# Experimental on-device first-time setup

**Developer experiment, not a distributable release.** New app ID `io.github.mogeldev.scda.localsetup`, label **SCDA Local Setup Test**. Does not update/clear the normal game or the earlier gameplay/probe apps.

The validated compiler/import implementation now lives in `android/src/main/java/` and the production ARM64 launcher enables it too. This separate app remains a safe validation harness: `LocalSetupActivity` adds only the private debug-import entry, while production uses `OnDeviceSetupActivity` without that entry. Both accept existing prebuilt ZIPs; legacy installs still skip setup. The Windows builder is unchanged. Production Gradle builds its own matching runtime and stages tools into `out/android-tools/`, independently of this harness's supplied host APK.

## Workflow

1. Pick a retail Xbox **ISO/XISO**, **ZIP**, **7z**, **RAR/RAR5**, or **TAR** (plain, gzip, bzip2 or xz), or use **Import extracted game folder**. Archives may contain one ISO/XISO or one game folder, optionally inside a wrapper directory; existing `assets/` + `libscda_code.so` ZIPs still work. Game folders require `default.xbe`, `Default/` and `Media/`. No generated code or prebuilt library is required. Only the validated unmodified USA XBE is accepted for local compilation. Other disc-root files such as versus/update executables are discarded after extraction; ambiguous/multiple game roots/images are rejected.
2. Import retains existing path/CRC/size/ABI/cancellation checks and atomic directory publication. Assets alone are not considered playable.
3. Tap **Compile / resume** if compilation was interrupted after import. Compilation starts automatically after a fresh import.
4. Translate, compile all 129 sources using the existing RAM/thermal-aware policy, link against the packaged matching host, run the separate loader/dispatch checker, verify the copy hash and atomically publish the library. Only then launch SDL.

Keep setup foreground. Back cancels import/compilation. Incomplete imports must be selected again; **completed compilation objects** survive cancellation/app termination and are reused only with matching build/XBE fingerprint and content-hash receipts. Translation outputs are hashed too. Changed tools/config/headers/runtime/compiler flags invalidate the cache. A compiler subprocess is killed directly on cancellation; no timing-wrapper orphan is used. Back cancellation/resume and force-stop/reopen recovery have both been validated on the phone.

Build workspace is internal `files/compile/`; imported assets/output are in this test app's external `files/game/`. Saves/settings are elsewhere under this app ID and are not deleted. Existing published game folders are never replaced by the importer. Cache is retained after success for this experiment (no cleanup UI). Requires roughly 3.5 GiB for campaign assets and at least 512 MiB free compilation space. File-picker input is first copied into staging so non-seekable/cloud providers work. ISO import additionally needs space for that ISO copy and the full extracted disc; an archive containing an ISO temporarily needs the archive copy, decompressed ISO and extracted disc together. Source files are read-only. Staging is deleted after success/cancellation/failure and reclaimed on the next attempt after process death. Storage checks are a conservative floor, not a full storage estimator.

## Build (Windows / JDK 21)

```bat
python tools/on-device-setup/build.py --sdk "C:\path\to\Android\Sdk" --host-apk out/SCDA-Android-V1.apk
```

Uses pinned probe compiler packages and the real unchanged translator; configs and freestanding CPU headers are APK assets. **No XBE, generated sources, objects, game library or Windows reference hashes are packaged.** The compiler's C++ runtime SONAME is relocated to `libc++_compil.so`, distinct from the matching game's `libc++_shared.so`; relocation changes only ELF dynamic strings. Host, SDL and launcher come from the trusted matching APK. The compiler version fingerprints all staged binaries/inputs and compilation implementation/policy.

APK: `out/on-device-setup/build/outputs/apk/debug/scda-on-device-setup-debug.apk` (about 80 MiB).

## Device test

```bat
python tools/on-device-setup/run.py --sdk "C:\path\to\Android\Sdk" --serial IP:PORT --allow-phone
```

Creates an ignored private assets-only ZIP, installs only the new app, transfers that ZIP into its internal storage with SHA-256 verification, and invokes the same importer via a debug-only test intent. For the manual flow, install the APK and select your own assets-only ZIP through the system picker. The automated path bypasses only the picker, not extraction or compilation. ZIP transfer uses 64-MiB tar chunks, verifies each chunk before appending, then verifies the entire ZIP. A single multi-GiB Windows ADB stdin stream was observed to truncate, and the final hash check correctly refused to start setup. The runner refuses an existing game directory. Never share private ZIPs or workspace outputs.

Then test Back during compilation → **Compile / resume**, and force-stop **only this app** during compilation → reopen → resume. Logs should show `Resume: N/129 objects reused` and eventually `BUILD_PASS`. Verify launch/gameplay. Do not clear data or uninstall the normal game.

## Current validation / limitations

- APK builds successfully; archive inspection excludes game code/assets.
- Existing importer regression plus new opt-in/prebuilt/unsupported-XBE checks pass (132 synthetic checks; 135 including full private assets-only ZIP extraction).
- 10 plain-Java cache tests pass: incomplete receipt rejection, corruption detection, matching-version reuse, version invalidation and cancellation.
- Probe's 11 Python tests still pass, including targeted dynamic-string relocation.
- **Integrated phone test passed** on the Redmi Note 13 Pro 5G / API 36 after installation approval. The real importer extracted the supported assets-only ZIP, translated on device, compiled all 129 objects, linked, passed the full loader/dispatch check and automatically launched the SDL game to its title screen.
- **Back cancellation passed:** eight verified object receipts survived, all compiler children stopped, and tapping Compile / resume reused all eight.
- **Force-stop recovery passed:** stopped only the setup test app while compilation was active; reopening offered Compile / resume and reused thirteen completed objects. Compilation then completed, with 129 verified objects. No importer re-extraction was needed.
- All **134 integrated translator outputs** matched Windows SHA-256 references (compared privately by the host after the run; no oracle hashes are needed by this app). The final compiled/internal-loaded library SHA-256 was `ba49078da1069f0bebd04ec4e5dac6b973ebd54d6f7f67a50911a7c1e23fb4e4`, identical to the earlier gameplay-validated phone output. Runtime logs confirmed 33,172 mappings, 290 host overrides, zero missing kernel imports, Vulkan rendering and SDL stereo initialization. The debug input ZIP was removed after import.
- Two Python transfer tests cover delayed chunk visibility and rejecting a bad chunk before append. The on-device cancellation/force-stop experiment took about eight minutes from translation to game launch including interruption/testing pauses; this is not a clean setup-time benchmark.
- **The user confirmed the game runs in SCDA Local Setup Test** after this integrated assets-only import/compile/recovery flow. This is a gameplay smoke confirmation, not an exhaustive campaign or long-session pass. Save/load, background/resume and Iceland A → B were previously confirmed in the separate gameplay-test app, not retested here.
- Manual system-picker UX, cancelling during extraction and low-storage stress remain unvalidated.
- ARM64/API 36 build only. No foreground service/background compilation; keep setup open. No production rollout, overall compiler licensing/distribution approval or low-storage stress testing.

## ISO / archive import implementation

`extract-xiso` is built from the same pinned revision as the Windows builder (`3f5b62cfe68f000b0e3c8a30104973f3a297948e`), with an extraction-only entry point and targeted guards for invalid names/extents, cycles/entry count, depth, duplicate output files, truncated data and storage reserve. Source-image rewrite/create options are not exposed. Archive readers use pinned libarchive 3.8.9, liblzma/XZ 5.8.2, bzip2 1.0.8 and Android system zlib; no external filter commands. Both tools are installed PIEs in the APK native-library directory, not executables extracted into writable storage. The APK includes license texts and setup exposes them.

This product includes software developed by in <in@fishtank.com>. Archive support includes libarchive, XZ Utils and bzip2; see packaged licenses. This does not resolve the separate compiler-tool distribution review.

Import trust boundaries reject absolute/traversal paths, links/devices, unsafe filenames, case-insensitive collisions, more than 100,000 entries, paths deeper than 64 and more than 16 GiB extracted data. Each extraction subprocess has a 30-minute wall timeout and 15-minute CPU limit; cancellation kills/waits the direct child before cleanup. Encrypted, split/multi-volume archives, recursive archive-in-archive chains and arbitrary disc-image formats (CHD/CSO/CCI) are not supported. Large compression dictionaries can still consume significant memory; Android Scudo prevents a useful simple virtual-address-space cap, so this is not a hardened sandbox for hostile inputs.

Emulator-only import testing is in [`../on-device-import-test/README.md`](../on-device-import-test/README.md). It executes the same staging/validation/extraction path without bundling game content or requiring an x86 game runtime. These formats and local compilation are now wired into the production ARM64 build. Production-package update/picker behavior still requires final device validation; do not install over an existing normal game without permission.

## Production integration validation

The compiler/cache/source-import classes were moved into `android/src/main/java/` (not forked). Production uses `OnDeviceSetupActivity`; the isolated `LocalSetupActivity` now adds only its private ADB hook. ARM64 compiler capability is gated on installed tools; x86-64/no-tools builds fall back to the original prebuilt-ZIP importer. Existing ready/legacy install detection remains unchanged, and a manifest alias retains the old explicit launcher component. Production app ID stays `io.github.mogeldev.scda`; version is 0.2/code 2. The Windows builder is unchanged.

Validated locally: automatic Gradle tool staging and production ARM64 build; dual-ABI development build; production APK identity/launcher, separate compiler/game runtimes, tools/notices and absence of game content/private debug-import hook; isolated setup build; 23 Python checks, 172 Java checks and rerun of all 30 emulator importer cases against the relocated shared classes. The emulator framework/package service disappeared during the first rerun; restarting the disposable emulator allowed the whole matrix to pass. No phone system changes or production-package installation occurred.

Production ARM64 debug APK: `android/build/outputs/apk/debug/scda-debug.apk` (about 101 MiB). This is a build artifact, not a signed public release. The actual production host is built from current source instead of copied from a reference APK; the compilation cache fingerprints its packaged host/SDL/C++/main bytes as well as tool inputs. Public distribution still requires corresponding-source/license compliance for the bundled toolchain (see `android/TOOLCHAIN-NOTICES.md`), final signed-APK/device upgrade validation, manual pickers, storage stress and 16-KiB devices. The normal phone game has not been overwritten.

## Current ISO/archive validation

The emulator import app passed the full retail ISO, both private 7z samples and a direct XISO from the supplied format folder. All 1,317 campaign file hashes matched the private reference for retail ISO and both 7z samples. The expanded synthetic/private-XBE matrix passed 30 cases (25 distinct plus five repeats), including path/link rejection, malformed/truncated ISO, unsupported XBE, encryption rejection and cancellation. Stored/compressed RAR5 reader checks matched 7-Zip. The latest ARM64 setup APK contains both tools/licenses. After the initial `INSTALL_FAILED_USER_RESTRICTED` rejection, the authorized retry installed successfully and the **direct-XISO phone test passed** on the Redmi Note 13 Pro 5G / API 36. See emulator README for archive-specific scope.

Phone result: hash-verified 4,055,040,000-byte XISO input → installed extract-xiso subprocess (`XISO_EXTRACT_PASS`) → supported-XBE validation → fresh translation → all 129 compiled objects → link → full runtime/dispatch validation → atomic publication → automatic SDL title-screen launch. All 134 translator outputs matched Windows SHA-256 references. Both compiled and internal-loaded libraries matched the earlier gameplay-tested output, SHA-256 `ba49078da1069f0bebd04ec4e5dac6b973ebd54d6f7f67a50911a7c1e23fb4e4`. Runtime logs confirmed 33,172 mappings, four continuations, 290 host overrides, zero missing kernel imports, Vulkan rendering and SDL stereo initialization. The private debug input was deleted before compilation and no compiler children remained after completion.

The run initially selected six adaptive workers. From `Resume: 0/129` to `BUILD_PASS` took **381,659 ms (6 min 22 s)**, including link/check/publication but excluding input transfer, copying, extraction and translation. No overall peak-memory or clean total-setup-time benchmark is claimed. Phone campaign-file hashes were not compared (OEM shell/run-as external-storage access restriction); full campaign-file comparison passed separately in the emulator. **The user confirmed gameplay works after this full direct-XISO phone import/build.** This is a gameplay smoke confirmation, not an exhaustive campaign, save/load or long-session retest. Phone archive inputs and manual file/folder pickers remain unvalidated. Only the isolated setup app was installed; no phone settings were changed.

Repeat on a fresh isolated setup installation, with user approval (check the current serial):

```text
python tools/on-device-setup/run.py --sdk SDK --serial SERIAL --allow-phone --source PRIVATE_XISO_OR_ARCHIVE
```

This retains the original runner's per-chunk/full-file verification and existing-game refusal. The debug staging filename remains `test-assets.zip` but contents are detected, not trusted by extension. It bypasses only the picker; the same import/compilation path runs. Keep the Windows builder and prebuilt-ZIP compatibility working.

## Session handoff

The user uninstalled all test builds from the phone after validation. Do not reinstall or touch the normal game without a new request. Those earlier test apps' private device caches/saves were removed. The ISO/XISO phone validation subsequently reinstalled only SCDA Local Setup Test, which now holds its own imported assets/compiler cache; do not delete or replace it without permission.

Local temporary transfer ZIP/tar files, duplicate probe inputs and diagnostic screenshots/raw hang captures were removed during cleanup. Source game assets, release packages, pinned compiler downloads, build caches, concise validation logs and the validated phone-built library remain under their existing paths (private outputs stay ignored under `out/`). The test emulator and Gradle daemons were stopped at that checkpoint. ISO/archive work subsequently restored missing PC SDK components and created a disposable 40-GiB `scda_import_x64` emulator for testing. After production integration, final PC cleanup deleted that disposable `scda_import_x64` AVD (including private test-app data), generated import fixtures, SDK bootstrap archive/unpacked duplicate, and phone diagnostic screenshots. The original `scda_x64` AVD and installed SDK components were retained. Three Gradle daemons were stopped. Production APK, compiler downloads/build caches, source game files, release packages and validation logs under `out/on-device-import/` remain. No phone app/data/settings changes were made during cleanup.

Next session: finish manual system file/folder-picker testing in the isolated setup app, then exercise low-storage handling without filling the user's real phone. Review compiler provenance/licensing/distribution and validate the production-package APK before shipping; Windows compilation remains the supported alternative. Known unrelated issue: mission loading intentionally waits for A but its prompt was not visible in the earlier test.

Run host tests:

```bat
mkdir out\on-device-setup\tests
javac -d out/on-device-setup/tests android/src/main/java/io/github/mogeldev/scda/GameImport.java android/tests/GameImportTest.java android/src/main/java/io/github/mogeldev/scda/CompileCache.java tools/on-device-setup/CompileCacheTest.java
java -cp out/on-device-setup/tests io.github.mogeldev.scda.GameImportTest
java -cp out/on-device-setup/tests io.github.mogeldev.scda.CompileCacheTest
```

Optional full private assets-only ZIP test: append `--assets-only out/on-device-setup/assets-only.zip` to `GameImportTest`.
