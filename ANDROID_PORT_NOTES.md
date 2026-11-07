# SCDA Android port: notes

State as of 2026-11-05. Branch `android-port`; latest work: Windows ARM64 player builder and first-boot game ZIP importer.

## Scope

- Native Android port of the SCDA Xbox recomp, released for other players. The fork may diverge from upstream.
- Campaign only (default.xbe). No co-op, versus, networking or voice. Starting versus shows a notice.
- Controller only. No touch, mouse or keyboard.
- Shader Model 1.1 Xbox look only. No SM3, HDR, AO, SMAA or MSAA.
- Must run on Snapdragon/Adreno and Mali. Mali has no BC textures, so they are decoded on the CPU.
- The game code is recompiled on the player's Windows PC into `libscda_code.so`. Players put it and the extracted campaign files into one ZIP; Android imports that ZIP on first boot through the system file picker. `tools/android-builder/` provides the self-contained Windows builder package.
- Not needed: F10 ImGui overlay, texture dump/replace, splash, custom cursor, GitHub Actions release packaging. Build and package locally.
- minSdk 30.

## Phases

| Phase | State |
|---|---|
| 0 Setup | done |
| 1 Platform layer | done |
| 2 Game code as ELF .so | done; Windows Android-ARM64 player builder implemented and tested |
| 3 Vulkan backend | first version done |
| 4 ARM64 phone | runs on Adreno 710 at 60 fps; Mali untested |
| 5 Release | Local builds/packaging only; GitHub Actions not required |

Commits: 644bd49 bring-up, 6e5a513 story boots headless, 041ba7f mission relaunch, 8344e24 pad bridge, 405cc4e Vulkan renderer, c38a742 runs on the phone, 94f4cfc late readbacks / border colours / unfiltered depth, 9a7852e 50% render default and -O2 host.

## Next session, in this order

1. Test on a Mali device, if one can be found. This also tests the CPU BC decode path.

## Architecture

- `SetupActivity` imports a private ZIP into `Android/data/<pkg>/files/game/` through the system file picker. It validates paths, XBE/ELF headers, ABI and CRCs, then atomically publishes the game directory. `libmain.so` copies the imported `game/libscda_code.so` into internal storage when present, else supports the existing `files/libscda_code.so` layout and the dev copy in the APK.
- `libscda_code.so` is freestanding (`-nostdlib -z undefs`) with DT_NEEDED `libscda_host.so`. The host is linked `-z global -z undefs`, so its X86_FUNC overrides beat the code's weak aliases. Exceptions cross both ways with `ANDROID_STL=c++_shared`.
- arm64 game code needs `-mno-outline-atomics`: the `__aarch64_*` helpers live in the compiler runtime, which the code doesn't link.
- src/android/host.cpp stands in for app/main.cpp and the Windows-only app files. src/android/audio.cpp is SDL audio. core/win32_keys.h gives VK/XInput names off Windows. xnet.cpp runs on POSIX sockets.
- Mission relaunch: every mission starts as a quick reboot (HalReturnToFirmware → kernel::Relaunch). On Android, app::Restart (host.cpp, JNI) starts Relaunch.java in process `:relaunch`, which kills the game pid and starts SDLActivity again. `hdd/launch.bin` carries the mission data. Takes about 0.2–0.5 s.
- CMake: the `if(DEFINED ANDROID_ABI)` branch (ANDROID is unset before project()). The NDK's --no-undefined is overridden by -z undefs. Generated headers come from generated/game, so the Windows recompile has to run first. The host compiles at -O2 even in debug builds.

## Vulkan renderer (src/android/vulkan.cpp)

- Port of d3d12.cpp's core. glslang 16.0.0 (FetchContent; the host needs the `ENABLE_HLSL` define) compiles the generated HLSL on the device.
- ForGlslang() swaps `1.#INF` for `asfloat(0x7F800000)` and adds `[[vk::location(r)]]` to ATTRr inputs.
- Bindings: b0/b1 → 0/1 (dynamic UBOs), t0–3 → 2–5, s0–3 → 6–9.
- Render passes with cached framebuffers. Dynamic rendering was dropped, because the Adreno 710 driver is Vulkan 1.1 without it. Targets stay in GENERAL layout, with one global barrier per pass end.
- Uploads go in a separate `up` command buffer submitted ahead of the draws. A flipped viewport keeps D3D's y convention.
- Swapchain: preTransform = currentTransform, extent swapped for 90/270, the present blit rotates (90: rot = 0,1,-1,0).
- Caches: SPIR-V in `files/shadercache/vulkan`; VkPipelineCache saved every 600 presents.
- Late readbacks: 3 slots with a submit serial. They stop the per-frame stall on the title's NOOVERWRITE lock.
- VK_EXT_custom_border_color without a format, when supported.
- Depth textures sample nearest where the depth format can't filter linearly.
- BC textures are decoded on the CPU where the GPU lacks BC.

