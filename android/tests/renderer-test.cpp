// Vulkan bookkeeping mocks plus real glslang compilation; no GPU or game assets required.
#include "android/descriptor_sets.h"
#include "android/frame_fences.h"
#include "android/image_memory.h"
#include "android/vertex_hlsl.h"
#include "gpu/shaders.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <thread>

static uintptr_t next_handle = 1;
static std::map<VkDeviceMemory, VkDeviceSize> memory;
static std::map<VkFence, bool> pending;
static unsigned allocations = 0, waits = 0;
static bool fail_alloc = false, fail_block = false, fail_submit = false, fail_wait = false;
static VkImage dedicated_image = VK_NULL_HANDLE;

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo* a, const VkAllocationCallbacks*, VkDeviceMemory* out) {
    if (fail_alloc || (fail_block && a->allocationSize == (32ull << 20))) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    dedicated_image = a->pNext ? static_cast<const VkMemoryDedicatedAllocateInfo*>(a->pNext)->image : VK_NULL_HANDLE;
    *out = reinterpret_cast<VkDeviceMemory>(next_handle++);
    memory[*out] = a->allocationSize;
    ++allocations;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory m, const VkAllocationCallbacks*) { assert(memory.erase(m) == 1); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* out) {
    *out = reinterpret_cast<VkFence>(next_handle++); pending[*out] = false; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice, VkFence f, const VkAllocationCallbacks*) { pending.erase(f); }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence f) {
    if (fail_submit) return VK_ERROR_DEVICE_LOST;
    assert(!pending.at(f)); pending[f] = true; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice, uint32_t n, const VkFence* f, VkBool32, uint64_t) {
    if (fail_wait) return VK_ERROR_DEVICE_LOST;
    assert(n == 1 && pending.at(*f)); ++waits; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice, uint32_t n, const VkFence* f) {
    assert(n == 1 && pending.at(*f)); pending[*f] = false; return VK_SUCCESS;
}

static void DescriptorTest() {
    gpu::DescriptorKey key;
    key.buffers = {reinterpret_cast<VkBuffer>(1), reinterpret_cast<VkBuffer>(2)};
    key.ranges = {4096, 1024};
    key.views.fill(reinterpret_cast<VkImageView>(3));
    key.layouts.fill(VK_IMAGE_LAYOUT_GENERAL);
    key.samplers.fill(reinterpret_cast<VkSampler>(4));
    gpu::DescriptorSets sets;
    auto set = reinterpret_cast<VkDescriptorSet>(5);
    sets.emplace(key, set);
    auto same = key;
    assert(sets.at(same) == set); // new dynamic offsets must reuse this set
    assert(gpu::DescriptorHash{}(same) == gpu::DescriptorHash{}(key));
    for (size_t i = 0; i < 2; ++i) {
        auto changed = key;
        changed.buffers[i] = reinterpret_cast<VkBuffer>(10);
        assert(!sets.contains(changed));
        changed = key; changed.ranges[i]++;
        assert(!sets.contains(changed));
    }
    for (size_t i = 0; i < 4; ++i) {
        auto changed = key;
        changed.views[i] = reinterpret_cast<VkImageView>(10);
        assert(!sets.contains(changed));
        changed = key; changed.layouts[i] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        assert(!sets.contains(changed));
        changed = key; changed.samplers[i] = reinterpret_cast<VkSampler>(10);
        assert(!sets.contains(changed));
    }
    gpu::DescriptorSets other_frame;
    assert(!other_frame.contains(key));
    sets.clear(); // fence completed, pool will reset: never reuse its old handles
    assert(!sets.contains(key));
    auto new_set = reinterpret_cast<VkDescriptorSet>(6);
    sets.emplace(key, new_set);
    assert(sets.at(key) == new_set);
}

