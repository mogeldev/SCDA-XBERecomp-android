// The Android host: what app/main.cpp and the Windows-only app files are on Windows. libmain.so loads the game code
// .so, which brings this library in, and calls ScdaMain on SDL's main thread.
#include <sys/system_properties.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>

#include "app/app.h"
#include "audio/output.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
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

std::string Property(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}

}  // namespace

namespace app {

// Android/data/<package>/files: the player copies the game into assets there over USB, logs and saves stay beside it
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

namespace gpu {

namespace {

// shortcut: draws nothing until the Vulkan backend exists, so the game runs headless
class NullBackend : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override {
        LOGI("gpu: no renderer yet, %ux%u frames are dropped", width, height);
        return true;
    }
    void Clear(const State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}
    void Draw(const State&, uint32_t, uint32_t, uint32_t, const uint8_t*) override {}
    void Copy(const Surface&, const Surface&, uint32_t, uint32_t, uint32_t) override {}
    void Present(const Surface&, bool) override {}
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<NullBackend>(); }
void SavePipelineCache() {}

}  // namespace gpu


// code: the game code .so as libmain.so loaded it
extern "C" __attribute__((visibility("default"))) int ScdaMain(const char* code) {
    SDL_AddEventWatch(Lifecycle, nullptr);
    logx::Init((app::ExeDir() / "logs").string(), "scda", false);
    app::Load();
    app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;
    if (!dispatch::Load(code)) {
        LOGE("game code: %s has not the tables the host needs", code);
        return 1;
    }
    g_window = SDL_CreateWindow("Splinter Cell: Double Agent", 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN);
    if (!g_window) {
        LOGE("window: %s", SDL_GetError());
        return 1;
    }
    input::Init(g_window);
    input::LoadBindings();
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
