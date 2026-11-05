# Splinter Cell: Double Agent — Android

Unofficial **Android ARM64 port** of the original Xbox game using static recompilation, Vulkan and SDL3. **Campaign and controller only**—no PC gameplay, touch/mouse/keyboard controls, co-op, versus, networking or voice.

> No game assets or recompiled game code are included. You need your own legally acquired **original Xbox copy**, not the PC or Xbox 360 version. Never share your game ZIP, XBE, generated sources, objects or `libscda_code.so`.

## Requirements and status

- **Phone:** Android 11+ (API 30), ARM64, Vulkan 1.1 and an SDL-compatible controller.
- **Builder:** Windows 10/11 x64, about 1 GB for tools/build files, plus space for the game and output ZIP. ISO input also needs temporary extraction space.
- **Game:** A supported, unmodified Xbox ISO or extracted folder containing `default.xbe`, `Default/` and `Media/`.
- **Tested:** Campaign, controller/settings and retail ISO → ZIP → Android import on Xiaomi Redmi Note 13 Pro 5G / Adreno 710. **Mali remains untested.**
- **Validated game build:** original Xbox USA retail ISO, `Tom Clancy's Splinter Cell - Double Agent (USA).iso`; its extracted `default.xbe` SHA-1 is `91290baf93c2f6ae735a3106c48734665c826d84`.

## Setup

1. Extract the complete builder package and run **`SCDA-Android-ARM64-Builder.exe`**. Keep its companion files together.
2. Select **Folder...** or **ISO...**, choose an output ZIP with **Save as...**, then click **Build game ZIP**. The source ISO is not modified; completed compilations can be reused after a failure.
3. Copy the resulting private `SCDA-game.zip` to the phone's **Downloads** folder.
4. Install the matching APK, launch SCDA, select **Import game ZIP**, and choose the ZIP using Android's file picker. Keep setup open during extraction; cancelled/failed imports can be retried.
5. The game starts automatically. Connect your controller and use **Options > Recomp Settings** or **Options > Controller**. Start with the default **50% render resolution**.

Use the builder and APK from the same revision. The builder needs no separate developer tools or runtime installation. Android needs space for the extracted game (roughly 3.5 GB) **in addition to the ZIP**; delete the ZIP after a successful import if desired. No root or manual access to `Android/data` is needed.

**Updates:** Install compatible APK updates over the existing app to preserve the imported game, saves and settings. Importing over an existing game folder is refused. **Uninstalling or clearing app data deletes saves/settings too.**

[Full builder/import instructions](tools/android-builder/README.md)

## Features

Render scale, FOV, brightness/contrast, FPS cap/counter, FMV scaling, anisotropic filtering, shadows, controller rebinding, sensitivity, inversion, deadzone, rumble, controller-dependent gyro, language, intro skipping and audio output settings.

Desktop-only rendering features and overlays are not supported. See [port notes](ANDROID_PORT_NOTES.md) for technical details and remaining limitations.

## Build locally

Requires JDK 21, Android SDK platform 36, NDK `29.0.14206865`, CMake `3.31.6`, and `ANDROID_HOME` pointing to the SDK. From a clone of this repository:

1. Put your extracted campaign files in `assets/`.
2. Build the Windows recompiler using the [maintainer instructions](tools/android-builder/README.md#maintainers).
3. Generate the headers/code required by the Android host, then build:

```bat
out\android-recompiler\compiler\xbe_recomp.exe config\scda_v2.toml
android\gradlew.bat -p android assembleDebug -Pabi=arm64-v8a
```

Output: `android/build/outputs/apk/debug/scda-debug.apk`, without embedded game code. For private development only, `-PdevCode` embeds it—**never distribute that APK**. Use `-Pabi=x86_64` for emulator testing.

[Builder packaging](tools/android-builder/README.md#maintainers) · [Tests](android/tests/README.md) · [Port notes](ANDROID_PORT_NOTES.md)

## Credits and license

Based on [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) and [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482. Project code is [MIT licensed](LICENSE); third-party components retain their licenses.

ISO support uses [extract-xiso](https://github.com/xboxdev/extract-xiso). This product includes software developed by in <in@fishtank.com>. Its license is included in the builder package.