static void MemoryTest() {
    gpu::ImageMemory pool;
    using A = gpu::ImageMemory::Allocation;
    VkMemoryRequirements req{4096, 4096, 1};
    std::vector<A> images(5000);
    for (auto& a : images) assert(pool.Allocate({}, req, 0, false, {}, a));
    assert(allocations == 1); // more images than Vulkan's minimum memory-allocation count
    for (auto& a : images) pool.Free({}, a);
    assert(memory.empty());

    A a, b, c;
    req = {100, 256, 1};
    assert(pool.Allocate({}, req, 0, false, {}, a));
    assert(pool.Allocate({}, req, 0, false, {}, b));
    assert(b.offset == 256);
    assert(pool.Allocate({}, req, 1, false, {}, c));
    assert(c.memory != b.memory);
    pool.Free({}, a); pool.Free({}, b); pool.Free({}, c);
    assert(memory.empty());

    req = {4096, 4096, 1};
    std::vector<A> full(8192);
    for (auto& x : full) assert(pool.Allocate({}, req, 0, false, {}, x));
    // An exhausted block can satisfy 8 KiB only after the adjacent holes coalesce.
    pool.Free({}, full[0]); pool.Free({}, full[1]);
    req.size = 8192;
    assert(pool.Allocate({}, req, 0, false, {}, b));
    assert(b.memory == full[2].memory && b.offset == 0);
    pool.Free({}, b);
    for (size_t i = 2; i < full.size(); ++i) pool.Free({}, full[i]);
    assert(memory.empty());
    req.size = 40ull << 20;
    assert(pool.Allocate({}, req, 0, false, {}, a));
    assert(memory[a.memory] == req.size);
    pool.Free({}, a);
    req = {100, 256, 1};

    VkImage image = reinterpret_cast<VkImage>(1);
    assert(pool.Allocate({}, req, 0, true, image, a));
    assert(dedicated_image == image && a.offset == 0 && memory[a.memory] == req.size);
    assert(pool.Allocate({}, req, 0, false, {}, b));
    assert(a.memory != b.memory);
    pool.Free({}, a); pool.Free({}, b);
    fail_block = true;
    assert(pool.Allocate({}, req, 0, false, {}, a));
    assert(memory[a.memory] == req.size);
    pool.Free({}, a);
    fail_alloc = true;
    assert(!pool.Allocate({}, req, 0, false, {}, a) && !a.memory);
    fail_alloc = fail_block = false;

    std::mt19937 rng(17);
    images.clear();
    for (int i = 0; i < 10000; ++i) {
        if (!images.empty() && rng() % 3 == 0) {
            size_t index = rng() % images.size();
            pool.Free({}, images[index]); images.erase(images.begin() + index);
        } else {
            req = {1 + rng() % 65536, VkDeviceSize(1) << (rng() % 13), 1};
            assert(pool.Allocate({}, req, 0, false, {}, a));
            assert(a.offset % req.alignment == 0 && a.offset + a.size <= memory[a.memory]);
            for (const auto& other : images)
                assert(a.memory != other.memory || a.offset + a.size <= other.offset || other.offset + other.size <= a.offset);
            images.push_back(a);
        }
    }
    for (auto& x : images) pool.Free({}, x);
    assert(memory.empty());
}