Renderer follow-up completed:
- WaitGpuFrame now waits on the submission fence recorded for the requested present; fence access is serialized against frame-slot reuse.
- Optimal-tiled images now share 32-MiB Vulkan allocations with alignment-aware free-range reuse; dedicated-required and large images use separate allocations. A whole-block device-memory failure retries with the exact image size. Freed ranges are reused only after the existing deferred image-destruction fence completes.
- Vulkan vertex shaders now write the PointSize built-in using programmable `oPts`, fixed-function vertex register 6, or point-size render state, clamped to device limits. Devices without `largePoints` get a constant 1.0. Shared D3D shader generation is unchanged; modified shader text gives new SPIR-V cache keys.
- Host tests cover allocation/fence bookkeeping and compile generated point shaders to SPIR-V; APK rebuild passed. User confirmed the updated APK runs the game successfully on the Adreno 710 phone. Mali gameplay and specific point-effect/background-resume validation remain.

Performance work in progress:
- Descriptor sets are cached per frame slot by buffer handles/ranges, texture views/layouts and samplers; dynamic UBO offsets remain per bind. The cache clears after its fence completes and before pool reset, including mid-frame flushes.
- `adb shell setprop debug.scda.vk_profile 1` before launch enables CPU timings, upload/pass/flush/readback counters and GPU timestamps when the queue supports them. `vk-perf` logs summarize 120 presents; CPU draw time includes descriptor time, compilation and any waits inside draws. GPU time is per completed submission, not per present, and can include scheduling/semaphore delays; unavailable timestamps report zero samples.
- `adb shell setprop debug.scda.vk_descriptor_cache 0` disables reuse for comparison (`1` restores it). With profiling enabled this is reread at each 120-present report, allowing same-scene A/B tests without restarting. Profiling defaults off, caching on.
- Live cache on/off/on comparison in the same Iceland view on the x86_64 emulator / RTX 2080 Ti, roughly 461 draws/present: uncached 463–464 sets, 54 ms descriptor CPU time and 112 ms CPU draw time; cached 173 sets, 24–25 ms descriptors and 81–83 ms CPU draw time. Latest 120-present windows yield about 8.2 FPS uncached versus 11.1 FPS cached. Warm samples had no shader/pipeline compilation, mid-frame flushes or synchronous readbacks; reported GPU submission intervals were about 1.5–1.7 ms. This is an emulator CPU/driver-call result, not a representative Adreno GPU result.
- Redmi Note 13 Pro 5G / Adreno 710 Vulkan 1.1: ARM64 APK built without embedded game code, signing certificate matched, updated in place without clearing game data. Same stationary Iceland view, live on/off/on, seven warm 120-present samples per phase after excluding switch boundaries (~561 draws/present): on-before/on-after allocated ~198 sets versus ~563 off (65% fewer); descriptor CPU time 1.31 ms on versus 1.75 ms off (~25% less), total CPU draw time 7.67–7.78 ms on versus 8.08 ms off. FPS was 56.72/56.93/57.04 for on/off/on: no measurable FPS gain. GPU submission intervals stayed ~16.5 ms and may include pacing/semaphore waits, so these timestamps alone do not prove a GPU bottleneck. Thermal status remained 0, battery temperature 27.0–27.5 C; no shader/pipeline compilation, flushes or synchronous readbacks in sampled windows. Logs/phase metadata/summary saved under ignored `out/vulkan-perf/phone-*`. Caching restored on; broader visual/background-resume/Mali validation remains.
- Same-view phone resolution probe with caching on: user lowered render resolution from 50% (internal guest-relative scale 1.271) to 20% (0.508), shadow resolution 512. Twelve warm 120-present windows at 20% averaged 106.3 FPS, 5.21 ms CPU draw time, 0.98 ms descriptor time and 7.72 ms GPU submission interval, ~551 draws and the same 37 passes/present. Earlier 50% sample was ~57 FPS, 7.67–7.78 ms CPU draw time and ~16.5 ms GPU interval. No thermal throttling reported (status 0, battery 30 C at probe start). This strongly suggests resolution-sensitive rendering/bandwidth cost rather than descriptor overhead limits this view; timestamps still include possible pacing waits. 20% also degrades the game's HUD/FPS text, so it is a diagnostic setting, not a recommended quality target. Logs/summary: ignored `out/vulkan-perf/phone-scale20*`. Subsequent pass-level profiling back at 50% returned to ~55 FPS, consistent with the earlier higher-resolution result.
- Tested and removed an unsuccessful attachment-layout experiment: COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL subpass references with explicit external dependencies, GENERAL entry/exit layouts and retained post-pass global barriers. Same stationary 50% Iceland view, ~561 draws/37 passes, eight warm windows per phase: baseline/optimal/baseline 55.13/55.58/54.89 FPS; GPU submission intervals 16.54/16.56/16.53 ms. Pass timings were effectively unchanged; no convincing gain, so no layout switch or extra transitions retained. Thermal status stayed 0 (battery 34–35 C). Logs/screenshots/metadata/summary: ignored `out/vulkan-perf/phone-layout-*`. This does not rule out persistent resource-specific layouts or narrower barriers; those are separate experiments.
- Profiling now timestamps up to 128 passes per submission, reads results only after the frame fence, and logs the eight most expensive target-address/size groups in `vk-pass` with ms/submission, passes/submission and draws/pass. Zero target address groups auxiliary blits/presentation. Bottom-of-pipe boundaries deliberately serialize intervals including load/store/transitions and the pass-end barrier; their overhead and possible pacing contamination mean A/B comparisons must use the same profiling mode. Aggregate GPU results retain their scheduling/pacing caveat. In the measured Iceland view the two 813x610 scene-target groups account for ~5.32 + 5.26 ms/submission, final 1220x2712 presentation ~1.82 ms, and the 512x512 shadow-map group ~0.37 ms. These are grouped intervals, not individual shader timings.
- Host descriptor-key/reset tests and ARM64 builds pass. Pass-query bounds/recording invariants have assertions. No layout/barrier/geometry optimization retained from the second experiment; the running test APK is restored to baseline layouts via its switch. Broader gameplay/visual validation remains. The private x86_64 `-PdevCode` APK contains private game code and must not be distributed.

