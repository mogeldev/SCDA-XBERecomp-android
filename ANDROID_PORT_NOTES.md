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
- Windows/desktop game builds (D3D12 renderer, desktop menus) are out of scope and not maintained. Shared code may break them; do not build or test them. The Windows player builder (`tools/android-builder/`) stays in scope.
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

Attachment lifetime/pass-break investigation:
- Added opt-in `vk-lifetime` counts alongside `vk-perf`: actual pass-end causes (target changes, auxiliary work, feedback copies, copies, readbacks, resize, presentation, submission), clear calls and full-attachment clears at pass start. Full-clear detection conservatively requires one rectangle covering the complete single-layer attachment; tests reject empty/multiple/partial/offset/layer cases. These are opportunity counts, not proof that attachment data can be discarded later.
- Stationary 50% Iceland baseline: 37 passes/frame end at 27 target changes, 5 feedback-copy breaks, 2 copies, 1 readback and 2 presentation boundaries. Asynchronous readback pass breaks are separate from the `sync` counter (which counts synchronous CPU readbacks). Only ~2.5 clears/frame are full-attachment pass starts: ~0.524 Mpixels of depth and ~0.003 Mpixels of colour, no stencil. Most of this opportunity is shadow depth rather than the expensive scene targets.
- Built/tested an independent colour/depth/stencil load-op-clear experiment for those full clears only; kept all barriers, external layouts, stores, partial clears and in-pass clears. Nine warm 120-present windows per build: baseline/experiment 55.17/55.47 FPS, GPU submission interval 16.61/16.48 ms, ~561.5 draws and 37 passes/frame. Shadow 512x512 interval 0.379/0.380 ms. Thermal status 0, battery 29/30 C. Restart briefly changed camera/subtitle state, so this is weaker than a live A/B comparison; no convincing benefit claimed. Removed the rendering change and retained diagnostics only. Logs/screenshots/metadata/summary and private local test APKs are ignored under `out/vulkan-perf/phone-lifetime-*`, `phone-load-clear*`, `phone-clear-summary.json`.
- Dependency trace: with profiling enabled, `debug.scda.vk_trace 1` traces one frame after each 120-present report (target/size, pass-end cause, draw count and bound target stages); `0` disables it. Exclude trace windows from benchmarks. Trace entries describe bound resources, not proof of a shader fetch. The five feedback copies are one alpha-only draw each across the scene/postprocess size chain. Captured generated shader bodies show old target blue (`t0.b`) contributes to output alpha, so all five copies are real dependencies: no copy removal or pass merging performed. Logs: ignored `out/vulkan-perf/phone-feedback-trace.log`, `phone-feedback-shader.log`. Temporary shader-source dumping was removed.
- Retained a small CPU cleanup: skip texture preparation for programmable modes 0/4/5/0xA/0x11 (no texture fetch), or fixed-function stages after the first disabled colour operation. Shader coordinate/discard operations remain; sampled stages and feedback copies remain. Unused bindings receive existing dummy resources. Tests cover mode classification, generated no-fetch bodies and fixed-function chain termination. `debug.scda.vk_skip_unused_stages` defaults on; profiling rereads it for controlled live comparisons.
- Same stationary 50% Iceland view, live on/off/on, nine warm windows per phase excluding eight seconds after switches: ~558.5 draws/frame, 37 passes unchanged, ~790–805 unused bound stages skipped/frame. CPU draw 7.328/7.556/7.379 ms; descriptors 1.240/1.310/1.253 ms; sets 186.0/197.8/186.0. FPS 57.059/56.653/57.098: no reliable FPS gain claimed. GPU submission intervals 16.236/16.189/16.189 ms. Thermal status 0, battery 35.8–36.0 C. Logs/screenshots/metadata/summary: ignored `out/vulkan-perf/phone-stages-*`. No geometry, synchronization, layouts or load/store behaviour changed. Broader gameplay/background-resume/Mali validation pending.

