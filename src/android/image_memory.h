#pragma once

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cassert>
#include <vector>

namespace gpu {

// Only optimal-tiled images share these blocks; buffers/linear images stay separate.
class ImageMemory {
public:
    struct Allocation {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0, size = 0;
    };

    bool Allocate(VkDevice dev, const VkMemoryRequirements& req, uint32_t type,
                  bool dedicated, VkImage image, Allocation& out) {
        constexpr VkDeviceSize kBlockSize = 32ull << 20;
        if (!dedicated && req.size <= kBlockSize / 2) {
            for (auto& b : blocks_)
                if (!b.dedicated && b.type == type && Take(b, req, out)) return true;
        }
        VkMemoryDedicatedAllocateInfo di{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        di.image = image;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = dedicated ? &di : nullptr;
        ai.memoryTypeIndex = type;
        ai.allocationSize = dedicated || req.size > kBlockSize / 2 ? req.size : kBlockSize;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkResult r = vkAllocateMemory(dev, &ai, nullptr, &memory);
        // A small image may still fit when a whole block no longer fits in the heap.
        if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY && ai.allocationSize != req.size) {
            ai.allocationSize = req.size;
            r = vkAllocateMemory(dev, &ai, nullptr, &memory);
        }
        if (r != VK_SUCCESS) return false;
        blocks_.push_back({memory, type, dedicated, 0, {{0, ai.allocationSize}}});
        bool ok = Take(blocks_.back(), req, out);
        assert(ok);
        return ok;
    }

    // Called only after the image is destroyed and its GPU work has completed.
    void Free(VkDevice dev, Allocation& a) {
        if (!a.memory) return;
        auto it = std::find_if(blocks_.begin(), blocks_.end(), [&](const Block& b) { return b.memory == a.memory; });
        assert(it != blocks_.end());
        if (--it->live == 0) {
            vkFreeMemory(dev, it->memory, nullptr);
            blocks_.erase(it);
        } else {
            auto& ranges = it->free;
            ranges.push_back({a.offset, a.size});
            std::sort(ranges.begin(), ranges.end(), [](Range x, Range y) { return x.offset < y.offset; });
            for (size_t i = 1; i < ranges.size();) {
                if (ranges[i - 1].offset + ranges[i - 1].size == ranges[i].offset) {
                    ranges[i - 1].size += ranges[i].size;
                    ranges.erase(ranges.begin() + i);
                } else ++i;
            }
        }
        a = {};
    }

private:
    struct Range { VkDeviceSize offset, size; };
    struct Block {
        VkDeviceMemory memory;
        uint32_t type;
        bool dedicated;
        uint32_t live;
        std::vector<Range> free;
    };
    static bool Take(Block& b, const VkMemoryRequirements& req, Allocation& out) {
        for (size_t i = 0; i < b.free.size(); ++i) {
            Range r = b.free[i];
            VkDeviceSize padding = (req.alignment - r.offset % req.alignment) % req.alignment;
            if (padding > r.size || req.size > r.size - padding) continue;
            VkDeviceSize offset = r.offset + padding, end = offset + req.size;
            b.free.erase(b.free.begin() + i);
            if (padding) b.free.push_back({r.offset, padding});
            if (end < r.offset + r.size) b.free.push_back({end, r.offset + r.size - end});
            out = {b.memory, offset, req.size};
            ++b.live;
            return true;
        }
        return false;
    }
    std::vector<Block> blocks_;
};

}  // namespace gpu
