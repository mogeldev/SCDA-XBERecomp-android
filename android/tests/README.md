# Importer and production setup tests

This is a plain-Java test for the path, CRC, ABI, cancellation, storage-preservation and atomic-publish checks:

```bat
mkdir out\import-tests
javac -d out\import-tests android\src\main\java\io\github\mogeldev\scda\GameImport.java android\tests\GameImportTest.java
java -cp out\import-tests io.github.mogeldev.scda.GameImportTest
```

It creates only temporary synthetic files; no game assets are included. To also extract and validate a private full game ZIP, append its path to the `java` command. Temporary extracted files are deleted afterward.

Test the PowerShell ZIP packager (including campaign-only filtering and failure preservation):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File android/tests/package-game-test.ps1
```

## On-device compiler/import integration

The production ARM64 launcher now uses `OnDeviceSetupActivity`; no private ADB test-import entry is present in production. It retains the old launcher component as an alias. With an installed game it opens as a launcher (Play, Check for updates); the app icon while the game runs returns to the game. Plain-Java checks for the shared cache/source importer and production wiring:

```bat
javac -d out/import-tests android/src/main/java/io/github/mogeldev/scda/GameImport.java android/src/main/java/io/github/mogeldev/scda/CompileCache.java android/src/main/java/io/github/mogeldev/scda/GameSource.java tools/on-device-setup/CompileCacheTest.java tools/on-device-setup/GameSourceTest.java
java -cp out/import-tests io.github.mogeldev.scda.CompileCacheTest
java -cp out/import-tests io.github.mogeldev.scda.GameSourceTest
javac -d out/import-tests android/src/main/java/io/github/mogeldev/scda/UpdateCheck.java android/tests/UpdateCheckTest.java && java -cp out/import-tests io.github.mogeldev.scda.UpdateCheckTest
python -m unittest discover -s android/tests -p "test_*.py"
```

The [emulator harness](../../tools/on-device-import-test/README.md) uses these exact shared classes, under a separate app ID. The [separate full setup app](../../tools/on-device-setup/README.md) still supports import/compile/gameplay tests without replacing production. Production-package upgrade, manual file/folder pickers and low-storage tests remain necessary. Do not install over the normal phone app without permission.

## Vulkan renderer tests

No GPU or game assets needed. Tests cover pass-end barrier stage/access scopes (attachment RAW/WAW, fragment-read WAR, transfer consumers and global alias scope); unused programmable texture modes (including coordinate/discard preservation) and fixed-function stage termination; full-attachment clear detection (empty/multiple/partial/offset/layer rejection); descriptor-set key equality/misses for every binding field, per-frame isolation and reset; 5,000 images sharing one allocation, alignment/non-overlap, hole coalescing, memory-type isolation, dedicated/large allocations and OOM fallback; present/flush fence ordering, concurrent wait/reset and failure handling; real glslang compilation with SPIR-V `PointSize` stores for fixed/programmed vertex shaders; and shadow/wide-filter fragment helpers across all four stages, fixed/programmed and quad paths, checking that no image/sampler resource is stored by `OpStore`.

On Windows, use an **x64 Visual Studio developer command prompt** with CMake/Ninja on PATH. Set `NDK` to your Android NDK directory (the NDK Clang supports the shader generator's `__int128`, unlike MSVC):

```bat
set "LLVM=%NDK%\toolchains\llvm\prebuilt\windows-x86_64"
cmake -S android/tests/renderer -B out/renderer-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="%LLVM%/bin/clang-cl.exe" -DCMAKE_CXX_COMPILER="%LLVM%/bin/clang-cl.exe" -DCMAKE_C_FLAGS="--target=x86_64-pc-windows-msvc" -DCMAKE_CXX_FLAGS="--target=x86_64-pc-windows-msvc" -DVULKAN_HEADERS="%LLVM%/sysroot/usr/include"
cmake --build out/renderer-tests --parallel 8
ctest --test-dir out/renderer-tests --output-on-failure
```

The first configure fetches glslang 16.0.0, the same version used by the APK. Only Vulkan headers are copied from the NDK; its libc headers are not used for host tests. This is a test executable, not PC gameplay support. Hardware checks still need Adreno/Mali gameplay, mission transitions, render-scale changes, background/resume, and point effects.

On a clean Android install, also manually test picker cancellation, import progress, Back/cancellation, a valid full import, automatic game launch and a second launch that skips setup. Do not clear an existing install's data without backing up saves/settings.
