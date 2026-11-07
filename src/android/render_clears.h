#pragma once

#include <vulkan/vulkan.h>
#include <cstddef>

namespace gpu {

// Conservative full-clear detection for attachment-lifetime profiling.
inline bool FullAttachmentClear(const VkClearRect* rects, std::size_t count, uint32_t width, uint32_t height) {
    return count == 1 && rects[0].rect.offset.x == 0 && rects[0].rect.offset.y == 0 &&
           rects[0].rect.extent.width == width && rects[0].rect.extent.height == height &&
           rects[0].baseArrayLayer == 0 && rects[0].layerCount == 1;
}

}  // namespace gpu