Remaining limitations:
- Colour-blind modes and the ImGui overlay are left out.
- Point-sprite texture coordinates and fixed-function distance attenuation are not implemented by this point-size fix.

## On-phone settings (user-verified on Adreno 710)

- Use the game's **Options > Recomp Settings** and **Options > Controller** pages, with the controller. The existing
  game-menu hook supplies these pages; Android no longer lists Mouse and Keyboard.
- Display: render resolution, FOV, brightness, contrast, FPS cap/counter and FMV scale. Graphics: anisotropic
  filtering, shadow resolution/distance. General: skip intros, language (restart required) and audio output
  (automatic, mono or stereo). Unsupported desktop/SM3/network/texture options are hidden.
- Controller sensitivity, inversion, deadzone, rumble, gyro and button bindings reuse the existing rows. The
  push-to-talk binding is left out. Changes save to `files/scda.toml` through the shared settings code.
- Vulkan applies render-scale and shadow-resolution changes between frames and retires old target/readback
  resources behind frame fences. Creating the window/swapchain invalidates the cached output dimensions, so an
  early 1920x1080 stand-in does not remain the aspect/render-scale basis.
- Validation: Windows release build and Android-conditional syntax checks of the two menu files pass. ARM64 debug
  APK built with NDK 29 and JDK 21, installed without clearing app data, and launched on Adreno 710. Startup logs
  show Vulkan rendering and the 8BitDo controller detected; no crash observed during startup.
- User confirmed the device checklist passes: controller navigation, live render scale 50% to 40% and back,
  shadow resolution, FPS counter, brightness/contrast, controller rebind/defaults and persistence after relaunch.

## Test devices

**Emulator:** AVD scda_x64 (android-35 x86_64 image), RTX 2080 Ti host, Vulkan 1.3. Files pushed by adb are unreadable for the app there: run `adb root` and `chown -R <app uid>:1078 /data/media/0/Android/data/<pkg>/files/assets`.

**Phone:** Xiaomi Redmi Note 13 Pro 5G, SM7435 / Adreno 710, Vulkan 1.1, Android 16, 2712x1220, swapchain transform ROTATE_90.
- BC textures yes, custom border colours yes, D24S8 not linearly filterable.
- Needs "Install via USB" in developer options.
- Files pushed with adb into `Android/data/<pkg>/files/assets` are readable (no chown needed).
- An 8BitDo Ultimate 2C over Bluetooth works.
- Performance is GPU-bound: render_percent 100 (scale 2.54) gives 16 fps, 50 (scale 1.27) gives 60 fps (vsync cap). The CPU has headroom (game thread ~67%, render ~44%). The Android default render_percent is 50 (src/app/app.h).

## Dev workflow

