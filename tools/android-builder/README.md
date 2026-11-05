# SCDA Android ARM64 builder

A Windows WPF GUI builder package that creates the Android game's import ZIP from your own extracted Xbox game. It runs on **Windows 10/11 x64** and produces **Android ARM64** code—not a PC game or Windows ARM64 executable.

## Players

1. Extract the complete builder ZIP into a writable folder and run `SCDA-Android-ARM64-Builder.exe`. Keep the EXE beside the `compiler/`, scripts and `licenses/` folders. No installation or administrator access is needed.
2. Beside **Game source**, click **Folder...** and select the extracted game folder containing `default.xbe`, `Default/` and `Media/`, or click **ISO...** and select an original Xbox XISO image. ISO contents are extracted automatically with the bundled `extract-xiso` tool; the ISO is never modified.
3. Click **Save as...** to choose where to save `SCDA-game.zip`, outside the source game folder.
4. Click **Build game ZIP**. The responsive WPF window shows the current phase, elapsed time, compiler progress and ZIP throughput. You can resize it or turn off **Follow output** while it works. A first build can take several minutes, followed by ZIP compression.
5. Copy the resulting ZIP to **Downloads** on your Android phone.
6. Install the **matching version** of the Android APK, launch SCDA, select **Import game ZIP**, and choose the ZIP using Android's file picker.
7. Keep setup open during extraction. On success, the game starts; later launches skip setup. Connect a controller to play.

No command line, Visual Studio, Android SDK, Java, Python or separate runtime installation is needed on a standard Windows 10/11 installation. The GUI uses Windows' built-in WPF/.NET Framework and Windows PowerShell; compiler tools and scripts are supplied beside the EXE. Folder selection uses the native Windows shell picker; ISO and ZIP selection use WPF file dialogs. No WinForms dependency is used. The executable is unsigned, so Windows may show a reputation warning.

### Storage and retrying

- Allow about **1 GB** for tools, generated code and compiled objects, plus room for your game files and the finished ZIP. ISO input additionally needs space for the complete extracted disc (including any versus data); temporary extraction is removed afterward. Replacing an existing ZIP temporarily requires space for both ZIPs.
- Resumable builds stay in the builder folder's `work/` directory. ISO extraction uses a temporary `iso-...` folder in the builder directory and removes it after the ZIP is created. The **Tools / licenses** button opens the bundled `licenses/` folder.
- If a build fails, fix the reported error and click **Build game ZIP** again. Completed compilations are reused. Builds are serialized so two instances cannot write the same workspace.
- The visible output is a bounded tail so very verbose compiler runs cannot lock up the UI. A complete timestamped copy is written to `build.log`; **Open full log** opens it in Notepad.
- The existing output ZIP is replaced only after a new ZIP has been written successfully. Source game files are read-only inputs.
- Keep the complete builder folder together when moving or copying it. Deleting `work/` discards incremental state; the next build recompiles.
- Android needs space for the extracted game in addition to the ZIP. After a successful import, the ZIP can be deleted from the phone.

### Input and output

Use an unmodified `default.xbe` supported by the matching APK/builder. The builder validates its SHA-1 after folder or ISO extraction. ISO input uses the pinned `extract-xiso` build from [xboxdev/extract-xiso](https://github.com/xboxdev/extract-xiso). Versus, co-op and networking are not supported. Files for versus and any builder README files are not included in the import ZIP.

The GUI creates this layout automatically:

```text
SCDA-game.zip
├── libscda_code.so
└── assets/
    ├── default.xbe
    ├── Default/  (with original files/subfolders)
    └── Media/    (with original files/subfolders)
```

**Keep the ZIP private.** Never distribute your game ZIP, XBE, generated sources, objects or `libscda_code.so`. The builder EXE contains none of those files.

Imported files live under `Android/data/io.github.mogeldev.scda/files/game/`; no manual access to that folder is needed. Failed/cancelled Android imports can be retried from the beginning. Existing installs, saves and settings are not overwritten. Importing over an existing `game/` folder is refused. Clearing app data/uninstalling also deletes saves/settings; back them up first.

## Maintainers

Build `xbe_recomp.exe` on Windows without any game assets (Clang, CMake, Ninja and Visual Studio C++ tools):

```bat
cmake -S . -B out\android-recompiler -G Ninja -DSCDA_RECOMP_ONLY=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out\android-recompiler --target xbe_recomp
```

Set `ANDROID_HOME` to an SDK containing NDK `29.0.14206865` and CMake `3.31.6`, then package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/android-builder/package.ps1 `
  -Recompiler out/android-recompiler/compiler/xbe_recomp.exe -Xbe assets/default.xbe
```

Output: `out/SCDA-Android-ARM64-Builder.zip`. Pair it with an APK built from the same revision. The package contains a small `SCDA-Android-ARM64-Builder.exe`, the pinned `extract-xiso.exe`, compiler tools, build/ZIP scripts, configs, freestanding headers, licenses, the supported XBE hash and an empty host link stub. Packaging compiles the WPF `Builder.cs` with Windows' .NET Framework `csc.exe` and builds extract-xiso from revision `3f5b62cfe68f000b0e3c8a30104973f3a297948e` using Visual Studio 2022 C++ (static runtime). The first packaging run fetches that source. No SDK-style .NET project, NuGet packages or runtime downloads are needed for players.

ISO support uses [extract-xiso](https://github.com/xboxdev/extract-xiso). This product includes software developed by in <in@fishtank.com>. The upstream license is included as `licenses/extract-xiso.txt`.

Internally, the GUI uses the pinned `extract-xiso.exe` for ISO input, then reuses `build.ps1` and `package-game.ps1`. Compiler concurrency leaves one CPU thread available for the desktop; reduce jobs in `build.ps1` if the machine still needs more headroom. The compiler uses NDK Clang, Ninja incremental/parallel builds bounded by free RAM, inline ARM64 atomics (`-mno-outline-atomics`), 16 KB ELF page alignment and unresolved host/runtime imports. The APK supplies `libc++_shared.so` for exceptions across the code/host boundary.

Smoke test (extracts the package to a path with spaces, opens the GUI, checks its bounded-output responsiveness with a 24,000-line flood, builds from both a folder and a generated XISO with Windows-only PATH, checks ISO immutability and failed-extraction cleanup, checks ELF/exports/runtime imports, and verifies no-op/resume builds):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/android-builder/test.ps1 `
  -Package out/SCDA-Android-ARM64-Builder.zip -Xbe assets/default.xbe
```

ZIP packager tests: `android/tests/package-game-test.ps1`. Android importer tests: [android/tests/README.md](../../android/tests/README.md).
