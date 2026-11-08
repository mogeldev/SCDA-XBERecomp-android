// Is this device's GPU in the bundled Turnip's device table? Reads the Adreno chip id from KGSL (no Vulkan driver
// is loaded) and matches it as Mesa's dev_id_compare does for the KGSL backend.
#pragma once
#include <cstdint>

#include "android/turnip_supported.h"

#if defined(__ANDROID__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace turnip {

inline bool Matches(uint32_t chip) {
    // tu_knl_kgsl.cc: gpu_id from the chip id, chip_id is the 32-bit KGSL value (no fuse bits)
    const uint32_t gpu = ((chip >> 24) & 0xff) * 100 + ((chip >> 16) & 0xff) * 10 + ((chip >> 8) & 0xff);
    constexpr uint64_t kFuse = 0x0000ffff00000000ull;
    auto wild = [](uint64_t ref, uint64_t id) { return (ref & 0xff) == 0xff && (ref & 0xffffff00) == (id & 0xffffff00); };
    for (const TurnipDevice& r : kTurnipDevices) {
        if (r.gpu_id) {
            if (r.gpu_id == gpu) return true;
            continue;
        }
        if (r.chip_id == chip || wild(r.chip_id, chip)) return true;
        if ((r.chip_id & kFuse) == kFuse && (r.chip_id == (chip | kFuse) || wild(r.chip_id, chip | kFuse))) return true;
    }
    return false;
}

// 0 when there is no readable Qualcomm KGSL device
inline uint32_t ChipId() {
#if defined(__ANDROID__)
    struct DevInfo {  // kgsl_devinfo
        unsigned int device_id, chip_id, mmu_enabled;
        unsigned long gmem_gpubaseaddr;
        unsigned int gpu_id;
        unsigned long gmem_sizebytes;
    } info{};
    struct Prop {  // kgsl_device_getproperty
        unsigned int type;
        void* value;
        unsigned long sizebytes;
    } prop{1 /* KGSL_PROP_DEVICE_INFO */, &info, sizeof(info)};
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    int r = ioctl(fd, _IOWR(0x09, 0x2, Prop), &prop);
    close(fd);
    return r == 0 ? info.chip_id : 0;
#else
    return 0;
#endif
}

// ARM64 only: the bundled driver and adrenotools are ARM64 builds
inline bool Supported() {
#if defined(__aarch64__)
    static const bool ok = [] { uint32_t c = ChipId(); return c && Matches(c); }();
    return ok;
#else
    return false;
#endif
}

}  // namespace turnip
