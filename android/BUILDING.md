# Build the Android APK

Requires JDK 21, Python 3.11+, Android SDK platform 36, NDK `29.0.14206865`, CMake `3.31.6`, network access, and `ANDROID_HOME` pointing to the SDK.

1. Put your own supported extracted campaign files in `assets/` (`default.xbe`, `Default/`, `Media/`). Supported XBE SHA-1: `91290baf93c2f6ae735a3106c48734665c826d84`.
2. Build the Windows recompiler using the [maintainer instructions](../tools/android-builder/README.md#maintainers).
3. From the repository root, generate the private headers/code required by the Android host, then build:

```bat
out\android-recompiler\compiler\xbe_recomp.exe config\scda_v2.toml
android\gradlew.bat -p android assembleDebug -Pabi=arm64-v8a
```

Output: `android/build/outputs/apk/debug/scda-debug.apk`, without embedded game code. All release channels and both Gradle build variants use debug signing keys; retain the same debug keystore for compatible updates. See [release policy](../RELEASING.md).

Gradle automatically runs `tools/on-device-setup/build.py --production --stage-only` to fetch/verify and stage pinned ARM64 compiler/import tools. The runtime is built from current source, not copied from an old APK. Tools and notices are staged under ignored `out/android-tools/`; no private game inputs are staged. After deliberately staging tools yourself, `-PtoolsStaged` skips preparation.

Use `-Pabi=x86_64` for emulator builds, which retain the prebuilt-ZIP path. For private development only, `-PdevCode` embeds generated game code—**never distribute that APK**. Never commit/share game assets, XBE, generated sources/objects or `libscda_code.so`.

The bundled compiler/toolchain corresponding-source and licensing obligations have been completed for this build; see [toolchain notices](TOOLCHAIN-NOTICES.md). The production-package upgrade/manual-picker checks, low-storage stress and 16-KiB device coverage remain outstanding; the isolated phone-tested workflow is not proof of an upgrade pass.

[Tests](tests/README.md) · [Compiler/import validation](../tools/on-device-setup/README.md) · [Windows builder packaging](../tools/android-builder/README.md#maintainers)
