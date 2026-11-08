#include "android/vulkan_driver.h"

#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <jni.h>
#include <unistd.h>
#include <volk.h>
#include <filesystem>
#include <fstream>
#include "android/turnip_detect.h"
#include "core/log.h"
#ifdef __aarch64__
#include <adrenotools/driver.h>
#endif

namespace android_driver {
namespace {
bool g_turnip = false;
std::filesystem::path ChoiceFile() { return std::filesystem::path(SDL_GetAndroidInternalStoragePath()) / "turnip-enabled"; }
std::string NativeDir() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    jclass cls = env->GetObjectClass(activity);
    jobject info = env->CallObjectMethod(activity, env->GetMethodID(cls, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;"));
    jclass info_cls = env->GetObjectClass(info);
    auto path = static_cast<jstring>(env->GetObjectField(info, env->GetFieldID(info_cls, "nativeLibraryDir", "Ljava/lang/String;")));
    const char* chars = env->GetStringUTFChars(path, nullptr);
    std::string result(chars);
    env->ReleaseStringUTFChars(path, chars);
    env->DeleteLocalRef(path);
    env->DeleteLocalRef(info_cls);
    env->DeleteLocalRef(info);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return result + "/";
}
}

bool Supported() { return turnip::Supported(); }
bool Requested() { return std::ifstream(ChoiceFile()).get() == '1'; }
bool Select(bool turnip) {
    if (turnip && !Supported()) return false;
    const auto path = ChoiceFile();
    const auto tmp = path.string() + ".tmp";
    { std::ofstream f(tmp); f << (turnip ? '1' : '0'); f.close(); if (!f) return false; }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}
bool IsTurnip() { return g_turnip; }

bool Initialize() {
    // Kept for the process lifetime: Vulkan handles must never outlive this loader.
    static void* library = nullptr;
    if (library) return true;
#ifdef __aarch64__
    if (Requested() && !Supported())
        LOGW("vulkan: Turnip requested but GPU chip 0x%08x is not in the bundled driver's list; using manufacturer",
             turnip::ChipId());
    if (Requested() && Supported()) {
        auto dir = NativeDir();
        library = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
            nullptr, dir.c_str(), dir.c_str(), "libvulkan_freedreno.so", nullptr, nullptr);
        g_turnip = library != nullptr;
        if (!library) LOGW("vulkan: Turnip loader failed; using manufacturer driver");
    }
#endif
    if (!library) library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    auto get = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr")) : nullptr;
    if (!get) { LOGE("vulkan: cannot load vkGetInstanceProcAddr"); return false; }
    volkInitializeCustom(get);
    LOGI("vulkan: GPU chip 0x%08x, Turnip %s; requested %s, loader %s (restart required to change)", turnip::ChipId(),
         Supported() ? "supported" : "unsupported", Requested() ? "Turnip" : "manufacturer",
         g_turnip ? "Turnip universal v4.5" : "manufacturer");
    return true;
}
}  // namespace android_driver