static void FenceTest() {
    gpu::FrameFences<2> f;
    assert(f.Init(reinterpret_cast<VkDevice>(1)));
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    assert(f.Begin(0)); assert(f.Submit({}, 0, info)); f.Present(0);
    assert(f.WaitPresent(0)); assert(f.Completed() == 1 && waits == 1);
    assert(f.WaitPresent(0)); assert(waits == 1);
    assert(f.Begin(1)); assert(f.Submit({}, 1, info)); f.Present(1);
    assert(f.Begin(0)); assert(f.Submit({}, 0, info)); // mid-frame Flush, not a present
    assert(f.Begin(0));
    unsigned before = waits;
    assert(f.WaitPresent(1)); assert(waits == before && f.Completed() == 3);
    for (uint64_t n = 2; n < 2000; ++n) {
        assert(f.Begin(n % 2)); assert(f.Submit({}, n % 2, info)); f.Present(n);
        std::thread waiter([&] { assert(f.WaitPresent(n)); });
        assert(f.Begin(n % 2)); // race the game's wait with render-thread fence reset/reuse
        waiter.join();
        assert(f.Completed() == f.NextSerial() - 1);
    }
    uint64_t next = f.NextSerial();
    fail_submit = true;
    assert(!f.Submit({}, 0, info));
    assert(f.NextSerial() == next);
    fail_submit = false;
    assert(f.Submit({}, 0, info)); f.Present(2000);
    fail_wait = true;
    assert(!f.WaitPresent(2000));
    assert(f.Completed() < next);
    fail_wait = false; assert(f.Begin(0));
}

static void CompileVertex(const std::string& text) {
    glslang::TShader shader(EShLangVertex);
    const char* src = text.c_str();
    shader.setStrings(&src, 1);
    shader.setEntryPoint("main"); shader.setSourceEntryPoint("main");
    shader.setEnvInput(glslang::EShSourceHlsl, EShLangVertex, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    shader.setHlslIoMapping(true); shader.setAutoMapLocations(true);
    auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    glslang::TProgram program;
    bool ok = shader.parse(GetDefaultResources(), 100, false, messages);
    program.addShader(&shader);
    ok = ok && program.link(messages) && program.mapIO();
    if (!ok) std::fprintf(stderr, "%s\n%s\n%s\n", shader.getInfoLog(), program.getInfoLog(), text.c_str());
    assert(ok);
    std::vector<uint32_t> spv;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), spv);
    uint32_t point = 0; bool stored = false;
    for (size_t i = 5; i < spv.size(); i += spv[i] >> 16) {
        // OpDecorate <id> BuiltIn PointSize
        if ((spv[i] & 65535) == 71 && (spv[i] >> 16) == 4 && spv[i + 2] == 11 && spv[i + 3] == 1) point = spv[i + 1];
    }
    for (size_t i = 5; i < spv.size(); i += spv[i] >> 16)
        if ((spv[i] & 65535) == 62 && spv[i + 1] == point) stored = true; // OpStore
    assert(point && stored);
}

static void ShaderTest() {
    glslang::InitializeProcess();
    for (bool pre : {false, true}) for (bool vertex : {false, true}) for (bool large : {false, true}) {
        auto original = gpu::FixedVertexHlsl(uint16_t(1 | (vertex ? 1 << 6 : 0)), pre);
        auto src = gpu::VulkanVertexHlsl(original, false, vertex, large);
        assert(original.find("PSIZE") == std::string::npos);
        assert(src.find(large ? (vertex ? "clamp(i.v6.x" : "clamp(hdr.z") : "o.point_size = 1.0;") != std::string::npos);
        // Same INF conversion as the renderer; no input-location conversion needed for this check.
        for (size_t p; (p = src.find("1.#INF")) != std::string::npos;) src.replace(p, 6, "asfloat(0x7F800000)");
        CompileVertex(src); CompileVertex(gpu::QuadHlsl(src));
    }
    auto src = gpu::VulkanVertexHlsl(gpu::VertexProgramHlsl({}, 1), true, false, true);
    assert(src.find("clamp(oPts.x") != std::string::npos);
    for (size_t p; (p = src.find("1.#INF")) != std::string::npos;) src.replace(p, 6, "asfloat(0x7F800000)");
    CompileVertex(src);
    assert(gpu::VulkanVertexHlsl("invalid", false, false, false).empty());
    glslang::FinalizeProcess();
}

int main() {
    DescriptorTest(); MemoryTest(); FenceTest(); ShaderTest();
    std::puts("PASS: descriptor keys/reuse/frame reset, image pooling/alignment/reuse/dedicated/OOM, frame fences/reuse/thread race/errors, SPIR-V PointSize stores");
}
