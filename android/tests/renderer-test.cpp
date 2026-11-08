// Vulkan bookkeeping mocks plus real glslang compilation; no GPU or game assets required.
#include "android/descriptor_sets.h"
#include "android/frame_fences.h"
#include "android/image_memory.h"
#include "android/pass_barrier.h"
#include "android/render_clears.h"
#include "android/texture_stages.h"
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
static unsigned pass_barriers = 0;

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t n, const VkMemoryBarrier* barriers, uint32_t buffers,
    const VkBufferMemoryBarrier*, uint32_t images, const VkImageMemoryBarrier*) {
    constexpr VkPipelineStageFlags attachments = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    assert(src == (attachments | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)); // WAR: fragment reads finish too
    assert(dst == (src | VK_PIPELINE_STAGE_TRANSFER_BIT));
    assert(!flags && n == 1 && !buffers && !images); // global scope covers aliases, not BY_REGION
    assert(barriers[0].sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    assert(barriers[0].srcAccessMask == (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
    assert(barriers[0].dstAccessMask == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT));
    ++pass_barriers;
}

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

static void TextureStageTest() {
    gpu::State fixed{};
    fixed.tss[0][gpu::kTssColorOp] = 4;
    fixed.tss[1][gpu::kTssColorOp] = 1;
    fixed.tss[2][gpu::kTssColorOp] = 4;
    assert(gpu::SampledTextureStages(fixed) == 1); // disabled stage terminates the fixed-function chain
    fixed.tss[1][gpu::kTssColorOp] = 4;
    assert(gpu::SampledTextureStages(fixed) == 7);
    fixed.tss[0][gpu::kTssColorOp] = 0;
    assert(gpu::SampledTextureStages(fixed) == 0);
    for (uint32_t mode = 0; mode < 32; mode++) {
        bool no_fetch = mode == 0 || mode == 4 || mode == 5 || mode == 10 || mode == 17;
        assert(gpu::TextureModeSamples(mode) == !no_fetch);
        if (!no_fetch) continue;
        for (uint32_t stage = 0; stage < 4; stage++) {
            gpu::State state{};
            state.ps = std::make_shared<gpu::PixelShader>();
            state.rs[gpu::kRsPsTextureModes] = mode << (stage * 5);
            gpu::TexInfo bound[4]{}, unbound[4]{};
            bound[stage].kind = gpu::kTex2D;
            assert(gpu::SampledTextureStages(state) == 0);
            auto before = gpu::MakePixelKey(state, bound), after = gpu::MakePixelKey(state, unbound);
            assert(before.tex_modes == after.tex_modes); // coordinate/discard stages must not disappear
            for (auto key : {before, after}) {
                std::string source = gpu::PixelHlsl(key);
                assert(source.find("tex" + std::to_string(stage) + ".Sample") == std::string::npos);
            }
        }
    }
}

static void ClearTest() {
    VkClearRect r{{{0, 0}, {813, 610}}, 0, 1};
    assert(gpu::FullAttachmentClear(&r, 1, 813, 610));
    assert(!gpu::FullAttachmentClear(nullptr, 0, 813, 610));
    assert(!gpu::FullAttachmentClear(&r, 2, 813, 610));
    assert(!gpu::FullAttachmentClear(&r, 1, 814, 610));
    r.rect.offset.x = 1; assert(!gpu::FullAttachmentClear(&r, 1, 813, 610)); r.rect.offset.x = 0;
    r.rect.offset.y = -1; assert(!gpu::FullAttachmentClear(&r, 1, 813, 610)); r.rect.offset.y = 0;
    r.rect.extent.height--; assert(!gpu::FullAttachmentClear(&r, 1, 813, 610)); r.rect.extent.height++;
    r.baseArrayLayer = 1; assert(!gpu::FullAttachmentClear(&r, 1, 813, 610)); r.baseArrayLayer = 0;
    r.layerCount = 2; assert(!gpu::FullAttachmentClear(&r, 1, 813, 610));
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

static std::vector<uint32_t> CompileShader(const std::string& text, EShLanguage stage) {
    glslang::TShader shader(stage);
    const char* src = text.c_str();
    shader.setStrings(&src, 1);
    shader.setEntryPoint("main"); shader.setSourceEntryPoint("main");
    shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
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
    glslang::GlslangToSpv(*program.getIntermediate(stage), spv);
    return spv;
}

static void CompileVertex(const std::string& text) {
    auto spv = CompileShader(text, EShLangVertex);
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
    assert(gpu::NeedsScreenLinearWorkaround(0x13B5, "Mali-G51", 0x04800000));
    assert(!gpu::NeedsScreenLinearWorkaround(0x13B5, "Mali-G51", 0x05000000));
    assert(!gpu::NeedsScreenLinearWorkaround(0x13B5, "Mali-G76", 0x04800000));
    assert(!gpu::NeedsScreenLinearWorkaround(0x5143, "Adreno (TM) 710", 0x04800000));
    // Perspective interpolation of e*w, multiplied by interpolated 1/w, equals screen-linear e.
    const double weights[] = {0.2, 0.3, 0.5}, clip_w[] = {0.5, 2.0, 4.0}, values[] = {2.0, -3.0, 7.0};
    double reciprocal_w = 0, numerator = 0, expected = 0;
    for (int i = 0; i < 3; ++i) {
        reciprocal_w += weights[i] / clip_w[i];
        numerator += weights[i] * (values[i] * clip_w[i]) / clip_w[i];
        expected += weights[i] * values[i];
    }
    assert(std::abs((numerator / reciprocal_w) * reciprocal_w - expected) < 1e-12);
    glslang::InitializeProcess();
    for (bool pre : {false, true}) for (bool vertex : {false, true}) for (bool large : {false, true}) {
        auto original = gpu::FixedVertexHlsl(uint16_t(1 | (vertex ? 1 << 6 : 0)), pre);
        auto src = gpu::VulkanVertexHlsl(original, false, vertex, large);
        assert(original.find("PSIZE") == std::string::npos);
        assert(src.find(large ? (vertex ? "clamp(i.v6.x" : "clamp(hdr.z") : "o.point_size = 1.0;") != std::string::npos);
        // Same INF conversion as the renderer; no input-location conversion needed for this check.
        for (size_t p; (p = src.find("1.#INF")) != std::string::npos;) src.replace(p, 6, "asfloat(0x7F800000)");
        CompileVertex(src); CompileVertex(gpu::QuadHlsl(src));
        auto linear = gpu::VulkanScreenLinearHlsl(gpu::QuadHlsl(src));
        assert(linear.find("noperspective") == std::string::npos);
        assert(linear.find("o.e *= o.pos.w;") != std::string::npos);
        CompileVertex(linear);
    }
    auto src = gpu::VulkanVertexHlsl(gpu::VertexProgramHlsl({}, 1), true, false, true);
    assert(src.find("clamp(oPts.x") != std::string::npos);
    for (size_t p; (p = src.find("1.#INF")) != std::string::npos;) src.replace(p, 6, "asfloat(0x7F800000)");
    CompileVertex(src);
    assert(gpu::VulkanVertexHlsl("invalid", false, false, false).empty());
    for (bool programmable : {false, true}) for (bool shadow : {false, true})
    for (uint32_t stage = 0; stage < 4; stage++) for (bool quad : {false, true}) {
        gpu::PixelKey key{};
        key.programmable = programmable;
        key.quad = quad;
        for (uint32_t i = 0; i <= stage; i++) key.ff[i][0] = key.ff[i][4] = 4;
        key.tex[stage].kind = gpu::kTex2D;
        key.tex[stage].shadow = shadow ? (stage % 2 + 1) : 0;
        key.tex[stage].flags = shadow ? 0 : gpu::kTexWide;
        key.tex_modes = (shadow ? 2u : 1u) << (stage * 5);
        auto text = gpu::PixelHlsl(key);
        assert(text.find("shadow(Texture") == std::string::npos);
        assert(text.find("spreadtap(Texture") == std::string::npos);
        auto linear = gpu::VulkanScreenLinearHlsl(text);
        assert(linear.find("noperspective") == std::string::npos);
        assert((linear.find("i.e *= i.pos.w;") != std::string::npos) == quad);
        CompileShader(linear, EShLangFragment);
        auto spv = CompileShader(text, EShLangFragment);
        std::map<uint32_t, bool> opaque;
        std::map<uint32_t, uint32_t> value_type;
        for (size_t i = 5; i < spv.size(); i += spv[i] >> 16) {
            uint32_t op = spv[i] & 65535;
            if (op == 25 || op == 26 || op == 27) opaque[spv[i + 1]] = true; // image/sampler/sampled image
            if (op == 61 || op == 55 || op == 83) value_type[spv[i + 2]] = spv[i + 1]; // load/parameter/copy
            if (op == 62) assert(!opaque[value_type[spv[i + 2]]]); // OpStore must not store a resource
        }
    }
    glslang::FinalizeProcess();
}

int main() {
    gpu::RenderPassBarrier({}); assert(pass_barriers == 1);
    TextureStageTest(); ClearTest(); DescriptorTest(); MemoryTest(); FenceTest(); ShaderTest();
    std::puts("PASS: pass-barrier RAW/WAW/WAR/transfer scopes, unused texture stages/coordinate modes, full-clear eligibility, descriptor keys/reuse/frame reset, image pooling/alignment/reuse/dedicated/OOM, frame fences/reuse/thread race/errors, SPIR-V PointSize and resource-helper stores");
}
