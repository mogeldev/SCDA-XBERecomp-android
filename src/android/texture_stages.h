#pragma once

#include "gpu/state.h"

namespace gpu {

// These Xbox texture modes generate coordinates/constants/discard, never a texture fetch.
inline bool TextureModeSamples(uint32_t mode) {
    return mode != 0 && mode != 4 && mode != 5 && mode != 0xA && mode != 0x11;
}

inline uint32_t SampledTextureStages(const State& state) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 4; i++) {
        if (state.ps) {
            if (TextureModeSamples((state.rs[kRsPsTextureModes] >> (i * 5)) & 0x1F)) mask |= 1u << i;
        } else {
            if (state.tss[i][kTssColorOp] <= 1) break;
            mask |= 1u << i;
        }
    }
    return mask;
}

}  // namespace gpu
