# On-device compiler prototype (Windows + x86-64 Android emulator)

Separate debug-only APK: `io.github.mogeldev.scda.compilerprobe`. Does not modify the game APK, importer, saves, or Windows builder. No phone required for this experiment.

## What it proves

Inside an ordinary Android app process, targeting API 36:

1. Execute Android-native Clang and LLD **from the APK's installed native-library directory**.
2. Compile a tiny C++ function to an x86-64 object and link a shared library in private app storage.
3. `dlopen` that library, resolve its function, and assert `probe_answer(35) == 42`.
4. Cross-compile the player's existing `generated/game/x86_recomp.0.cpp` at `-O2` to ARM64.
5. Link a **partial** ARM64 game library and validate ELF class, object/shared-library type and machine architecture. It deliberately retains unresolved host/other-partition symbols and is **not loadable game code**.

The compiler/linker are PIE executables packaged under `libprobe_*.so` names, not shared libraries loaded via JNI. Android extracts them at install time using legacy native-library packaging. This avoids executing a binary from writable app storage, without root or lowering the target SDK. Only the small `libprobe.so` loader uses JNI.

Uses pinned, SHA-256-verified Android x86-64 LLVM 21.1.8 packages from [Termux](https://packages.termux.dev/apt/termux-main/). `prepare.py` relocates ELF dependency names and RUNPATH to the APK directory. In particular the bundled versioned zlib is renamed to avoid Android's system `libz.so`. No Termux installation, package scripts, system modifications, SDK sysroot or runtime network access are needed. `clang -cc1` avoids driver subprocess/path assumptions and uses the project's freestanding headers.

**Developer experiment only. Do not publish this APK.** Before shipping a compiler, review upstream sources/build provenance, licenses/notices (LLVM and all bundled dependencies, including libiconv), security updates, package size, 16-KB page compatibility, and distribution policy. Pinned package URLs may eventually expire. This is not yet the production compiler supply chain.

## Run

Requires the same JDK 21, Android SDK 36, NDK 29.0.14206865 and CMake 3.31.6 as the game build, plus Python 3 with HTTPS access. You must already have privately generated the game sources using the current Windows recompiler. They are sent separately to the emulator and **never embedded in this test APK**.

Start the existing `scda_x64` AVD, then from the repository root:

```bat
python -m unittest discover -s tools/on-device-probe -p "test_*.py" -v
python tools/on-device-probe/run.py --sdk "C:\path\to\Android\Sdk" --serial emulator-5554
```

`--sdk` defaults to `ANDROID_HOME`. The runner checks for an x86-64 emulator before installation; it refuses a physical phone. It downloads compiler packages into `out/on-device-probe/`, builds/installs only the test APK, streams private inputs directly into the debug app's internal storage via `run-as`, and waits for `PASS`. Compilation is one process at a time with a 180-second timeout per command. No `adb root` is used. Re-running replaces prototype outputs, not the game installation.

Outputs:

- APK: `out/on-device-probe/build/outputs/apk/debug/scda-compiler-probe-debug.apk`
- Report: `out/on-device-probe/result.txt`
- Device diagnostics: `adb -s emulator-5554 logcat -s CompilerProbe AndroidRuntime`
- Device artifacts: prototype app's `files/` directory, accessible with `adb shell run-as io.github.mogeldev.scda.compilerprobe ...`

Uninstalling **only this prototype** removes its private generated inputs and compiled outputs:

```bat
adb -s emulator-5554 uninstall io.github.mogeldev.scda.compilerprobe
```

Never share generated game inputs/outputs. The downloaded tools and all build outputs stay under ignored `out/`.

## Recorded result

Android 35 x86-64 `scda_x64` emulator on this Windows x86-64 development machine, target SDK 36, no root:

| Check | Result |
|---|---|
| Packaged Clang and LLD execute under app UID | PASS |
| Tiny x86-64 compile / link / load / call | PASS; returns 42 |
| Actual generated partition → ARM64 object | PASS; 226,576 bytes, about 4.5 seconds |
| Partial ARM64 library link | PASS; 248,872 bytes, about 0.1 seconds |
| ELF relocation/archive helper unit tests | 4 passed |

Timings include process startup and are **not phone benchmarks**. The prototype APK is roughly 76 MB compressed, with roughly 206 MiB of staged compiler executables/libraries; it is not size-optimized.

## Still unproven / next step

- ARM64-hosted compiler execution and generated-library loading on an actual ARM64 Android device.
- Porting/invoking `xbe_recomp` inside Android; this experiment uses pre-generated private C++.
- Full 129-source compilation, final host linkage, gameplay, peak RAM, battery/thermal cost and setup duration.
- API 30 and 16-KB-page devices; this emulator only validates Android 35 with its current page size.
- Compiler flags are passed to the internal `-cc1` interface, which must be kept in sync with the pinned compiler. This is not a stable public driver API.
- No production UI, ISO importer, resume/cancellation workflow or importer changes. Keep the Windows builder as the working alternative.
