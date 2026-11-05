// libmain.so, which SDLActivity runs: loads the game code .so, which brings libscda_host.so in, and hands over to the
// host (android/host.cpp). A release finds the code the player built in Android/data/<package>/files; a development
// build (SCDA_DEV_CODE) carries it.
#include <dlfcn.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr char kCode[] = "libscda_code.so";

int Fail(const std::string& text) {
    SDL_Log("%s", text.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Splinter Cell: Double Agent", text.c_str(), nullptr);
    return 1;
}

// Android runs no code from shared storage: the player's build is copied into the app's own folder when it changed
std::string CodePath() {
    std::error_code ec;
    fs::path root = SDL_GetAndroidExternalStoragePath();
    fs::path shared = root / "game" / kCode;
    if (!fs::exists(shared, ec)) shared = root / kCode; // Existing USB/ADB installs.
    fs::path own = fs::path(SDL_GetAndroidInternalStoragePath()) / kCode;
    if (fs::exists(shared, ec)) {
        auto stamp = fs::last_write_time(shared, ec);
        if (!fs::exists(own, ec) || fs::last_write_time(own, ec) != stamp || fs::file_size(own, ec) != fs::file_size(shared, ec)) {
            fs::copy_file(shared, own, fs::copy_options::overwrite_existing, ec);
            if (!ec) fs::last_write_time(own, stamp, ec);
        }
    }
    return fs::exists(own, ec) ? own.string() : kCode;
}

}  // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) return Fail(std::string("SDL: ") + SDL_GetError());
    std::string code = CodePath();
    void* lib = dlopen(code.c_str(), RTLD_NOW);
    if (!lib) {
        return Fail(std::string("The game code is missing or does not fit this version. Import a game ZIP with ") +
                    kCode + " from the matching player builder, or copy it into " + SDL_GetAndroidExternalStoragePath()
                    + ".\n\n" + dlerror());
    }
    auto entry = reinterpret_cast<int (*)(const char*)>(dlsym(lib, "ScdaMain"));
    if (!entry) return Fail("libscda_host.so has no ScdaMain");
    return entry(code.c_str());
}
