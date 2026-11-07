#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <functional>
#include <unordered_map>

namespace gpu {

// Dynamic offsets are supplied at bind time, not part of a descriptor's contents.
struct DescriptorKey {
    std::array<VkBuffer, 2> buffers{};
    std::array<VkDeviceSize, 2> ranges{};
    std::array<VkImageView, 4> views{};
    std::array<VkImageLayout, 4> layouts{};
    std::array<VkSampler, 4> samplers{};
    bool operator==(const DescriptorKey&) const = default;
};

struct DescriptorHash {
    size_t operator()(const DescriptorKey& key) const {
        size_t hash = 0;
        auto add = [&](auto value) {
            hash ^= std::hash<decltype(value)>{}(value) + size_t(0x9e3779b9) + (hash << 6) + (hash >> 2);
        };
        for (auto v : key.buffers) add(v);
        for (auto v : key.ranges) add(v);
        for (auto v : key.views) add(v);
        for (auto v : key.layouts) add(v);
        for (auto v : key.samplers) add(v);
        return hash;
    }
};

// Clear only after the frame fence, before its descriptor pools are reset.
using DescriptorSets = std::unordered_map<DescriptorKey, VkDescriptorSet, DescriptorHash>;

}  // namespace gpu
