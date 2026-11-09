# Splinter Cell: Double Agent — Android

Unofficial **original Xbox → Android ARM64 / x86_64** port using static recompilation, Vulkan and SDL3. **Campaign only; controller recommended, touch controls available.** No co-op, versus or networking; the main menu lists only Solo, Credits and Exit.

**Bring your own legally acquired Xbox game.** No game assets or recompiled game code are included. PC/Xbox 360 versions are not supported.

**Want to play on PC?** Visit the upstream [SCDA-XBERecomp project by TGP482](https://github.com/TGP482/SCDA-XBERecomp) for the PC version of this Xbox recompilation. Thank you to TGP482 and the upstream contributors for the original project and the work that made this Android port possible!

## Requirements

- Android 10+, ARM64 or x86_64, and Vulkan 1.1. **Android 10 support is experimental.** Use touch controls or an SDL-compatible controller.
- Your own supported original-Xbox game. See the [supported build and verification hashes in the wiki](https://github.com/mogeldev/SCDA-XBERecomp-android/wiki/Setup#supported-game).
- Roughly **3.5 GB for assets**, plus input/extraction space and at least **512 MiB for compilation**.

## Setup

On-device compilation runs on **ARM64 and x86_64** from 0.2.0-testing.3 (earlier releases: ARM64 only).


1. Install the APK (includes ARM64 and x86_64) and open SCDA.
2. Import your supported game and keep setup open for on-device compilation (ARM64 and x86_64). The Windows builder is retired from 0.3.0-testing.1.
3. Play with touch controls or a controller. Start at **50% render resolution**.

[Full setup/import/update guide](https://github.com/mogeldev/SCDA-XBERecomp-android/wiki/Setup) · [Controls and touch settings](https://github.com/mogeldev/SCDA-XBERecomp-android/wiki/Controls) · [Troubleshooting](https://github.com/mogeldev/SCDA-XBERecomp-android/wiki/Troubleshooting)

**Install updates over the existing app with the matching signing certificate. Uninstalling or clearing app data deletes saves/settings.**

## Status

[**0.3.0-testing.2**](https://github.com/mogeldev/SCDA-XBERecomp-android/releases/tag/v0.3.0-testing.2) — testing prerelease, not stable. **Export error logs (ZIP)** from the launcher: save game logs, the last setup error and Vulkan diagnostics through the file picker. No automatic upload; review logs for file paths before sharing. **New anti-aliasing option (SMAA default, FXAA, off)**, 4x anisotropic filtering by default, a quiet update check at app start, and a main menu without co-op/versus. On-device compilation on ARM64 and x86_64; **the Windows builder is retired** and no longer published. In-game-style launcher, optional bundled Turnip driver with supported-Adreno detection, and a manual **Share device report**. Manufacturer driver remains default. The APK is about 219 MB because it carries compiler tools for both architectures.

Gameplay confirmed on Adreno 710 and experimentally on Mali-G51; x86_64 emulator tested through the title screen. See the wiki for [tested devices, level-transition results and known limitations](https://github.com/mogeldev/SCDA-XBERecomp-android/wiki/Compatibility). Other Mali drivers, 16-KiB devices and extended stability remain untested.

[Build](android/BUILDING.md) · [Tests](android/tests/README.md) · [Validation](tools/on-device-setup/README.md) · [Port notes](ANDROID_PORT_NOTES.md) · [Release policy](RELEASING.md)

## Credits

Based on [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) and [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482. Project: [MIT](LICENSE); dependencies retain their [licenses](android/TOOLCHAIN-NOTICES.md).

ISO support uses [extract-xiso](https://github.com/xboxdev/extract-xiso). This product includes software developed by in <in@fishtank.com>.
