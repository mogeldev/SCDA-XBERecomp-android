// The Android host: what app/main.cpp and the Windows-only app files are on Windows. libmain.so loads the game code
// .so, which brings this library in, and calls ScdaMain on SDL's main thread.
#include <jni.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>

#include "app/app.h"
#include "audio/output.h"
#include "core/input.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace dispatch {
bool Load(const std::filesystem::path& dll);  // cpu/dispatch.cpp
}

namespace {

SDL_Window* g_window = nullptr;
// set from SDL's lifecycle notices as they happen: the main thread blocks in the event pump while in the background
std::atomic<bool> g_background{false};

bool SDLCALL Lifecycle(void*, SDL_Event* e) {
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) g_background = true;
    else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) g_background = false;
    return true;
}

void JNICALL TouchSet(JNIEnv*, jclass, jint buttons, jint left, jint right, jint lx, jint ly, jint rx, jint ry) {
    input::SetTouch(uint32_t(buttons), uint8_t(left), uint8_t(right), int16_t(lx), int16_t(ly), int16_t(rx),
                    int16_t(ry));
}

jint JNICALL TouchOpacity(JNIEnv*, jclass) { return input::TouchOpacity(); }

jboolean JNICALL PhoneWanted(JNIEnv*, jclass) {
    std::lock_guard lock(app::UiLock());
    const auto& s = app::Get().gyro;
    return s.enabled && s.source == gyro::kPhone;
}
void JNICALL PhoneReset(JNIEnv*, jclass, jboolean active) { gyro::ResetPhone(active); }
void JNICALL PhoneSample(JNIEnv*, jclass, jboolean accel, jfloat x, jfloat y, jfloat z, jlong ns, jint rotation) {
    if (ns > 0) gyro::PhoneSample(accel, x, y, z, uint64_t(ns), rotation);
}

// the on-screen controller (TouchControls.java) over SDL's view; it shows itself when the settings say
void AttachTouchControls() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    // the app's class loader: this library came in through dlopen
    jobject loader = env->CallObjectMethod(
        activity, env->GetMethodID(env->GetObjectClass(activity), "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass loader_class = env->FindClass("java/lang/ClassLoader");
    auto touch = static_cast<jclass>(env->CallObjectMethod(
        loader, env->GetMethodID(loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"),
        env->NewStringUTF("io.github.mogeldev.scda.TouchControls")));
    const JNINativeMethod natives[] = {
        {const_cast<char*>("nativeSet"), const_cast<char*>("(IIIIIII)V"), reinterpret_cast<void*>(TouchSet)},
        {const_cast<char*>("nativeOpacity"), const_cast<char*>("()I"), reinterpret_cast<void*>(TouchOpacity)},
    };
    if (!env->ExceptionCheck() && touch && env->RegisterNatives(touch, natives, 2) == JNI_OK)
        env->CallStaticVoidMethod(touch, env->GetStaticMethodID(touch, "attach", "(Landroid/app/Activity;)V"),
                                  activity);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("touch controls: not attached");
    }
    auto phone = static_cast<jclass>(env->CallObjectMethod(
        loader, env->GetMethodID(loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"),
        env->NewStringUTF("io.github.mogeldev.scda.PhoneGyro")));
    const JNINativeMethod phone_natives[] = {
        {const_cast<char*>("nativeWanted"), const_cast<char*>("()Z"), reinterpret_cast<void*>(PhoneWanted)},
        {const_cast<char*>("nativeReset"), const_cast<char*>("(Z)V"), reinterpret_cast<void*>(PhoneReset)},
        {const_cast<char*>("nativeSample"), const_cast<char*>("(ZFFFJI)V"), reinterpret_cast<void*>(PhoneSample)},
    };
    if (!env->ExceptionCheck() && phone && env->RegisterNatives(phone, phone_natives, 3) == JNI_OK)
        env->CallStaticVoidMethod(phone, env->GetStaticMethodID(phone, "attach", "(Landroid/app/Activity;)V"), activity);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("phone gyro: not attached");
    }
}

std::string Property(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}

}  // namespace

namespace app {

// Android/data/<package>/files: imported game data is under game/, while logs and saves stay beside it
std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = SDL_GetAndroidExternalStoragePath();
    return dir;
}

