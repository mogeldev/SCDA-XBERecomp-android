#pragma once

#include <vulkan/vulkan.h>

namespace gpu {

// Generated vertex shaders only read uniforms; image reads happen in fragment shaders.
inline void RenderPassBarrier(VkCommandBuffer cmd) {
    constexpr VkPipelineStageFlags attachments = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkPipelineStageFlags sources = attachments | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    constexpr VkPipelineStageFlags consumers = sources | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    // Fragment execution is included even without source read access: sampled images may be overwritten next.
    // Keep global memory scope for target aliases; transfer completion barriers remain separate.
    vkCmdPipelineBarrier(cmd, sources, consumers, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

}  // namespace gpu
