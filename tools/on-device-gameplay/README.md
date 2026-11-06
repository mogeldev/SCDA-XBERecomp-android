# Isolated phone-built gameplay test

Developer-only companion to `tools/on-device-probe/`. Reuses the production Java activities/manifest and the **matching existing APK's** ARM64 host, SDL, C++ runtime and launcher. It does not rebuild/change the host or importer.

- App ID: `io.github.mogeldev.scda.phonebuilt`
- Launcher label: **SCDA Phone-built Test**
- Separate game data, settings, logs and saves under this app ID.
- No generated code or game assets in the APK. Never distribute this experimental APK or its private inputs.
- Does not update, force-stop, clear, read or write the installed production game app.

## Run (Windows, JDK 21 / Android SDK 36)

First complete the compiler probe full build/load validation. Use the same trusted host APK for both steps:

```bat
python tools/on-device-gameplay/run.py --sdk "C:\path\to\Android\Sdk" --serial IP:PORT --allow-phone --host-apk out/SCDA-Android-V1.apk --code-library out/on-device-probe/libscda_code-phone.so
```

The runner installs only the separate test APK, opens setup briefly to create its storage directory, stops **only the test app**, copies `assets/default.xbe`, `assets/Default/`, `assets/Media/` and the phone-built library into a staging folder, validates every SHA-256, atomically renames staging to `game/`, then launches the existing setup/game activities. Requires approximately 3.5 GiB additional phone storage plus a 512-MiB reserve. Saves/settings are not copied from the existing game. Optional `--assets PATH` selects the local campaign assets.

The raw binary transfer uses `adb exec-in`. On the tested OEM, `run-as` cannot access external app storage; ADB shell writes directly to **only the test app's app-specific directory**, never a shared Downloads/temp folder. The production loader then copies code into the test app's internal storage before executing it. A temporary uncompressed archive is kept under ignored `out/on-device-gameplay/` and deleted after successful publication. Failure may leave staging/archive files; do not share them.

Existing `game/` is never overwritten. To launch again, tap **SCDA Phone-built Test**. To start fresh, uninstall **only** `io.github.mogeldev.scda.phonebuilt`; this deletes its own test saves/data. Never uninstall/clear the production app for this test.

Outputs:

- APK: `out/on-device-gameplay/build/outputs/apk/debug/scda-phone-built-test-debug.apk`
- Game logs: `/sdcard/Android/data/io.github.mogeldev.scda.phonebuilt/files/logs/`
- Runtime diagnostics: `adb -s SERIAL logcat -s scda SDL AndroidRuntime`

## Checks

```bat
python -m unittest discover -s tools/on-device-gameplay -p "test_*.py" -v
```

Tests cover ELF validation, selecting only matching runtime libraries, campaign filtering / XBE validation / missing assets, and the separate app ID with reused production sources. The runner also verifies the APK contains the exact matching runtime binaries and no XBE or generated game code.

## Recorded phone boot

On the Redmi Note 13 Pro 5G / API 36, the separate APK installed successfully. All **1,318 private input files (3.49 GiB)** passed SHA-256 verification. The production SDL launcher loaded the phone-built code from this test app's internal storage; both the imported and internal library hashes matched the compiler output (`ba49078da1069f0bebd04ec4e5dac6b973ebd54d6f7f67a50911a7c1e23fb4e4`). The APK is approximately 6.8 MiB and contains the exact matching runtime binaries, no game code/assets.

Observed: game startup sequence rendering on Vulkan, SDL stereo audio initialization, 33,172 dispatch functions, zero missing kernel imports, and an 8BitDo controller detected. The app remained running during observation. These are boot checks, not a measured gameplay-performance pass. Logs also contain failed voice-input opens because the existing app lacks `RECORD_AUDIO`, plus some unsuccessful intermediate asset-path lookups; neither was changed in this compiler experiment. Voice chat is not validated.

## User-confirmed gameplay checks

Using the phone-built library in the separate test app, the user confirmed:

- In-mission gameplay works.
- Save and load work.
- Backgrounding the app and resuming work.
- The Iceland A → Iceland B transition continues successfully after pressing A.

The transition initially appeared hung on its loading picture. Logs showed `loading: mapname=x01_iceland_b.unr?... , waiting for a key`, with no subsequent input acknowledgment. The existing host deliberately calls `input::WaitForPress()` before mission relaunch (`src/game/loading.cpp`). The user confirmed that pressing A continued the game. This was not a confirmed deadlock or on-device compiler failure. **Known UI issue: the captured loading picture had no visible “Press Any Key” prompt**, making the intentional wait look frozen; the cause of the missing prompt is not yet diagnosed.

These are user-confirmed smoke checks on one phone, not exhaustive campaign, performance, long-session or multi-device validation. Production installation, saves and importer remain untouched.
