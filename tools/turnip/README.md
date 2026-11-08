# Bundled Turnip experiment

Introduced in `0.2.0-testing.1` (prerelease). The app stays on the manufacturer driver by default.

- **PERFORMANCE → VULKAN DRIVER** selects Manufacturer / Turnip (experimental).
- The launcher also has a **Vulkan driver** selector, so a driver startup failure does not require entering the game or deleting settings.
- **Completely close/force-stop the app and reopen after changing the driver.** Selecting a driver never switches a live Vulkan instance.
- **Auto-detection:** the Adreno chip id is read from KGSL (`/dev/kgsl-3d0`, no Vulkan driver loaded) and matched against the A6xx–A8xx entries of Mesa's `freedreno_devices.py` at the bundled revision, with Mesa's own matching rules (`src/android/turnip_detect.h`). Only matching ARM64 devices see the option, in the launcher (via `libscda_gpuprobe.so`) and in PERFORMANCE. If Turnip is still selected on a non-matching device, the selector stays visible so Manufacturer can be chosen; the game ignores the selection there.
- The table `src/android/turnip_supported.h` is generated: `python tools/turnip/gen_supported.py <freedreno_devices.py>` (pinned by SHA-256 to Mesa `02f13375`). Regenerate when the bundled driver changes. Being listed means Mesa knows the GPU, not that this game was tested on it; only Adreno 710 is verified.
- Selection is in internal app storage (`files/turnip-enabled`), separate from game TOML. Both selectors use this file; missing/invalid selection defaults to manufacturer. No network access is needed at runtime.
- Driver and hooks are installed in Android's native library directory, not writable external storage. `useLegacyPackaging` is required by adrenotools.
- Volk dispatches every Vulkan call through the selected loader; SDL supplies a native window without loading a second Vulkan library. Surface creation uses that same dispatch path. The Turnip path verifies `VK_DRIVER_ID_MESA_TURNIP` rather than silently claiming a manufacturer fallback is Turnip.
- Failure to open the custom loader falls back to the manufacturer loader and is logged. Later Vulkan failures require selecting Manufacturer in the launcher and restarting. Native driver crashes cannot be safely recovered in-process.
- Existing system-driver shader/pipeline caches are retained. Turnip uses `shadercache/turnip-v4.5/` and `turnip-v4.5_pipelines.bin`; runtime selection changes do not alter the active cache path.

## Build

Normal dual-ABI Android build; Gradle runs `python tools/turnip/stage.py`. The script downloads the pinned universal v4.5 ZIP, verifies its SHA-256 and ELF architecture, and stages only the expected library under ignored `out/turnip/jniLibs/arm64-v8a/`. A mismatching download fails the build. No binary is committed. CMake fetches pinned volk and ARM64-only adrenotools (with its pinned submodule).

Sources, hashes and licenses: `android/src/main/assets/licenses/Turnip-provenance.txt`. Source archives, hashes, notices and reconstruction instructions are recorded in [SOURCES.md](SOURCES.md) and included in the corresponding-source release attachment. Binary/source provenance is upstream-reported, not an independently reproduced build. Old release source scripts do not include these dependencies; do not use them unchanged. No forced TU_DEBUG/sysmem settings, turbo clocks, auto-downloads, ZIP importer or driver updater are added.

## Validation

- Matching host test `android/tests/turnip-detect-test.cpp` (710/720/722/6xx/740/750/8xx accepted; A5xx/A3xx/unknown rejected); x86_64 emulator hides the launcher option.
- Dual-ABI build, 11 Python tests, renderer host tests and APK inspection passed. Packaged ARM64 driver checksum and hook presence checked; no Turnip binary/hooks on x86_64.
- Android 15 x86_64 emulator: in-place installation, launcher recovery dialog, unavailable Turnip choice and saved Manufacturer selection checked. System Vulkan path started existing game content (~59 fps logged); this is not a visual/campaign regression test.
- **Adreno 710 phone:** user confirmed Turnip, chip detection and switching back. Logs verify chip `0x07010000`, Mesa Turnip driver ID and manufacturer restart. Same stationary scene: manufacturer sample means 62.4 / 62.3 FPS, Turnip 69.3 FPS (~11% gain); periodic samples, not a frame-time/stutter or campaign-wide result. User reported no visible problems.
- The launcher redesign's fresh setup/import/compile/play test was user-confirmed on the phone before the version bump. Other Adreno GPUs, long-session stability, failure recovery and comprehensive lifecycle/visual regression remain open.