- ZIP importer tests: see `android/tests/README.md`. 128 synthetic checks plus extraction/validation of the real campaign ZIP passed. ZIP packaging tests pass; ARM64 APK built without embedded game code and existing Adreno install launched successfully. User confirmed the fresh-install game ZIP import works on Adreno 710. Import uses a staging directory, lock, free-space/size limits and cancellation; never overwrites an existing `game/` folder, saves or settings.
- Windows GUI builder: `tools/android-builder/package.ps1` creates `out/SCDA-Android-ARM64-Builder.zip`, containing a small WPF GUI EXE plus companion compiler tools, scripts and licenses. It contains no game assets or generated code. `Builder.cs` uses code-only WPF (no WinForms reference), WPF file dialogs and the native Windows shell folder picker. Build work runs off the UI thread; a throttled bounded log queue/timer keeps the window responsive during verbose compiler output, while the full timestamped log is saved to `build.log`. It lets players select an original Xbox ISO or extracted game folder and an output ZIP, reusing `build.ps1`/`package-game.ps1`. ISO extraction uses bundled extract-xiso (XboxDev), pinned to `3f5b62cfe68f000b0e3c8a30104973f3a297948e`, built with the static Windows runtime; its license is included. This product includes software developed by in <in@fishtank.com>. The source ISO is read-only and temporary extracted files are cleaned up after success/failure. Resumable builds stay in the extracted builder's `work/` directory; no embedded payload, CLI or external runtime installation is required for players. The GUI accepts the matching `default.xbe`, uses NDK Clang with `-mno-outline-atomics`, resumes object compilation with Ninja, and emits the import ZIP with 16-KB-aligned ARM64 `libscda_code.so`.
- Validation: `tools/android-builder/test.ps1` passed WPF window/control and no-WinForms-reference, small EXE/companion tool, and `test-gui.ps1` passed a 24,000-line output flood with a 46 ms maximum UI heartbeat gap. The visible log is bounded and full output is saved to `build.log`. Folder and generated-XISO build-to-ZIP, ISO immutability/cleanup/rejection, path validation, XBE rejection, full build, export, host dependency, inline-atomic, no-op rebuild and resume checks also passed. A stable hash-validated `work/default.xbe` preserves Ninja state across temporary ISO extraction paths. The desktop USA retail ISO also passed the complete extraction → matching-XBE validation → ARM64 build → campaign ZIP pipeline with Windows-only PATH: 1,318 ZIP entries, approximately 3.1 GiB compressed. Source ISO SHA-256 was unchanged and temporary extraction was cleaned up. User confirmed the resulting retail-ISO-generated ZIP imports and works on the Adreno 710 phone. The generated library was also previously loaded successfully by the APK.
- SDK in this environment: `C:\Users\admin\AppData\Local\Packages\Claude_pzs8sxrjxfjjc\LocalCache\Local\Android\Sdk`.
  Set both `ANDROID_HOME` and `ANDROID_SDK_ROOT` to this path for builds; the old
  `C:\Users\admin\AppData\Local\Android\Sdk` path is absent. No global environment changes were made.
- Direct build: `cmake -S . -B out/android-x86_64` with the NDK toolchain, `-DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_shared -DSCDA_DEV_CODE=ON`, then ninja.
- APK (from the repo root): `android/gradlew.bat -p android assembleDebug -PdevCode -Pabi=x86_64` (or `-Pabi=arm64-v8a` for the phone). Output: `android/build/outputs/apk/debug/scda-debug.apk`.
- New installs use the first-boot ZIP importer; imported files go to `/sdcard/Android/data/io.github.mogeldev.scda/files/game/`. Existing direct installs under `files/assets` remain supported.
- Controller in the emulator: `python tools/padbridge/padbridge.py` turns an XInput pad on the PC into a uinput Xbox 360 pad (needs adb root). The pad shows up in the game about 9 s after the uinput device appears.
- Logs: `adb logcat -s scda:*`. The file log is in `files/logs`.
- Windows builder checks: build the recompiler using `tools/android-builder/README.md#maintainers`, then run `tools/android-builder/test.ps1`. The legacy PC gameplay build/release scripts and Windows gameplay CMake presets have been removed.

Toolchain: Android SDK (NDK 29.0.14206865, cmake 3.31.6, emulator, platform 36), LLVM 23.1.5, JDK 25, Gradle 9.8.2, AGP 9.4.1.

## Lessons

- The Android window disappears before WILL_ENTER_BACKGROUND is polled, so host.cpp uses SDL_AddEventWatch for the lifecycle.
- input.cpp may not pump SDL on Android; it peeks only gamepad add/remove events.
- adb gamepad keyevents are not joysticks. Android lists the emulator's qwerty2 keyboard as a gamepad, so input.cpp skips pads without a left stick on Android.
- The instance must ask for the highest API version the loader supports (up to 1.3), or 1.1+ entry points come back null.
