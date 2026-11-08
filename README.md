# Splinter Cell: Double Agent — Android

Unofficial **original Xbox → Android ARM64 / x86_64** port using static recompilation, Vulkan and SDL3. **Campaign only; controller recommended, touch controls available.** No co-op, versus or networking.

**Bring your own legally acquired Xbox game.** No game assets or recompiled game code are included. PC/Xbox 360 versions are not supported.

**Want to play on PC?** Visit the upstream [SCDA-XBERecomp project by TGP482](https://github.com/TGP482/SCDA-XBERecomp) for the PC version of this Xbox recompilation. Thank you to TGP482 and the upstream contributors for the original project and the work that made this Android port possible!

## Requirements

- Android 10+, ARM64 or x86_64, and Vulkan 1.1. **Android 10 support is experimental.** Use touch controls or an SDL-compatible controller.
- **Supported game build:** Redump-verified original Xbox release **Tom Clancy's Splinter Cell - Double Agent (USA, Europe) (En,Fr,De,Es,It)**. Redump ISO checksums: CRC-32 `DF53C218`, MD5 `06161188422B4D2EAB54D30F3158E0AA`, SHA-1 `83D68B38A8514B6D2580EA0F57487CD575EDD4B7`. A compatible XISO made from this disc is also supported. The extracted `default.xbe` must have SHA-1 `91290baf93c2f6ae735a3106c48734665c826d84`; provide it with `Default/` and `Media/`. Other builds, the PC version and Xbox 360 version are unsupported.
- Roughly **3.5 GB for assets**, plus input/extraction space and at least **512 MiB for compilation**.

## Setup

1. Install the APK (includes ARM64 and x86_64) and open SCDA.
2. **ARM64:** import **ISO/XISO, ZIP, 7z, RAR, TAR**, or an extracted game folder. **x86_64:** import a game ZIP from the matching [Windows builder](tools/android-builder/README.md); it includes both architectures, and the app selects the right library. On-device compilation is ARM64-only. Archives may contain one ISO or game folder. Encrypted/split archives are unsupported.
3. Keep setup open while it compiles on your phone—about **6–8 minutes for compilation** on the tested device, plus import time. Back cancels; **Compile / resume** retains verified work.
4. Play with touch controls or an SDL-compatible controller. Start at **50% render resolution**.

Touch controls appear automatically when no controller is connected. In **Recomp Settings → CONTROLLER → GENERAL**, set **TOUCH CONTROLS** to AUTOMATIC / ON / OFF and adjust **TOUCH CONTROLS OPACITY** (10–100%, default 50%). Button positions and sizes are fixed.

The app opens as a launcher (Play, Check for updates). **Check for updates** asks GitHub only when tapped and opens the newest release page (pre-releases included); nothing is downloaded or installed automatically. The Windows builder has the same button. Windows is optional on ARM64; x86_64 needs a prebuilt game ZIP from the [Windows builder](tools/android-builder/README.md), which produces compatible dual-ABI ZIPs; use the builder of the same version as the app. **Uninstalling or clearing app data deletes saves/settings.** Updates require a matching signing certificate.

## Status

[**0.1.0-testing.6**](https://github.com/mogeldev/SCDA-XBERecomp-android/releases/tag/v0.1.0-testing.6) — testing prerelease, not stable. APK and Windows builder support ARM64 + x86_64.

- **Adreno 710 / Android 16:** compilation and gameplay confirmed; level 1 → 2 works repeatedly in the user's current build.
- **Mali-G51 / Android 10 (experimental):** compilation and gameplay confirmed, with an automatic workaround for the tested driver.
- **x86_64 emulator:** builder-ZIP import and title screen confirmed; campaign gameplay unverified.

Other Mali drivers, 16-KiB devices and extended stability remain untested. Detailed coverage and limitations: [port notes](ANDROID_PORT_NOTES.md).

[Build](android/BUILDING.md) · [Tests](android/tests/README.md) · [Validation](tools/on-device-setup/README.md) · [Port notes](ANDROID_PORT_NOTES.md) · [Release policy](RELEASING.md)

## Credits

Based on [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) and [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482. Project: [MIT](LICENSE); dependencies retain their [licenses](android/TOOLCHAIN-NOTICES.md).

ISO support uses [extract-xiso](https://github.com/xboxdev/extract-xiso). This product includes software developed by in <in@fishtank.com>.
