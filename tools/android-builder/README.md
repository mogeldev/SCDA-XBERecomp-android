# SCDA Android ARM64 player builder

For **Windows 10/11 x64**, producing game code for **Android ARM64**, not Windows ARM64.
Requires about 1 GB free disk space. No Visual Studio, Android SDK, Java or Python is needed by players.

1. Extract the entire builder zip into a writable folder. Do not run inside the zip.
2. Put `default.xbe` from your legally owned, extracted Xbox game in `assets/`.
   Alternatively, drag `default.xbe` onto `build.bat`.
3. Double-click `build.bat`. The first build can take several minutes; slower PCs take longer.
   Completed compilations are kept in `work/`, so running it again resumes an interrupted build.
4. Create a ZIP with the generated library and your extracted campaign files. The included `package-game.ps1` can create it:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File package-game.ps1
   ```

   It expects `assets/` beside `libscda_code.so` and writes `SCDA-game.zip`. Use `-Assets`, `-Library` and `-Output` to choose other paths.

   ```text
   SCDA-game.zip
   ├── libscda_code.so
   └── assets/
       ├── default.xbe
       ├── Default/  (with all original files/subfolders)
       └── Media/    (with all original files/subfolders)
   ```

   ZIP the items above, not their parent folder. Use a standard unencrypted ZIP (ZIP64 supported).
   Preserve these names. Do not include the builder's `assets/README.txt`, tools, versus files or extra folders.
5. Copy this private ZIP to **Downloads** on the phone. Install the **matching version** of the Android APK.
6. Launch SCDA, select **Import game ZIP**, and pick it using Android's file picker.
7. Keep setup open during extraction. On success, the game starts; later launches skip setup. Connect a controller to play.

The importer needs space for the extracted game in addition to the ZIP. After success, the ZIP can be deleted.
Failed/cancelled imports can be retried from the beginning. Existing installs, saves and settings are not overwritten.
Imported files live under `Android/data/io.github.mogeldev.scda/files/game/`; no manual access to that folder is needed.
Importing over an existing `game/` folder is refused. Clearing app data/uninstalling also deletes saves/settings; back them up first.

The previous USB/ADB layout remains supported for existing installs:

```bat
adb shell am force-stop io.github.mogeldev.scda
adb push libscda_code.so /sdcard/Android/data/io.github.mogeldev.scda/files/
adb push assets/. /sdcard/Android/data/io.github.mogeldev.scda/files/assets/
```

Do not copy `compiler/libscda_host.so` to the phone: it is only an empty link stub. The real host is in the APK.
The builder accepts only the XBE SHA-1 recorded by its release; other game versions need matching configs/hooks.
ISO input, versus, co-op and networking are not supported by this Android port.
Never distribute your game ZIP, XBE, generated sources, objects or `libscda_code.so`.

## Maintainers

Build `xbe_recomp.exe` on Windows without any game assets (Clang, CMake, Ninja and the Visual Studio C++ tools):

```bat
cmake -S . -B out\android-recompiler -G Ninja -DSCDA_RECOMP_ONLY=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out\android-recompiler --target xbe_recomp
```

Set `ANDROID_HOME` to an SDK containing NDK `29.0.14206865` and CMake `3.31.6`, then package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/android-builder/package.ps1 `
  -Recompiler out/android-recompiler/compiler/xbe_recomp.exe -Xbe assets/default.xbe
```

Output: `out/SCDA-Android-ARM64-Builder.zip`. Pair it with an APK built from the same revision.
The zip contains compiler tools, configs, freestanding headers, licenses, the supported XBE hash and an empty
host link stub. It contains **no XBE, game assets, generated game code or APK**.

The builder uses NDK Clang, Ninja dependency tracking, parallel jobs bounded by free RAM, inline ARM64 atomics
(`-mno-outline-atomics`), ELF shared libraries with 16 KB page alignment and unresolved host/runtime imports.
It links no private C++ runtime: the APK supplies `libc++_shared.so` for exceptions across the code/host boundary.
To cap compile concurrency: `build.bat -Jobs 4`.

Smoke test (extracts to a path with spaces, checks rejection, ARM64 ELF, exports, runtime imports, and a no-op rebuild):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/android-builder/test.ps1 `
  -Package out/SCDA-Android-ARM64-Builder.zip -Xbe assets/default.xbe
```
