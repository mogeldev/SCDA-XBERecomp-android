# Experimental on-device first-time setup

**Developer experiment, not a distributable release.** New app ID `io.github.mogeldev.scda.localsetup`, label **SCDA Local Setup Test**. Does not update/clear the normal game or the earlier gameplay/probe apps.

Reuses `android/` setup/importer/SDL sources with an opt-in compilation subclass. The normal setup does **not** opt in and still requires the matching Windows-builder library. Both modes accept the existing prebuilt ZIP. The Windows builder is unchanged.

## Workflow

1. Pick a ZIP whose root contains `assets/default.xbe`, `assets/Default/` and `assets/Media/`. No generated code or prebuilt library is required. Only the validated unmodified USA XBE is accepted for local compilation.
2. Import retains existing path/CRC/size/ABI/cancellation checks and atomic directory publication. Assets alone are not considered playable.
3. Tap **Compile / resume** if compilation was interrupted after import. Compilation starts automatically after a fresh import.
4. Translate, compile all 129 sources using the existing RAM/thermal-aware policy, link against the packaged matching host, run the separate loader/dispatch checker, verify the copy hash and atomically publish the library. Only then launch SDL.

Keep setup foreground. Back cancels import/compilation. Incomplete imports must be selected again; **completed compilation objects** survive cancellation/app termination and are reused only with matching build/XBE fingerprint and content-hash receipts. Translation outputs are hashed too. Changed tools/config/headers/runtime/compiler flags invalidate the cache. A compiler subprocess is killed directly on cancellation; no timing-wrapper orphan is used. Process death recovery is implemented but **not yet validated on device**.

Build workspace is internal `files/compile/`; imported assets/output are in this test app's external `files/game/`. Saves/settings are elsewhere under this app ID and are not deleted. Existing published game folders are never replaced by the importer. Cache is retained after success for this experiment (no cleanup UI). Requires roughly 3.5 GiB for assets, another private ZIP copy during the automated test, and at least 512 MiB free compilation space. Storage checks are a conservative floor, not a full storage estimator.

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

Creates an ignored private assets-only ZIP, installs only the new app, transfers that ZIP into its internal storage with SHA-256 verification, and invokes the same importer via a debug-only test intent. For the manual flow, install the APK and select your own assets-only ZIP through the system picker. The automated path bypasses only the picker, not extraction or compilation. It refuses an existing game directory. Never share private ZIPs or workspace outputs.

Then test Back during compilation → **Compile / resume**, and force-stop **only this app** during compilation → reopen → resume. Logs should show `Resume: N/129 objects reused` and eventually `BUILD_PASS`. Verify launch/gameplay. Do not clear data or uninstall the normal game.

## Current validation / limitations

- APK builds successfully; archive inspection excludes game code/assets.
- Existing importer regression plus new opt-in/prebuilt/unsupported-XBE checks pass (132 synthetic checks; 135 including full private assets-only ZIP extraction).
- 10 plain-Java cache tests pass: incomplete receipt rejection, corruption detection, matching-version reuse, version invalidation and cancellation.
- Probe's 11 Python tests still pass, including targeted dynamic-string relocation.
- Phone installation is currently **blocked** by `INSTALL_FAILED_USER_RESTRICTED: Install canceled by user`. Integrated import/compile/load/gameplay, picker UX, cancellation and process-death recovery remain **untested on Android**. Earlier isolated compiler and gameplay results do not prove this integration.
- ARM64/API 36 build only. No foreground service/background compilation; keep setup open. No ISO extraction, production rollout, licensing/distribution approval or low-storage stress testing.

Run host tests:

```bat
mkdir out\on-device-setup\tests
javac -d out/on-device-setup/tests android/src/main/java/io/github/mogeldev/scda/GameImport.java android/tests/GameImportTest.java tools/on-device-setup/src/main/java/io/github/mogeldev/scda/CompileCache.java tools/on-device-setup/CompileCacheTest.java
java -cp out/on-device-setup/tests io.github.mogeldev.scda.GameImportTest
java -cp out/on-device-setup/tests io.github.mogeldev.scda.CompileCacheTest
```

Optional full private assets-only ZIP test: append `--assets-only out/on-device-setup/assets-only.zip` to `GameImportTest`.