Pass-end barrier experiment (opt-in, default unchanged):
- `debug.scda.vk_narrow_barriers 1` replaces only the barrier immediately after `vkCmdEndRenderPass`: source stages are fragment shader + colour output + early/late depth tests; destination stages add transfer. Source access covers colour/depth-stencil attachment writes; destination covers fragment sampling, attachment reads/writes and transfer reads/writes. Fragment execution remains in the source scope for read-before-overwrite (WAR), even though source reads need no availability operation. Memory scope stays global to cover possible aliases. Generated vertex shaders only read uniforms (no image reads/storage writes). Transfer-completion, initialization, feedback-copy and host-readback barriers are untouched, as are layouts/load/store operations and all five feedback copies. This is stage/access narrowing, not per-resource hazard tracking or pass merging. The switch is read at startup and at profiling report boundaries; `0` restores baseline.
- Added `debug.scda.vk_profile_passes` (startup, default `1`) to disable serialized per-pass timestamps independently of whole-submission timestamps. Set to `0` for barrier scheduling experiments; otherwise bottom-of-pipe per-pass boundaries may mask the benefit. Both compared modes used per-pass timestamps OFF and tracing OFF. CPU counters/whole-submission timing remained enabled.
- Same stationary 50% Iceland scene, ~558 draws and 37 passes/frame: global-before / narrow / global-after / narrow-repeat / global-repeat FPS 56.69 / 54.15 / 57.02 / 57.32 / 57.15, GPU submission intervals 16.157 / 16.103 / 16.189 / 16.056 / 16.166 ms (9–10 warm windows/phase, first eight seconds excluded). Initial narrow phase had an unexplained ~2-second report gap, retained in raw averages; median was 57.23 FPS. Repeated both modes rather than silently filtering it. No reliable FPS gain claimed. Thermal status 0, battery 37.6–38.1 C. Baseline restored on phone; narrower barrier stays opt-in pending broader validation. Logs/screenshots/metadata/summary are ignored `out/vulkan-perf/phone-barriers-*`.
- ARM64 build and host tests pass; mocked tests verify exact global stage/access scopes including fragment WAR ordering and transfer consumers. Screenshot inspection found no obvious new corruption in the tested view. Subsequent synchronization validation is recorded below; broader gameplay/background-resume and Mali verification remain. Do not treat mask unit tests as proof of all runtime dependencies.

Phone synchronization-validation run:
- Loaded the official Khronos `vulkan-sdk-1.4.363.1` ARM64 validation layer through Android's per-app debug-layer mechanism, without modifying the APK. Library SHA256 `7d36b66e72fd11270fba96de10002256ba95c8408047a8167e636673c16760ae`. Saved original global debug settings, copied the layer to the debuggable app's private root, enabled `VK_LAYER_KHRONOS_validation`, and set `debug.vulkan.khronos_validation.validate_sync=true`; startup logs explicitly confirm Core Checks + Synchronization enabled. Profiling/tracing off; unused-stage cleanup on. Logs, screenshots, settings backup and summary: ignored `out/vulkan-perf/validation/`.
- Tested startup/loading and stationary Iceland gameplay with original global barriers, then a separate launch with `debug.scda.vk_narrow_barriers=1`. Gameplay process logs through ~71 s baseline and ~58 s narrow contain **zero `SYNC-HAZARD` messages**, with synchronization enabled confirmed in each process. This is bounded scene coverage, not a correctness proof; validation overhead reduced gameplay to ~18 FPS, so these are not performance results.
- Both modes exposed the same general Vulkan errors: `VUID-vkCmdDraw-None-07832` (dynamic scissor missing; one message per gameplay launch, also seen during initial startup screens) and eight `VUID-StandaloneSpirv-OpTypeImage-06924` messages (SPIR-V stores to image/sampler opaque objects). The former is consistent with `UploadTarget` setting a viewport before `Blit` but not setting its own scissor. The latter is consistent with generated HLSL passing texture/sampler objects through helper functions while this build omits SPIRV-Tools optimization; root cause/fix still needs verification. No errors suppressed or fixed in this validation-only task. The layer also reported an informational missing validation shader-cache file under `/tmp`; game shader caches were untouched.
- Cleanup completed: stopped the validation process, restored all original Android global GPU-debug settings, restored layer properties, removed both temporary phone copies of the validation library, restored original barriers, disabled profiling/tracing and relaunched normally. Saves/settings/imported game code and existing shader/pipeline caches preserved. Narrow barriers remain opt-in. This initial run was not general-validation-clean; fixes and the subsequent clean bounded rerun are recorded below.

