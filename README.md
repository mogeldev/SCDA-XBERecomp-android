# Splinter Cell: Double Agent — Android XBE Recomp

An unofficial native **Android port** of the original Xbox version of *Tom Clancy's Splinter Cell: Double Agent*, using static recompilation. The game's x86 code is translated to C++ ahead of time and compiled for Android ARM64. The Android runtime uses Vulkan for graphics and SDL3 for controllers and audio.

**This fork supports Android only. PC gameplay is not supported and will not be maintained.** Windows is used only to prepare the Android game-code library. For the separate PC port, visit the [upstream project](https://github.com/TGP482/SCDA-XBERecomp).

> [!IMPORTANT]
> No game assets or recompiled game code are included. You must supply the files from your own legally acquired copy of the **original Xbox game**, not the PC or Xbox 360 version.

## Current status

- Campaign runs on Android ARM64; tested on a Xiaomi Redmi Note 13 Pro 5G with Adreno 710.
- Controller navigation, settings, live render-scale/shadow changes and settings persistence have been verified on that device.
- Mali support is intended, with CPU decoding for unsupported BC texture formats, but has **not yet been tested on a Mali device**.
- The Windows player builder has been tested end to end, including loading its generated library on the phone.
- APK builds and builder packaging are performed locally. GitHub Actions release packaging is not planned.

See [ANDROID_PORT_NOTES.md](ANDROID_PORT_NOTES.md) for implementation details, known renderer limitations and the device test history.

## Requirements

### To play

- Android 11 or newer (API 30+).
- An ARM64 device with Vulkan 1.1 support. Performance depends on the device and render resolution.
- An SDL-compatible controller; an 8BitDo Ultimate 2C over Bluetooth has been tested.
- Your extracted Xbox game files: `default.xbe`, `Default/` and `Media/`.
- A matching Android APK and Windows player-builder package.

**Controller only:** no touch controls, mouse or keyboard gameplay.

### To prepare the game code

- Windows 10/11 x64 and approximately 1 GB of free space for the builder, plus space for the extracted game and output ZIP. ISO input additionally needs temporary space for the full extracted disc.
- The `SCDA-Android-ARM64-Builder.exe` GUI and its companion builder package; no command line, Visual Studio, Android SDK, Java, Python or separate runtime installation is needed on a standard Windows 10/11 installation.
- An unmodified `default.xbe` matching the version accepted by the builder.

The builder runs on Windows x64 but produces **Android ARM64 code**, not a Windows game executable.

## Installation

### 1. Create your import ZIP

1. Extract the complete builder ZIP, then run **`SCDA-Android-ARM64-Builder.exe`** from that folder. Keep the EXE beside its `compiler/`, scripts and `licenses/` folders. No installation or administrator access is needed.
2. Click **Folder...** to select the extracted Xbox game folder containing `default.xbe`, `Default/` and `Media/`, or **ISO...** to select an original Xbox ISO. The bundled [extract-xiso](https://github.com/xboxdev/extract-xiso) extracts the image without modifying it.
3. Click **Save as...** to choose an output ZIP outside the game folder, then click **Build game ZIP**.
4. Wait for compilation and packaging to finish. The window shows the build log; completed compilations are cached for retrying failed builds.

Use the builder and APK from the same revision. The builder validates the XBE's SHA-1; unsupported game versions need matching recompiler configs and game hooks. Resumable build files stay in the builder folder's `work/` directory; keep the complete extracted folder together. ISO input also needs space for the extracted disc contents, which are removed after the build.

Full instructions: [Windows player builder](tools/android-builder/README.md).

The GUI creates `SCDA-game.zip` with this layout automatically:

```text
SCDA-game.zip
├── libscda_code.so
└── assets/
    ├── default.xbe
    ├── Default/
    │   └── … all original files and subfolders
    └── Media/
        └── … all original files and subfolders
```

Only the campaign files and generated game-code library are included, not versus files or builder tools. This ZIP contains your game files and **must not be shared**.

The builder's ISO support uses the pinned [extract-xiso](https://github.com/xboxdev/extract-xiso) tool and never modifies the source ISO. This product includes software developed by in <in@fishtank.com>. Its license is included in the builder package.

### 2. Import on Android

1. Copy the ZIP to a normal phone folder, such as **Downloads**.
2. Install the matching APK and launch SCDA.
3. Select **Import game ZIP** and choose your ZIP in Android's system file picker.
4. Keep the setup screen open while extraction runs. Progress shows the extracted size and current file. Press Back to cancel; an interrupted or failed import can be retried from the beginning.
5. SCDA starts automatically after a successful import. Later launches skip setup.

No root, broad storage permission, or manual access to `Android/data` is required. Leave enough free space for the extracted game in addition to the ZIP (campaign files are around 3.5 GB, depending on the dump). You can delete the ZIP after a successful import.

Imported files live in `Android/data/io.github.mogeldev.scda/files/game/`. Saves and settings remain outside that folder and are not overwritten by the importer. Existing USB/ADB installs using `files/assets/` and `files/libscda_code.so` still work without reimporting.

To keep the first version simple, importing over an existing `game/` folder is refused. Uninstalling or clearing app data removes the imported game **and saves/settings**; back up anything you need first.

### 3. Play

Connect your controller and launch SCDA. Open **Options > Recomp Settings** or **Options > Controller** to adjust settings. Settings are saved to `files/scda.toml`; saves and shader caches also stay in the app's files directory.

Start with the default **50% render resolution**. Higher resolutions can substantially reduce performance on phone GPUs.

## Android features and scope

- **Display:** render resolution, field of view, brightness, contrast, frame-rate cap, FPS counter and FMV scaling.
- **Graphics:** Vulkan renderer, anisotropic filtering, shadow resolution and shadow distance, retaining the Xbox Shader Model 1.1 look.
- **Controller:** button rebinding, sensitivity, inversion, deadzone, rumble and gyro settings. Gyro availability depends on the controller.
- **General:** skip intros, language selection, automatic/mono/stereo audio output and background pausing.
- **Campaign only:** no co-op, versus, online/LAN play or voice support.

Desktop-only features are not supported: the F10 overlay, window/monitor selection, mouse/keyboard input, Shader Model 3, HDR, ambient occlusion, SMAA/MSAA, texture dumping/replacement, custom cursor and boot splash.

## Building locally

Clone this fork:

```bat
git clone https://github.com/mogeldev/SCDA-XBERecomp-android.git
cd SCDA-XBERecomp-android
```

### Generate the game headers and code

Place your extracted game files in the repository's `assets/` folder. Build the Windows recompiler using the [maintainer instructions](tools/android-builder/README.md#maintainers), then run:

```bat
out\android-recompiler\compiler\xbe_recomp.exe config\scda_v2.toml
```

This generates `generated/game/`, which the Android host build currently requires. `sc4_adv.xbe` and the versus assets are not needed for Android.

### Build the APK

Install a compatible JDK and Android SDK, and set `ANDROID_HOME` to the SDK directory. The project uses:

- Android platform 36
- NDK `29.0.14206865`
- CMake `3.31.6`
- The included Gradle wrapper (JDK 21 has been tested)

From the repository root, build an ARM64 debug APK **without game code embedded**:

```bat
android\gradlew.bat -p android assembleDebug -Pabi=arm64-v8a
```

Output: `android/build/outputs/apk/debug/scda-debug.apk`. Supply `libscda_code.so` in your import ZIP using the player builder.

For local development only, add `-PdevCode` to compile and embed the generated game code in the APK. **Do not distribute an APK built with `-PdevCode`.** Android x86_64 emulator builds are available with `-Pabi=x86_64`; this is a development target, not PC gameplay support.

For local builder packaging and smoke tests, see [tools/android-builder/README.md](tools/android-builder/README.md#maintainers). No CI workflow is required.

## Credits and license

Based on [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) and [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482. This Android fork builds on their static recompilation and game-specific work.

Project code is distributed under the [MIT license](LICENSE). Third-party components retain their own licenses. Never distribute the game's assets, XBE, generated sources, objects or `libscda_code.so`.
