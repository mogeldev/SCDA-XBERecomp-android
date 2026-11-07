# Splinter Cell: Double Agent — Android

Unofficial **original Xbox → Android ARM64** port using static recompilation, Vulkan and SDL3. **Campaign only; controller required.** No touch controls, co-op, versus or networking.

**Bring your own legally acquired Xbox game.** No game assets or recompiled game code are included. PC/Xbox 360 versions are not supported.

## Requirements

- Android 11+, ARM64, Vulkan 1.1 and an SDL-compatible controller.
- **Supported game build:** Redump-verified original Xbox release **Tom Clancy's Splinter Cell - Double Agent (USA, Europe) (En,Fr,De,Es,It)**. Redump ISO checksums: CRC-32 `DF53C218`, MD5 `06161188422B4D2EAB54D30F3158E0AA`, SHA-1 `83D68B38A8514B6D2580EA0F57487CD575EDD4B7`. A compatible XISO made from this disc is also supported. The extracted `default.xbe` must have SHA-1 `91290baf93c2f6ae735a3106c48734665c826d84`; provide it with `Default/` and `Media/`. Other builds, the PC version and Xbox 360 version are unsupported.
- Roughly **3.5 GB for assets**, plus input/extraction space and at least **512 MiB for compilation**.

## Setup

1. Install the ARM64 APK and open SCDA.
2. Import **ISO/XISO, ZIP, 7z, RAR, TAR**, or an extracted game folder. Archives may contain one ISO or game folder. Encrypted/split archives are unsupported.
3. Keep setup open while it compiles on your phone—about **6–8 minutes for compilation** on the tested device, plus import time. Back cancels; **Compile / resume** retains verified work.
4. Play with a controller. Start at **50% render resolution**.

The app opens as a launcher (Play, Check for updates). **Check for updates** asks GitHub only when tapped and opens the newest release page (pre-releases included); nothing is downloaded or installed automatically. The Windows builder has the same button. Windows is optional: the [Windows builder](tools/android-builder/README.md) still produces compatible prebuilt ZIPs; use the builder of the same version as the app. **Uninstalling or clearing app data deletes saves/settings.** Updates require a matching signing certificate.

## Status

**Testing → RC → Stable**, starting at **0.1.0**; debug signing only. The [current prerelease](https://github.com/mogeldev/SCDA-XBERecomp-android/releases) is **0.1.0-testing.2** with an ARM64 APK download.

ISO → on-device compilation → gameplay confirmed on Redmi Note 13 Pro 5G / Adreno 710. Toolchain corresponding-source and licensing requirements are complete for this testing build. Production-package upgrade/picker tests, low-storage stress and 16-KiB device coverage remain open; Mali is untested.

[Build](android/BUILDING.md) · [Tests](android/tests/README.md) · [Validation](tools/on-device-setup/README.md) · [Port notes](ANDROID_PORT_NOTES.md) · [Release policy](RELEASING.md)

## Credits

Based on [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) and [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482. Project: [MIT](LICENSE); dependencies retain their [licenses](android/TOOLCHAIN-NOTICES.md).

ISO support uses [extract-xiso](https://github.com/xboxdev/extract-xiso). This product includes software developed by in <in@fishtank.com>.