int Instance() { return 0; }
void* Window() { return g_window; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

// the window always fills the screen
void ApplyDisplay() {}
void OpenGameWindow(bool) {}

// no boot splash, handover or menu cursor on Android
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}
void* MenuCursor() { return nullptr; }

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("Splinter Cell: Double Agent XBE Recomp V%d, Android (%s), %s", kVersion, IMAGE_XBE, date);
    LOGR("Path: %s", ExeDir().c_str());
    LOGR("Device: %s %s, Android %s, %s", Property("ro.product.manufacturer").c_str(), Property("ro.product.model").c_str(),
         Property("ro.build.version.release").c_str(), Property("ro.soc.model").c_str());
    LOGR("CPU: %u threads, RAM: %.1f GB", std::thread::hardware_concurrency(), SDL_GetSystemRAM() / 1024.0);
    Mode desk = Desktop();
    LOGR("Screen: %ux%u", desk.width, desk.height);
    LogSettings();
}

// in the background the game and its sound stop: there is no window to draw to
void WaitWhileUnfocused() {
    if (!g_background) return;
    LOGI("background: paused");
    audio::SetPaused(true);
    kernel::HoldGuestTime(true);
    while (g_background) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    kernel::HoldGuestTime(false);
    audio::SetPaused(false);
    LOGI("foreground: resumed");
}

// A mission starts as a quick reboot into the title (kernel::Relaunch). A fresh process does that: Relaunch.java, in
// its own process, kills this one and starts the game again, which finds the launch data in hdd. Versus is not part
// of the Android port.
void Restart(const char* exe) {
    if (std::string_view(exe) != "SCDA") {
        std::remove(kernel::g_launch_file.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Splinter Cell: Double Agent",
                                 "Versus is not part of the Android version.", g_window);
        return;
    }
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    jclass intent_class = env->FindClass("android/content/Intent");
    jobject intent = env->NewObject(intent_class, env->GetMethodID(intent_class, "<init>", "()V"));
    env->CallObjectMethod(intent,
                          env->GetMethodID(intent_class, "setClassName",
                                           "(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;"),
                          activity, env->NewStringUTF("io.github.mogeldev.scda.Relaunch"));
    env->CallObjectMethod(intent,
                          env->GetMethodID(intent_class, "putExtra", "(Ljava/lang/String;I)Landroid/content/Intent;"),
                          env->NewStringUTF("pid"), jint(getpid()));
    env->CallVoidMethod(activity,
                        env->GetMethodID(env->GetObjectClass(activity), "startActivity", "(Landroid/content/Intent;)V"),
                        intent);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("relaunch: the restart activity did not start");
    }
}

uint32_t RefreshRate() {
    const SDL_DisplayMode* mode = g_window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window)) : nullptr;
    return mode && mode->refresh_rate > 1 ? uint32_t(mode->refresh_rate + 0.5f) : 0;
}

// as app/main.cpp's
void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    if (cap == kFpsCapRefresh) cap = RefreshRate();
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

// code: the game code .so as libmain.so loaded it
extern "C" __attribute__((visibility("default"))) int ScdaMain(const char* code) {
    SDL_AddEventWatch(Lifecycle, nullptr);
    // after a mission relaunch the log goes on in the same file
    bool relaunched = std::filesystem::exists(app::ExeDir() / "hdd" / "launch.bin");
    logx::Init((app::ExeDir() / "logs").string(), "scda", relaunched);
    app::Load();
    app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;
    if (!dispatch::Load(code)) {
        LOGE("game code: %s has not the tables the host needs", code);
        return 1;
    }
    // Own the Vulkan loader/surface; SDL supplies only the native window (no second system loader).
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Splinter Cell: Double Agent");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
    g_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!g_window) {
        LOGE("window: %s", SDL_GetError());
        return 1;
    }
    // Settings/logging can ask for the aspect before the window exists: discard that stand-in output size.
    app::g_display_applied++;
    input::Init(g_window);
    input::LoadBindings();
    AttachTouchControls();
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Splinter Cell: Double Agent",
                                 "The game failed to start, see the log in the logs folder.", g_window);
        return 1;
    }

    // The game runs on its own threads. This one pumps SDL, which keeps the controllers' state current; the
    // controller notices are left for input.cpp, the rest nobody reads.
    for (;;) {
        SDL_PumpEvents();
        if (SDL_HasEvent(SDL_EVENT_QUIT) || SDL_HasEvent(SDL_EVENT_TERMINATING)) break;
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_GAMEPAD_BUTTON_UP);
        SDL_FlushEvents(SDL_EVENT_GAMEPAD_REMAPPED, SDL_EVENT_LAST);
        SDL_Delay(4);
    }
    logx::Flush();
    kernel::Shutdown(0);
}
