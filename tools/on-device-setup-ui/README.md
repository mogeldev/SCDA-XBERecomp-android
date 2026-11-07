# Native setup UI preview

Separate package `io.github.mogeldev.scda.setuppreview`, using the production `SetupView` directly. No game content, native libraries, compiler tools or file access. Import buttons simulate progress/resume only. Do not count this as an importer/picker or gameplay test.

```text
android/gradlew.bat -p tools/on-device-setup-ui assembleDebug
adb -s emulator-5554 install -r out/setup-ui/build/outputs/apk/debug/scda-setup-ui-debug.apk
adb -s emulator-5554 shell am start -n io.github.mogeldev.scda.setuppreview/io.github.mogeldev.scda.SetupPreviewActivity
```

Set `ANDROID_HOME` / `ANDROID_SDK_ROOT` to your SDK. Add `--ez prebuilt true` to the activity command for the no-compiler layout. Only target an emulator; do not replace production or modify phone settings.

Design: Third Echelon green, subtle grid, short labels, centered panel at at most 80% of safe width (also capped at 1100dp). Import choices stack for narrow widths/enlarged text. Scroll fallback preserves access at large font sizes/short screens. Native buttons have 48dp minimum targets and visible keyboard/controller focus; status uses a polite accessibility live region. No fake progress percentages in production.

Validated on API 35 emulator at 1920×1080 and 2520×1080, including 130% text at 16:9/320dpi. UI hierarchy checks confirmed 80% bounds, visible 48dp button targets and simulated resume state. All production Java sources compiled against API 36; six Python production wiring checks passed. No full production APK rebuild or physical-device UI/import check in this change. Screenshots/logs remain ignored under `out/setup-ui/`.
