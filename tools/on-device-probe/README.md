# On-device compiler prototype (Windows host, Android emulator or ARM64 phone)

Separate debug-only APK: `io.github.mogeldev.scda.compilerprobe`. Does not modify the game APK, importer, saves, or Windows builder.

## What it proves

Inside an ordinary Android app process, targeting API 36:

1. Execute Android-native Clang and LLD **from the APK's installed native-library directory**.
2. Compile a tiny C++ function to a native object (x86-64 on the emulator, ARM64 on the phone) and link a shared library in private app storage.
3. `dlopen` that library, resolve its function, and assert `probe_answer(35) == 42`.
4. Validate the imported `default.xbe` SHA-1, then run the actual XBE translator on Android to produce all 128 C++ partitions and supporting files. All **134 outputs** must match SHA-256 references from the Windows translator.
5. Compile the **newly generated on-device** `x86_recomp.0.cpp` at `-O2` to ARM64.
6. Link a **partial** ARM64 game library and validate ELF class, object/shared-library type and machine architecture. It deliberately retains unresolved host/other-partition symbols and is **not loadable game code**.

The compiler/linker are PIE executables packaged under `libprobe_*.so` names, not shared libraries loaded via JNI. Android extracts them at install time using legacy native-library packaging. This avoids executing a binary from writable app storage, without root or lowering the target SDK. Only the small `libprobe.so` loader uses JNI.

