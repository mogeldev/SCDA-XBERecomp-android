# ZIP importer test

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

## Vulkan renderer tests

No GPU or game assets needed. Tests cover 5,000 images sharing one allocation, alignment/non-overlap, hole coalescing, memory-type isolation, dedicated/large allocations and OOM fallback; present/flush fence ordering, concurrent wait/reset and failure handling; and real glslang compilation with SPIR-V `PointSize` stores for fixed/programmed vertex shaders.

On Windows, use an **x64 Visual Studio developer command prompt** with CMake/Ninja on PATH. Set `NDK` to your Android NDK directory (the NDK Clang supports the shader generator's `__int128`, unlike MSVC):

```bat
set "LLVM=%NDK%\toolchains\llvm\prebuilt\windows-x86_64"
cmake -S android/tests/renderer -B out/renderer-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="%LLVM%/bin/clang-cl.exe" -DCMAKE_CXX_COMPILER="%LLVM%/bin/clang-cl.exe" -DCMAKE_C_FLAGS="--target=x86_64-pc-windows-msvc" -DCMAKE_CXX_FLAGS="--target=x86_64-pc-windows-msvc" -DVULKAN_HEADERS="%LLVM%/sysroot/usr/include"
cmake --build out/renderer-tests --parallel 8
ctest --test-dir out/renderer-tests --output-on-failure
```

The first configure fetches glslang 16.0.0, the same version used by the APK. Only Vulkan headers are copied from the NDK; its libc headers are not used for host tests. This is a test executable, not PC gameplay support. Hardware checks still need Adreno/Mali gameplay, mission transitions, render-scale changes, background/resume, and point effects.

On a clean Android install, also manually test picker cancellation, import progress, Back/cancellation, a valid full import, automatic game launch and a second launch that skips setup. Do not clear an existing install's data without backing up saves/settings.
