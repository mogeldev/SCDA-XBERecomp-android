#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <mutex>

namespace gpu {

// Fence wait/reset/reuse must be serialized with WaitPresent on the game thread.
template<size_t Frames>
class FrameFences {
public:
    ~FrameFences() {
        if (!device_) return;
        for (auto& s : slots_)
            if (s.fence) vkDestroyFence(device_, s.fence, nullptr);
    }
    bool Init(VkDevice device) {
        device_ = device;
        VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (auto& s : slots_)
            if (vkCreateFence(device_, &ci, nullptr, &s.fence) != VK_SUCCESS) return false;
        return true;
    }
    bool Begin(size_t slot) {
        std::lock_guard lock(mutex_);
        auto& s = slots_[slot];
        if (s.pending) {
            if (!Wait(s) || vkResetFences(device_, 1, &s.fence) != VK_SUCCESS) return false;
            s.pending = false;
        }
        return true;
    }
    bool Submit(VkQueue queue, size_t slot, const VkSubmitInfo& info) {
        std::lock_guard lock(mutex_);
        auto& s = slots_[slot];
        if (vkQueueSubmit(queue, 1, &info, s.fence) != VK_SUCCESS) return false;
        s.serial = ++submitted_;
        s.pending = true;
        return true;
    }
    void Present(uint64_t n) {
        std::lock_guard lock(mutex_);
        presents_[n % presents_.size()] = submitted_;
    }
    bool WaitPresent(uint64_t n) {
        std::lock_guard lock(mutex_);
        uint64_t serial = presents_[n % presents_.size()];
        if (serial <= completed_.load()) return true;
        // A reused slot was already waited; otherwise its pending fence still covers this present.
        for (auto& s : slots_)
            if (s.pending && s.serial == serial) return Wait(s);
        return false;
    }
    uint64_t NextSerial() const { return submitted_ + 1; }  // render thread only
    uint64_t Completed() const { return completed_.load(); }

private:
    struct Slot { VkFence fence = VK_NULL_HANDLE; uint64_t serial = 0; bool pending = false; };
    bool Wait(const Slot& s) {
        // Do not retire resources or reset an unsignaled fence after a failed wait.
        if (vkWaitForFences(device_, 1, &s.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
        if (s.serial > completed_.load()) completed_.store(s.serial);
        return true;
    }
    VkDevice device_ = VK_NULL_HANDLE;
    std::array<Slot, Frames> slots_{};
    // The threaded backend runs at most one present ahead of its WaitPresent caller.
    std::array<uint64_t, Frames + 1> presents_{};
    uint64_t submitted_ = 0;
    std::atomic<uint64_t> completed_{0};
    std::mutex mutex_;
};

}  // namespace gpu