Uses pinned, SHA-256-verified Android x86-64 or ARM64 LLVM 21.1.8 packages from [Termux](https://packages.termux.dev/apt/termux-main/). `packages.json` pins x86-64; `packages-arm64-v8a.json` pins ARM64. `prepare.py` checks the compiler ELF architecture and relocates dependency names and RUNPATH to the APK directory. In particular the bundled versioned zlib is renamed to avoid Android's system `libz.so`. No Termux installation, package scripts, system modifications, SDK sysroot or runtime network access are needed. `clang -cc1` avoids driver subprocess/path assumptions and uses the project's freestanding headers.

The XBE translator is built from the existing `tools/recomp/*.cpp` sources, unchanged, through `recompiler/CMakeLists.txt`. It uses the same Zydis v4.1.1 and tomlplusplus v3.4.0 tags as the Windows builder. The runner cross-compiles it with the NDK as a static-libc++ PIE (`libprobe_recomp.so`) before building the APK. A separate build keeps the translator's NDK runtime from replacing the Termux compiler's newer shared libc++. Translation runs as an isolated subprocess with measured peak RSS; errors stop the pipeline. Each translation starts in a fresh output directory so stale outputs cannot pass reference checks.

**Developer experiment only. Do not publish this APK.** Before shipping a compiler, review upstream sources/build provenance, licenses/notices (LLVM and all bundled dependencies, including libiconv), security updates, package size, 16-KB page compatibility, and distribution policy. Pinned package URLs may eventually expire. This is not yet the production compiler supply chain.

## Run

Requires the same JDK 21, Android SDK 36, NDK 29.0.14206865 and CMake 3.31.6 as the game build, plus Python 3 and Git with HTTPS access. Place the validated USA retail `default.xbe` in `assets/`. You must also have privately generated `generated/game/` with the current Windows recompiler **only as a test oracle**: the runner sends reference hashes, not those generated sources. The device receives the XBE, two configs, CPU headers and reference hashes separately via ADB. **Neither the XBE nor generated game code is embedded in the APK.**

From the repository root:

```bat
python -m unittest discover -s tools/on-device-probe -p "test_*.py" -v
```

### Emulator

Start the existing `scda_x64` AVD, then:

```bat
python tools/on-device-probe/run.py --sdk "C:\path\to\Android\Sdk" --serial emulator-5554
```

### ARM64 phone (USB or wireless ADB)

For wireless ADB, enable Developer options > Wireless debugging, pair with `adb pair IP:PAIRING_PORT`, then connect with `adb connect IP:CONNECTION_PORT`. The two ports differ and may change. Do not commit pairing codes or device addresses.

```bat
python tools/on-device-probe/run.py --sdk "C:\path\to\Android\Sdk" --serial IP:CONNECTION_PORT --allow-phone
```

Keep the phone unlocked and the prototype in the foreground until `PASS` appears. The runner detects the target's ABI and builds only that architecture. It refuses physical devices unless `--allow-phone` is explicitly passed. Installing this separate app never clears or updates the actual game app.

`--sdk` defaults to `ANDROID_HOME`. The runner downloads compiler packages into `out/on-device-probe/`, builds/installs only the test APK, streams private inputs directly into the debug app's internal storage via `run-as`, verifies every transferred file with SHA-256, and waits for `PASS`. Raw `adb exec-in` avoids OEM shell corruption of binary input; verification waits for asynchronous extraction to finish. No shared-storage staging or `adb root` is used. Re-running replaces prototype outputs, not the game installation.

Every command has a 300-second timeout. The smoke test is serial. Full builds use adaptive parallel batches: at most `cores - 1` workers, bounded by `(available RAM - reserve) / 512 MiB`. The reserve is at least 768 MiB or Android's low-memory threshold, whichever is higher. At least one worker is used. Moderate thermal pressure halves the CPU limit; severe or higher limits it to one. RAM/thermal status is checked between batches, not in the middle of a compiler process. The budget is conservative, not a guarantee against OOM on unusually large partitions. Link and translation remain single commands. The probe keeps its screen on while open; closing/backgrounding it can still interrupt the experiment.

Android's `/system/bin/time -v` records per-process peak RSS and elapsed/user/system time alongside the app's wall time. RSS includes mapped compiler libraries, not the Java app or total system memory; summing process RSS double-counts shared pages.

### Full ARM64 build (phone only)

Append `--host-apk` to the phone command to compile all 129 on-device-generated source files and link against a matching game host:

```bat
python tools/on-device-probe/run.py --sdk "C:\path\to\Android\Sdk" --serial IP:CONNECTION_PORT --allow-phone --host-apk out/SCDA-Android-V1.apk
```

The host `.so`, SDL and C++ runtime are read from that APK and sent as private inputs, **never packaged in the probe APK or installed into the game**. After linking, a packaged native checker loads `libscda_code.so` with `RTLD_NOW | RTLD_LOCAL`, matching production. DT_NEEDED brings in the matching host and its dependencies. The checker validates address metadata, all 33,172 dispatch mappings, their resolved function pointers (including host overrides), the entry mapping, four continuations and `ScdaMain`. It never calls `ScdaMain`, initializes SDL or calls generated game functions. Loading does execute native library initializers, so only use trusted, matching runtime inputs.

The load checker runs in a **separate app-owned subprocess**, not the Java/JNI process. Its `LD_LIBRARY_PATH` points only to the privately transferred game runtime; compiler processes still use the APK's pinned Termux runtime. This prevents a false pass against Termux's newer C++ library. A missing symbol, failed dispatch check or subprocess crash fails the probe. The checker targets the fixed validated USA image; it is not a validator for arbitrary/untrusted native libraries.

The resulting private `libscda_code.so` stays in the probe app; the runner copies it to ignored `out/on-device-probe/libscda_code-phone.so` and verifies the transfer hash. The actual game app and saves are never modified. Objects are rebuilt from scratch each run; no resume/cache yet. This is a **compile/link/load/dispatch** check, not a gameplay check. The runner waits up to two hours in full mode.

To repeat only the load check using the previously retrieved phone-built library (no translation or compilation), append:

```bat
--code-library out/on-device-probe/libscda_code-phone.so
```

This requires `--host-apk`. Inputs must be ARM64 ELF64 shared libraries. Load-only mode transfers its input separately and never overwrites the retrieved build output.

Test worker selection without Android:

```bat
mkdir out\on-device-probe\job-tests
javac -d out/on-device-probe/job-tests tools/on-device-probe/src/main/java/io/github/mogeldev/scda/compilerprobe/BuildJobs.java tools/on-device-probe/BuildJobsTest.java
java -cp out/on-device-probe/job-tests io.github.mogeldev.scda.compilerprobe.BuildJobsTest
```

Outputs:

- APK: `out/on-device-probe/build/outputs/apk/debug/scda-compiler-probe-debug.apk` (most recently built ABI)
- Report: `out/on-device-probe/result.txt`, also preserved as `result-x86_64.txt` or `result-arm64-v8a.txt`
- Device diagnostics: `adb -s SERIAL logcat -s CompilerProbe AndroidRuntime`
- Device artifacts: prototype app's `files/` directory, accessible with `adb -s SERIAL shell run-as io.github.mogeldev.scda.compilerprobe ...`

Uninstalling **only this prototype** removes its private generated inputs and compiled outputs:

```bat
adb -s SERIAL uninstall io.github.mogeldev.scda.compilerprobe
```

Never share generated game inputs/outputs. The downloaded tools and all build outputs stay under ignored `out/`.

## Recorded results

Both targets use target SDK 36 and normal app privileges, without root:

| Check | Android 35 x86-64 emulator | Android 16 / API 36 ARM64 phone |
|---|---|---|
| Packaged Clang and LLD execute under app UID | PASS | PASS |
| Native tiny compile / link / load / call | PASS; returns 42 | PASS; returns 42 |
| XBE → all generated C++/support files | PASS; about 5.6 seconds | PASS; about 7.3 seconds |
| All 134 outputs vs. Windows SHA-256 references | PASS; byte-identical | PASS; byte-identical |
| On-device-generated partition → ARM64 object | PASS; about 4.3 seconds | PASS; about 7.9 seconds |
| Partial ARM64 library link | PASS; about 0.1 seconds | PASS; about 0.1 seconds |

The phone is the Xiaomi Redmi Note 13 Pro 5G (Snapdragon 7s Gen 2), accessed through wireless ADB. The translator process peaked at **137,884 KiB RSS (about 135 MiB)**; the generated partition compiler at **257,096 KiB (about 251 MiB)**; the linker at **62,536 KiB (about 61 MiB)**. The translator identified 33,172 functions. Both devices produced a 226,576-byte ARM64 object and a 248,872-byte partial shared library. Compilation timings cover one partition only, not full-game setup. Eight host unit tests cover ELF/archive helpers, both package manifests/ABI selection, XBE rejection and staging only hashes rather than generated sources.

The ARM64 prototype APK including the translator is about **76 MiB** compressed; the x86-64 APK is roughly 76 MB, with roughly 206 MiB of staged compiler executables/libraries. Neither is size-optimized. The standalone project's `gradle.properties` gives Gradle a 2-GiB heap for packaging these large libraries. Only 4-KiB-page devices have been tested.

### Full adaptive phone build

All **129 objects compiled and linked successfully in 437,526 ms (7 min 18 s)**, excluding translation, reference verification, APK installation and ADB transfer. Four compiler workers were selected on the eight-core / approximately 7.1-GiB-RAM phone, with 3,013 MiB available at initial worker selection. Every batch retained four workers; Android reported no thermal throttling status requiring a reduction during admission checks. The busiest recorded compiler reached approximately **275 MiB RSS** (per process, not total build memory). The full link took about 0.3 seconds. The earlier serial experiment was stopped before completion, so no measured end-to-end serial speedup is claimed.

The output is **29,255,568 bytes (27.9 MiB)**, with SONAME `libscda_code.so`, sole DT_NEEDED `libscda_host.so`, NOW/RELRO, non-executable stack and 16-KiB-aligned LOAD segments. It was linked against the host extracted from the existing matching V1 game APK, copied back privately and SHA-256 verified. It has **not** been installed into the game. The x86-64 smoke path still passes, together with eleven Python unit tests (including runtime staging/rejection) and eight Java worker-policy checks (CPU/RAM limits, one-core/low-memory floor and thermal reductions).

### Full-library load validation

The phone-built library successfully loads in the isolated native subprocess with the matching V1 host, SDL and C++ runtime. All **33,172 mappings**, **4 continuations**, entry metadata and `ScdaMain` resolve correctly, including **290 host overrides**. Initial load plus complete dispatch checking took approximately **5.1 seconds**, with **61,748 KiB peak RSS**. No generated function or `ScdaMain` was called. Missing code, missing host and a tiny library with the wrong exports were each rejected with exit code 1. These loader checks ran on the phone under the debug app UID; normal-app execution of the success path was driven by the probe activity.

A fresh end-to-end phone run then repeated XBE translation (all 134 hashes matched), compiled all 129 objects, linked in **410,351 ms (6 min 50 s for compile/link)**, and passed the complete load/dispatch check in approximately **5.0 seconds**. The private output transfer hash was verified. A separate load-only rerun passed too. The x86-64 emulator smoke path was rerun successfully; its game output remains a deliberately unloaded ARM64 partial library.

## Still unproven / next step

- See [`../on-device-gameplay/README.md`](../on-device-gameplay/README.md) for the separately installed SDL game-process test: the user confirmed gameplay, save/load, background/resume and the Iceland A → B transition with phone-built code. The transition intentionally waits for A, but its loading prompt was not visible; that UI issue remains undiagnosed. Exhaustive campaign/multi-device checks, total/parallel peak RAM, repeatable battery/thermal measurements and complete user-facing setup duration remain unvalidated.
- API 30 and 16-KB-page devices.
- Compiler flags are passed to the internal `-cc1` interface, which must be kept in sync with the pinned compiler. This is not a stable public driver API.
- Experimental setup integration now lives in [`../on-device-setup/README.md`](../on-device-setup/README.md), with an opt-in assets-only importer, progress, cancellation and hashed object recovery under a separate app ID. It builds and passes host tests, but phone installation is currently blocked by user/OEM restriction; integrated Android behavior is unvalidated. The normal importer still requires a prebuilt library. No production rollout or ISO extraction; keep the Windows builder as the working alternative.