Validation fixes and rerun:
- `UploadTarget()` now sets a full-target dynamic scissor before its blit, just like the presentation/downsample blits. It no longer depends on scissor state left by a previous draw or command buffer; this also prevents stale partial scissors clipping CPU surface uploads.
- Shared pixel HLSL generation now emits stage-specific shadow/wide-filter helpers referencing global `texN`/`smpN`, instead of passing opaque texture/sampler objects as function parameters. The sampling math is unchanged. This avoids the glslang resource `OpStore` output that requires SPIRV-Tools legalization (not linked in the Android build). No optimizer dependency added. Source hashes change automatically; old shader/pipeline caches were preserved. The shared generator also affects Windows/D3D12; Windows GPU validation was not performed.
- Regression tests compile shadow and wide-filter fragment helpers for all four stages, fixed/programmed and quad paths, and inspect SPIR-V `OpStore` operands for image/sampler/sampled-image types. Host renderer tests and ARM64 build pass; scissor fix exercised by on-device validation during startup/loading.
- Installed the matching-certificate ARM64 APK in place, then repeated core + synchronization validation with Khronos 1.4.363.1: startup/loading and Iceland gameplay with original barriers, then a separate launch with narrow barriers. Both have **zero Validation Error messages and zero SYNC-HAZARD messages**, with Synchronization explicitly confirmed enabled. The two earlier VUIDs did not recur. This establishes clean validation only for the exercised paths, not all game scenes/devices. Logs/screenshots/summary: ignored `out/vulkan-perf/validation/fixes-*`.
- Restored all original global debug settings and layer properties, removed temporary validation libraries from phone, restored original barriers/profiling off/tracing off and relaunched normally. Saves/settings/imported game code and caches untouched.

Stutter and compile-hitch measurement (Adreno 710, profiling on, opt-in `debug.scda.vk_cold_cache` exists but was not used):
- Run 1 after the SPIR-V source change (partly stale cache, ~200 s): 60 shader compiles/1.86 s, 334 pipelines/1.96 s; every gameplay hitch of 200-741 ms had compiles. Run 2, same route with warm cache (~155 s): 1 shader/278 ms (new video/menu content), 303 pipelines/0.37 s; ~1.8 s lost in compile frames versus ~5.0 s.
- Remaining large hitches are load/save/autosave and video transitions (title waits up to 326 ms for a flush and 516 ms for a read-back), not compilation. ~350 frames at 25-33 ms are 120 Hz pacing with a GPU near 12-18 ms per frame.
- Decision: an RPCS3-style asynchronous interpreter/fallback (estimated 4-7 weeks) is not worthwhile for repeat play. Possible later: record/warm pipeline recipes for first launch after shader-source changes.

Scaled presentation (now the PERFORMANCE menu row SCREEN UPSCALING, default DISPLAY; live):
- Swapchain uses the window aspect at `render_percent` (50%: 610x1356 instead of 1220x2712). SurfaceFlinger reports `Device` composition, so the display hardware stretches it without a GPU composition pass. Recreated when the render scale changes.
- Stationary Iceland, 50%, live off/on/off/on: GPU 16.18/14.84/16.31/14.82 ms, FPS 56.76/61.37/56.77/61.58; draws unchanged (~561). Screenshot differences on/off match off/off noise (moving sun glow only); zoomed scene and HUD crops equivalent because the game renders into its 813x610 target anyway.
- Perfetto GPU counters, same comparison: write bandwidth 551 -> 460 MB/s median, fragments 744 -> 618 M/s; clocks unchanged at ~940 MHz.
- Not verified: rotation/resume and menus with the option on; at 100% render scale the swapchain is unchanged.

