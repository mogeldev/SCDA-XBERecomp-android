# SCDA Android port: notes

State as of 2026-11-05. Branch `android-port`, last pushed commit `9a7852e`.

## Scope

- Native Android port of the SCDA Xbox recomp, released for other players. The fork may diverge from upstream.
- Campaign only (default.xbe). No co-op, versus, networking or voice. Starting versus shows a notice.
- Controller only. No touch, mouse or keyboard.
- Shader Model 1.1 Xbox look only. No SM3, HDR, AO, SMAA or MSAA.
- Must run on Snapdragon/Adreno and Mali. Mali has no BC textures, so they are decoded on the CPU.
- The game code is recompiled on the player's Windows PC into `libscda_code.so`, then copied with the game files into the app folder over USB.
- Not needed: F10 ImGui overlay, texture dump/replace, splash, custom cursor.
- minSdk 30.

## Phases

| Phase | State |
|---|---|
| 0 Setup | done |
| 1 Platform layer | done |
| 2 Game code as ELF .so | done; Windows player builder still to do |
| 3 Vulkan backend | first version done |
| 4 ARM64 phone | runs on Adreno 710 at 60 fps; Mali untested |
| 5 Release | APK and builder zip via GitHub Actions, to do |

Commits: 644bd49 bring-up, 6e5a513 story boots headless, 041ba7f mission relaunch, 8344e24 pad bridge, 405cc4e Vulkan renderer, c38a742 runs on the phone, 94f4cfc late readbacks / border colours / unfiltered depth, 9a7852e 50% render default and -O2 host.

## Next session, in this order

1. A way to change settings on the phone. The ImGui overlay (menu.cpp) isn't drawn on Android, so today only `files/scda.toml` changes them.
2. Test on a Mali device, if one can be found. This also tests the CPU BC decode path.
3. The Windows player builder for the arm64 `libscda_code.so`. It must pass `-mno-outline-atomics`.
4. Release APK via GitHub Actions. The CI build needs generated headers without the xbe.

## Architecture

- `libmain.so` (src/android/main.cpp) runs SDL_main. It copies `Android/data/<pkg>/files/libscda_code.so` into internal storage when present, else uses the dev copy in the APK, dlopens it and calls ScdaMain.
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

Open points:
- WaitGpuFrame is a no-op.
- No settings change at runtime; the render scale is fixed at init.
- Colour-blind modes and the ImGui overlay are left out.
- One vkAllocateMemory per image (risk of hitting the allocation count limit).
- Point size for points is not written.
- Desktop() before the window may cache 1920x1080 for the aspect.

## Test devices

**Emulator:** AVD scda_x64 (android-35 x86_64 image), RTX 2080 Ti host, Vulkan 1.3. Files pushed by adb are unreadable for the app there: run `adb root` and `chown -R <app uid>:1078 /data/media/0/Android/data/<pkg>/files/assets`.

**Phone:** Xiaomi Redmi Note 13 Pro 5G, SM7435 / Adreno 710, Vulkan 1.1, Android 16, 2712x1220, swapchain transform ROTATE_90.
- BC textures yes, custom border colours yes, D24S8 not linearly filterable.
- Needs "Install via USB" in developer options.
- Files pushed with adb into `Android/data/<pkg>/files/assets` are readable (no chown needed).
- An 8BitDo Ultimate 2C over Bluetooth works.
- Performance is GPU-bound: render_percent 100 (scale 2.54) gives 16 fps, 50 (scale 1.27) gives 60 fps (vsync cap). The CPU has headroom (game thread ~67%, render ~44%). The Android default render_percent is 50 (src/app/app.h).

## Dev workflow

- Direct build: `cmake -S . -B out/android-x86_64` with the NDK toolchain, `-DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_shared -DSCDA_DEV_CODE=ON`, then ninja.
- APK: `android/gradlew.bat assembleDebug -PdevCode -Pabi=x86_64` (or `-Pabi=arm64-v8a` for the phone). Output: `android/build/outputs/apk/debug/scda-debug.apk`.
- Game files go to `/sdcard/Android/data/io.github.mogeldev.scda/files/assets`.
- Controller in the emulator: `python tools/padbridge/padbridge.py` turns an XInput pad on the PC into a uinput Xbox 360 pad (needs adb root). The pad shows up in the game about 9 s after the uinput device appears.
- Logs: `adb logcat -s scda:*`. The file log is in `files/logs`.
- Windows check: `build.bat` with CI=1 and the SDK's cmake 3.31.6 on PATH.

Toolchain: Android SDK (NDK 29.0.14206865, cmake 3.31.6, emulator, platform 36), LLVM 23.1.5, JDK 25, Gradle 9.8.2, AGP 9.4.1.

## Lessons

- The Android window disappears before WILL_ENTER_BACKGROUND is polled, so host.cpp uses SDL_AddEventWatch for the lifecycle.
- input.cpp may not pump SDL on Android; it peeks only gamepad add/remove events.
- adb gamepad keyevents are not joysticks. Android lists the emulator's qwerty2 keyboard as a gamepad, so input.cpp skips pads without a left stick on Android.
- The instance must ask for the highest API version the loader supports (up to 1.3), or 1.1+ entry points come back null.