GPU counter profile (Android GPU Inspector 3.3.3 counter list, Perfetto `gpu.counters`, stationary Iceland 50%):
- GPU at ~940 MHz; shaders busy 87%; 90-93% of time shading fragments; texture fetch stall ~39-45%; texture L1 miss ~40-45%; texture pipes busy ~70%; ALU capacity used only ~33%. Bus busy ~5% and system-memory stall ~0.1%.
- So the scene is fragment/texture-fetch limited, not memory-bandwidth or ALU limited. This weakens the earlier hypothesis that repeated tile load/store per pass dominates. `gpu.renderstages` produced no slices on this driver without the AGI layer.
- Texture statistics (`vk-textures`, profiling only), same scene: ~1,190 binds/frame = ~655 BC (stay compressed; `BC textures yes`), ~437 uncompressed, ~99 render targets. ~445 with mips, ~647 without (average ~48k texels), ~52 without mips at >=256x256 (as the game supplies them). No BC mip chains dropped for non-multiple-of-4 sizes; no anisotropic samplers.
- All ~437 uncompressed binds are console A8R8G8B8 (32-bit), average ~2,660 texels (~52x52). Native 16/8-bit host formats would gain nothing here; not implemented. Mip generation or runtime compression would change the image and were not pursued.
- Per-pass GPU time by vision mode (per-pass timestamps on, stationary Iceland 50%, A on): total 15.4 / 14.7 / 8.4 / 8.2 ms for normal / night / thermal / EMF. Main scene 10.6 / 10.7 / 5.7 / 6.2 ms. Glow chain 1 (325 -> 20 px, all modes) 2.6 / 2.6 / 1.9 / 1.2 ms; its first 325x325 shrink of the scene alone ~0.8 ms in every mode (four stages, each a 2x2 box tap = 16 reads per pixel). Chain 2 with 16x16 read-back (probably eye adaptation) only in normal view, ~1.0 ms. Passes per frame 36 / 28 / 11 / 13. Effect-to-pass mapping inferred from traces/mode differences.
- Image-changing effect options, now in the in-game menu (Recomp Settings > PERFORMANCE, Android only, live; `scda.toml` keys in brackets): SCREEN UPSCALING `DISPLAY (FASTER)`/`GPU` [`scaled_presentation`, default on], GLOW RESOLUTION `ORIGINAL (FASTER)`/`HIGH` [`native_effect_size` = E1, default on], GLOW QUALITY `HIGH (DEFAULT)`/`FAST (MAY FLICKER)` [`fast_glow` = E2, default off]. The former debug properties were removed.
  - E1: colour targets of at most 256 console pixels per side and smaller than the screen keep the console's size instead of the render scale. Copies between targets of different scale use a filtered `vkCmdBlitImage`. Switching rebuilds targets.
  - E2: no box-averaged taps when an upscaled target is shrunk into a smaller one (single bilinear read per stage).
  - Live base/E1/E2/E3/base, GPU ms: normal 15.39/14.47/14.27/13.60/15.41 (E3 -1.79 ms, FPS 58.2 -> 64.4); night 14.68/14.06/13.89/13.30/14.69 (E3 -1.38 ms). Thermal/EMF are not GPU bound (85-105 FPS at 120 Hz): Android lowers the GPU clock with less work (thermal E3 739 vs 873 MHz, EMF E3 794 vs 864 MHz), so frame time stays ~8.2-9.5 ms and is not a fair measure there.
  - Screenshots in all four modes look equivalent to the agent; pixel differences from baseline are close to baseline/baseline noise (animated sun glow and vision-mode noise). User first approved E3, then saw the sun's reflection flicker with E2 while playing; E1 alone does not flicker. At 100% render resolution: none 15 FPS, E1 19 FPS, E3 25 FPS (user-reported). Hence E1 default on, E2 opt-in. User confirmed menu, translations and live switching on the phone. Not verified: other levels' small effect targets, desktop build of the menu change.
- Maybe later: C) do not store depth after its final use in a frame; D) merge/reorder passes. Neither started; C requires proving no later depth read; D can change output.

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
