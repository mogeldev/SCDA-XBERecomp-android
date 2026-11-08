// The title's Direct3D 8 calls drawn with Vulkan on Android: gpu/d3d12.cpp's renderer, without what the port leaves
// out (shader model 3, HDR, SMAA, ambient occlusion, MSAA, captures, the texture pack and the overlay). The generated
// HLSL goes through glslang to SPIR-V. Render targets stay in the GENERAL layout and a pass ends with one memory
// barrier: fewer layout transitions to get wrong, for a 640x480 title's few targets.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <sys/system_properties.h>
#include <vulkan/vulkan.h>

#include <dxgiformat.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <cassert>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "android/descriptor_sets.h"
#include "android/frame_fences.h"
#include "android/image_memory.h"
#include "android/pass_barrier.h"
#include "android/render_clears.h"
#include "android/texture_stages.h"
#include "android/vertex_hlsl.h"

namespace gpu {

namespace {

constexpr int kFrames = 2;
constexpr VkDeviceSize kRingSize = 48ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr VkDeviceSize kRingReserve = 8ull << 20;
constexpr uint32_t kSetsPerPool = 2048;
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kNone = UINT32_MAX;
constexpr uint32_t kTimedPasses = 128;
constexpr uint32_t kTimestampQueries = 2 + kTimedPasses * 2;

bool DebugFlag(const char* name, bool fallback = false) {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get(name, value) ? std::strcmp(value, "1") == 0 : fallback;
}

struct CpuTimer {
    using Clock = std::chrono::steady_clock;
    double* ms;
    Clock::time_point start;
    explicit CpuTimer(double* out) : ms(out), start(out ? Clock::now() : Clock::time_point{}) {}
    ~CpuTimer() {
        if (ms) *ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
};

void CheckGpu(bool ok) {
    if (ok) return;
    LOGE("vulkan: GPU submit/fence failed; stopping before reusing in-flight resources");
    std::abort();
}

// the full screen copy: src rectangle in uv, gamma lookup, alpha, n x n box filter, colour scale; xf places the quad
// in clip space (scale, offset), rot turns it to the panel's own orientation
const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; float4 xf; float4 rot; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2(id & 1, id >> 1);
    float2 c = (t * float2(2, -2) + float2(-1, 1)) * xf.xy + xf.zw;
    o.p = float4(c.x * rot.x + c.y * rot.y, c.x * rot.z + c.y * rot.w, 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// ---- the console's state in Vulkan terms, as d3d12.cpp reads it

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case kBlendDestColor: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER to ALWAYS are 0x200 to 0x207, in VkCompareOp's order
uint32_t Cmp(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }

VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// Vulkan has three border colours without VK_EXT_custom_border_color: the nearest, the colour before the alpha (a
// decal multiplied over a wall wants white past its edge, whatever its alpha)
VkBorderColor Border(uint32_t argb) {
    uint32_t rgb = ((argb >> 16) & 255) + ((argb >> 8) & 255) + (argb & 255);
    if (rgb > 3 * 127) return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    return (argb >> 24) < 128 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
}

// the vertex formats vertex.cpp hands over
VkFormat VertexFormat(uint8_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

bool IsDepth(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D32_SFLOAT ||
           f == VK_FORMAT_D16_UNORM;
}

bool HasStencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// ---- BC1 to BC3 to BGRA, for GPUs without BC textures (Mali, most Adreno)

void Rgb565(uint16_t c, uint8_t* bgr) {
    bgr[2] = uint8_t((c >> 11) * 255 / 31);
    bgr[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    bgr[0] = uint8_t((c & 31) * 255 / 31);
}

// one 8 byte colour block into 16 BGRA texels; four: BC2 and BC3 always use four colours
void ColorBlock(const uint8_t* b, bool four, uint8_t out[16][4]) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4] = {};
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (four || c0 > c1) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (!four && c0 <= c1) pal[3][3] = 0;
    uint32_t bits = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) std::memcpy(out[i], pal[(bits >> (i * 2)) & 3], 4);
}

void AlphaBlock(const uint8_t* b, uint8_t out[16][4]) {
    uint8_t a[8] = {b[0], b[1]};
    for (int i = 2; i < 8; i++) {
        if (a[0] > a[1]) a[i] = uint8_t(((8 - i) * a[0] + (i - 1) * a[1]) / 7);
        else a[i] = i < 6 ? uint8_t(((6 - i) * a[0] + (i - 1) * a[1]) / 5) : (i == 6 ? 0 : 255);
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (i * 8);
    for (int i = 0; i < 16; i++) out[i][3] = a[(bits >> (i * 3)) & 7];
}

// blocks: rows of pitch bytes; out: w x h BGRA
void DecodeBc(Host host, const std::vector<uint8_t>& blocks, uint32_t pitch, uint32_t w, uint32_t h,
              std::vector<uint8_t>& out) {
    uint32_t bb = host == Host::BC1 ? 8 : 16;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    out.assign(size_t(w) * h * 4, 0);
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            size_t at = size_t(by) * pitch + size_t(bx) * bb;
            if (at + bb > blocks.size()) return;
            const uint8_t* b = &blocks[at];
            uint8_t px[16][4];
            ColorBlock(host == Host::BC1 ? b : b + 8, host != Host::BC1, px);
            if (host == Host::BC2) {
                for (int i = 0; i < 16; i++) px[i][3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            } else if (host == Host::BC3) {
                AlphaBlock(b, px);
            }
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < w && y < h) std::memcpy(&out[(size_t(y) * w + x) * 4], px[i], 4);
            }
        }
    }
}

// ---- guest memory checks, as d3d12.cpp's

constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    ImageMemory::Allocation alloc;
    VkImageView view = VK_NULL_HANDLE;    // what shaders sample: the depth aspect of a depth format
    VkImageView attach = VK_NULL_HANDLE;  // depth and stencil, to draw to
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
};

struct Target {
    Image im;
    Image fb;  // the copy a draw samples while it draws to the target
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0;  // last lock seen
    uint64_t sig = 0;    // guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
};

bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

struct Tex {
    Image im;
    TextureDesc d;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
};

// one texture stage as a draw binds it
struct StageView {
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
};

struct DrawTextures {
    TexInfo info[4];
    StageView bind[4];
    uint32_t levels[4] = {};
    bool rt[4] = {};     // stage samples a render target
    float wide[4] = {};  // host texels per console texel of a target read into a smaller one, 0 otherwise
};

struct DrawShaders {
    bool prog = false;
    bool quad = false;
    bool fixed_transform = false;
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t pkey = 0;
};

struct GuestSize {
    uint32_t w;
    uint32_t h;
};

struct PipeKey {
    uint64_t vs, ps;
    uint32_t regs;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t strides[16];
    uint32_t buffers;
    uint32_t topo;  // VkPrimitiveTopology
    uint32_t rtfmt;
    uint32_t ds;
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc;
    uint32_t sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t cull, ccw, fill;
    int32_t zbias;
    float zslope;
};

// what a Blit draws
struct BlitArgs {
    VkImageView src = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    float uv[4] = {0, 0, 1, 1};
    bool gamma = false;
    bool alpha = false;
    uint32_t taps = 1;
    float mul = 1;
    float xf[4] = {1, 1, 0, 0};
    float rot[4] = {1, 0, 0, 1};
    VkFormat fmt = kBgra;  // the target's
    bool over = false;     // premultiplied over the target, which keeps its alpha
};

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;
constexpr uint32_t kQuadVertices = 6;
constexpr float kDepth24Max = 16777215.0f;
constexpr float kAlphaTestAlways = 7.0f;  // VK_COMPARE_OP_ALWAYS
constexpr float kUpscaledMin = 1.01f;
constexpr float kGlowBoxTexels = 3.0f;

uint64_t TargetKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

bool IsShadowSurface(const Surface& s, bool depth) {
    const ShadowTargets& sh = g_shadow;
    return sh.blur_hi && s.data == (depth ? sh.map : sh.color);
}

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable];
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kQuadSalt = 0xC3C30000;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

// A small draw whose every float2 input lies in 0..1: wrap reads as clamp there (d3d12.cpp's UnitTexcoords)
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss) {
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    }
    return false;
}

// ---- constants, as d3d12.cpp fills them

void FillSurfaceConstants(const State& s, GuestSize size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    if (s.rs[kRsFogEnable]) {
        c.fog[0] = Bits(s.rs[kRsFogStart]);
        c.fog[1] = Bits(s.rs[kRsFogEnd]);
        c.fog[2] = Bits(s.rs[kRsFogDensity]);
        c.fog[3] = float(s.rs[kRsFogTableMode]);
    }
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
    c.hdr[0] = c.hdr[1] = 1;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, Vulkan half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    c.texsize[i][2] = -0.5f / float(d.width);
    c.texsize[i][3] = -0.5f / float(d.height);
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width));
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height));
        c.spread[i][2] = n;
    }
}

void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestSize size, DrawConstants& c) {
    float wv[16], wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w;
    float sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1;
    float oy = 1 - (2.0f * v.y + v.h) / size.h;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
void LightToView(const std::array<uint32_t, 26>& l, const float* vm, float (&o)[7][4]) {
    float f[26];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[1 + j];
        o[1][j] = f[5 + j];
        o[2][j] = f[9 + j];
    }
    const float* pos = f + 13;
    const float* dir = f + 16;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[19];
    o[4][3] = f[20];
    o[5][0] = f[21];
    o[5][1] = f[22];
    o[5][2] = f[23];
    o[6][0] = std::cos(f[24] * 0.5f);
    o[6][1] = std::cos(f[25] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

// glslang reads the generated HLSL as fxc does, but for 1.#INF; vertex inputs take their register as location
std::string ForGlslang(std::string s) {
    for (size_t p; (p = s.find("1.#INF")) != std::string::npos;) s.replace(p, 6, "asfloat(0x7F800000)");
    static const std::regex kAttr(R"((float4|int4|uint) v(\d+) : ATTR\d+;)");
    return std::regex_replace(s, kAttr, "[[vk::location($2)]] $1 v$2 : ATTR$2;");
}

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front, bool pillarbox) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override {
        if (!DeliverLate(data)) Sync(data, 0);
        RecordLate(data);
    }
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void WaitGpuFrame(uint64_t n) override { CheckGpu(fences_.WaitPresent(n)); }
    void SetGamma(const uint8_t*) override {}
    void SavePipelines();

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer up = VK_NULL_HANDLE;   // uploads and new images, ahead of the draws
        VkCommandBuffer cmd = VK_NULL_HANDLE;  // the draws
        VkSemaphore acquired = VK_NULL_HANDLE;
        Buffer ring;
        VkDeviceSize used = 0;
        std::vector<VkDescriptorPool> pools;
        size_t set_pool = 0;  // the pool sets come from now
        DescriptorSets sets;
        VkQueryPool timestamps = VK_NULL_HANDLE;
        bool timestamps_pending = false;
        struct TimedPass {
            uint64_t key = 0;
            uint32_t draws = 0;
        };
        std::array<TimedPass, kTimedPasses> timed_passes{};
        uint32_t timed_count = 0;
        std::vector<Image> dead_images;
        std::vector<Buffer> dead_buffers;
    };
    struct Alloc {
        VkBuffer buf;
        VkDeviceSize off;
        uint8_t* cpu;
    };

    Frame& Cur() { return frames_[frame_ % kFrames]; }
    // Init, in order
    bool CreateInstance();
    bool CreateSurface();
    bool CreateDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateLayouts();
    bool CreateFrames();
    void LoadPipelineCache();
    void CreateDummies();
    // memory and objects
    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const;
    bool CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props);
    void DestroyBuffer(Buffer& b);
    struct ImageDesc {
        VkFormat fmt;
        uint32_t w, h, depth = 1, layers = 1, levels = 1;
        VkImageUsageFlags usage;
        VkImageViewType view = VK_IMAGE_VIEW_TYPE_2D;
    };
    bool CreateImage(Image& im, const ImageDesc& d);
    void DestroyImage(Image& im);
    void Release(Image& im) {
        if (im.image) Cur().dead_images.push_back(im);
        im = {};
    }
    Alloc Upload(VkDeviceSize size, VkDeviceSize align);
    void InitTargetImage(Image& im, bool clear);
    // frames
    void BeginFrame();
    void Submit(VkSemaphore wait, VkSemaphore signal);
    // passes
    void BeginRendering(Target* rt, Target* ds);
    void BeginRenderingView(VkImageView view, VkFormat fmt, VkImageLayout layout, uint32_t w, uint32_t h, bool clear);
    VkRenderPass Pass(VkFormat color, bool depth, bool clear, VkImageLayout layout);
    VkFramebuffer Framebuffer(VkRenderPass rp, VkImageView color, VkImageView depth, uint32_t w, uint32_t h);
    void ForgetView(VkImageView v);
    void BeginPass(VkRenderPass rp, VkFramebuffer fb, uint32_t w, uint32_t h, uint64_t timing_key);
    enum class PassEnd { Targets, Auxiliary, Feedback, Copy, Readback, Resize, Present, Submit, Count };
    void EndRendering(PassEnd reason);
    void GlobalBarrier();
    void SetViewport(float x, float y, float w, float h);
    // targets
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    void UploadTarget(Target& t, const Surface& s);
    bool CreatePixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    VkImageView EnsureFeedbackCopy(Target& t);
    // textures
    void BindTexture(const State& s, int stage, DrawTextures& stages);
    void BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, DrawTextures& stages);
    void BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, bool same);
    void StageLevel(VkCommandBuffer cb, const Image& im, uint32_t face, uint32_t level, uint32_t lw, uint32_t lh,
                    uint32_t ld, const std::vector<uint8_t>& px, uint32_t pitch, bool blocks);
    VkSampler EnsureSampler(const VkSamplerCreateInfo& ci, uint64_t border = 0);
    VkSampler StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp);
    void ReleaseOldTextures();
    // shaders and pipelines
    VkShaderModule Module(const std::string& hlsl, EShLanguage stage, const char* entry);
    VkShaderModule VertexModule(const State& s, const Assembled& a, const DrawShaders& sh);
    VkShaderModule PixelModule(const PixelKey& pk, uint64_t pkey);
    void BuildPipelineKey(const State& s, const Assembled& a, const DrawShaders& sh, Target* rt, Target* ds, PipeKey& k);
    VkPipeline EnsurePipeline(const PipeKey& k, VkShaderModule vs, VkShaderModule ps);
    VkPipeline EnsureBlitPipeline(VkFormat fmt, bool over);
    VkDescriptorSet AllocSet();
    void BindSet(const Alloc& vc, VkDeviceSize vc_size, const Alloc& dc, VkDeviceSize dc_size, const StageView views[4],
                 const VkSampler samplers[4]);
    void Blit(const BlitArgs& b);
    // present
    void UploadGammaRamp(uint32_t brightness, uint32_t contrast);
    void PresentBlit(const Target& t, bool pillarbox);
    bool AcquireImage(uint32_t& index);
    void CountFps();
    void LogStats();
    void ApplySettings();

    SDL_Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    uint32_t instance_version_ = VK_API_VERSION_1_1;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties mem_props_{};
    uint32_t family_ = 0;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    struct Fb {
        VkFramebuffer fb;
        VkImageView color, depth;
    };
    std::unordered_map<uint64_t, VkRenderPass> passes_;
    std::unordered_map<uint64_t, Fb> fbs_;
    bool bc_ = false;
    bool aniso_ = false;
    bool wireframe_ = false;
    bool large_points_ = false;
    bool custom_border_ = false;  // VK_EXT_custom_border_color, without a format
    bool depth_linear_ = true;    // depth textures filter linearly
    VkFormat depth_fmt_ = VK_FORMAT_D24_UNORM_S8_UINT;
    VkDeviceSize ubo_align_ = 256;
    // swapchain, in the panel's own orientation
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swap_fmt_ = kBgra;
    uint32_t swap_w_ = 0, swap_h_ = 0;
    VkSurfaceTransformFlagBitsKHR transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::vector<VkImage> swap_images_;
    std::vector<VkImageView> swap_views_;
    std::vector<VkSemaphore> rendered_;
    bool recreate_ = false;
    // layouts, caches
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::mutex cache_lock_;
    bool cache_dirty_ = false;
    std::unordered_map<uint64_t, VkShaderModule> modules_;  // by source hash
    std::unordered_map<uint64_t, VkShaderModule> vmods_;    // by vertex key
    std::unordered_map<uint64_t, VkShaderModule> pmods_;    // by pixel key
    std::unordered_map<uint64_t, VkPipeline> pipelines_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    VkSampler linear_ = VK_NULL_HANDLE, point_ = VK_NULL_HANDLE;
    Image dummy_[4];  // 2D, 3D, cube, depth: what a stage without a texture reads
    Image lut_;
    uint32_t ramp_set_ = ~0u;
    bool gamma_ = false;
    // frames
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    uint64_t presents_ = 0;
    // submissions: counted, and the newest one the GPU is known to have finished (any thread reads it)
    FrameFences<kFrames> fences_;
    ImageMemory image_memory_;
    std::atomic<uint64_t> presents_seen_{0};
    // surfaces locked without a wait (NOOVERWRITE): copies in flight, DeliverLate hands the newest finished one over
    static constexpr int kLateSlots = 3;
    struct Late {
        Buffer rb[kLateSlots];
        Image down;  // the target at the console's size, when it is upscaled
        uint64_t serial[kLateSlots] = {};
        uint64_t frame[kLateSlots] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0, gh = 0, next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;
    VkImage GuestSizedSource(Target& t, Image& down);
    void HostBarrier();
    // the pass being recorded: a target pair, or another view (other_)
    bool active_ = false;
    bool other_ = false;
    Target* active_rt_ = nullptr;
    Target* active_ds_ = nullptr;
    Target* draw_rt_ = nullptr;
    Target* draw_ds_ = nullptr;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, Tex> textures_;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    float scale_ = 1;
    uint32_t shadow_res_ = 0;
    Assembled asm_;
    bool profile_ = false;
    bool profile_passes_ = true;
    bool narrow_barriers_ = false;
    // swapchain at the render scale; the display hardware stretches it
    bool present_scaled_ = false;
    // OPTIMISATION menu rows (scaled_presentation, native_effect_size, fast_glow in scda.toml). E1: targets of at most
    // kEffectSide console pixels (glow/brightness chains) keep the console's size. E2 (fast glow): no box-averaged taps
    // when an upscaled target is shrunk into a smaller one; the sun's reflection can flicker, so it defaults off.
    static constexpr uint32_t kEffectSide = 256;
    bool effect_native_ = false;
    bool effect_single_tap_ = false;
    bool rebuild_targets_ = false;
    bool EffectTarget(uint32_t gw, uint32_t gh) const {
        return effect_native_ && std::max(gw, gh) <= kEffectSide && gw < guest_w_ && gh < guest_h_;
    }
    bool descriptor_cache_ = true;
    bool skip_unused_stages_ = true;
    bool screen_linear_workaround_ = false;
    bool timing_pass_ = false;
    bool trace_passes_ = false;
    uint32_t trace_draws_ = 0;
    struct PassStats {
        double ms = 0;
        uint64_t count = 0, draws = 0;
    };
    std::unordered_map<uint64_t, PassStats> pass_stats_;
    uint32_t timestamp_bits_ = 0;
    struct Stats {
        uint64_t frames = 0, draws = 0, sets = 0, hits = 0, upload_bytes = 0, passes = 0, flushes = 0, syncs = 0;
        uint64_t shaders = 0, pipelines = 0, gpu_samples = 0;
        std::array<uint64_t, size_t(PassEnd::Count)> pass_ends{};
        uint64_t clears = 0, full_clear_starts = 0, unused_stages = 0;
        // texture binds by kind; nomip_big = no mip chain and at least 256x256 (minified it skips texels)
        uint64_t tex_bc = 0, tex_raw = 0, tex_mip = 0, tex_nomip = 0, tex_nomip_big = 0, tex_rt = 0, tex_aniso = 0;
        uint64_t tex_bc_padded = 0, tex_texels = 0;
        uint64_t clear_color_pixels = 0, clear_depth_pixels = 0, clear_stencil_pixels = 0;
        double draw_ms = 0, descriptor_ms = 0, wait_ms = 0, acquire_ms = 0, present_ms = 0;
        double shader_ms = 0, pipeline_ms = 0, gpu_ms = 0;
    } stats_;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
    // Per-present hitch attribution while profiling; cold mode ignores and never writes disk caches.
    bool cold_cache_ = false;
    std::chrono::steady_clock::time_point frame_t_{};
    uint64_t frame_shaders_ = 0, frame_pipelines_ = 0;
    double frame_shader_ms_ = 0, frame_pipeline_ms_ = 0;
    void LogFrame();
};

VulkanBackend* g_vk = nullptr;

#define VK_TRY(x)                                       \
    do {                                                \
        VkResult r_ = (x);                              \
        if (r_ != VK_SUCCESS) {                         \
            LOGE("vulkan: %s failed: %d", #x, int(r_)); \
            return false;                               \
        }                                               \
    } while (0)

VulkanBackend::~VulkanBackend() {
    g_vk = nullptr;
    if (dev_) {
        vkDeviceWaitIdle(dev_);
        for (Frame& f : frames_)
            if (f.timestamps) vkDestroyQueryPool(dev_, f.timestamps, nullptr);
    }
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    window_ = static_cast<SDL_Window*>(app::Window());
    profile_ = DebugFlag("debug.scda.vk_profile");
    profile_passes_ = DebugFlag("debug.scda.vk_profile_passes", true);
    narrow_barriers_ = DebugFlag("debug.scda.vk_narrow_barriers");
    {
        // OPTIMISATION menu rows; ApplySettings follows later changes
        std::lock_guard lock(app::UiLock());
        present_scaled_ = app::Get().scaled_presentation;
        effect_native_ = app::Get().native_effect_size;
        effect_single_tap_ = app::Get().fast_glow;
    }
    descriptor_cache_ = DebugFlag("debug.scda.vk_descriptor_cache", true);
    skip_unused_stages_ = DebugFlag("debug.scda.vk_skip_unused_stages", true);
    cold_cache_ = profile_ && DebugFlag("debug.scda.vk_cold_cache");
    LOGI("vulkan: profiling %s, descriptor cache %s%s", profile_ ? "on" : "off", descriptor_cache_ ? "on" : "off",
         cold_cache_ ? ", cold shader/pipeline cache (read/write disabled)" : "");
    glslang::InitializeProcess();
    if (!CreateInstance() || !CreateSurface() || !CreateDevice()) return false;
    if (!CreateSwapchain()) LOGW("vulkan: no swapchain yet, presents wait for the window");
    if (!CreateLayouts() || !CreateFrames()) return false;
    LoadPipelineCache();
    scale_ = app::RenderScale(guest_h_);
    shadow_res_ = app::Get().shadow_resolution;
    BeginFrame();
    CreateDummies();
    g_vk = this;
    LOGI("vulkan: %ux%u guest, render scale %.3f", width, height, scale_);
    return true;
}

bool VulkanBackend::CreateInstance() {
    Uint32 n = 0;
    const char* const* ext = SDL_Vulkan_GetInstanceExtensions(&n);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SCDA";
    // 1.3 where the loader has it: a device's 1.3 calls only come with an instance that asks for them
    uint32_t loader = VK_API_VERSION_1_1;
    vkEnumerateInstanceVersion(&loader);
    instance_version_ = std::min<uint32_t>(loader, VK_API_VERSION_1_3);
    app.apiVersion = instance_version_;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = ext;
    VK_TRY(vkCreateInstance(&ci, nullptr, &instance_));
    return true;
}

bool VulkanBackend::CreateSurface() {
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        LOGW("vulkan: no surface: %s", SDL_GetError());
        surface_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool VulkanBackend::CreateDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
    for (VkPhysicalDevice gpu : gpus) {
        uint32_t m = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &m, nullptr);
        std::vector<VkQueueFamilyProperties> families(m);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &m, families.data());
        for (uint32_t i = 0; i < m && !gpu_; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface_, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                gpu_ = gpu;
                family_ = i;
                timestamp_bits_ = families[i].timestampValidBits;
            }
        }
        if (gpu_) break;
    }
    if (!gpu_) {
        LOGE("vulkan: no GPU presents to the window");
        return false;
    }
    vkGetPhysicalDeviceProperties(gpu_, &props_);
    screen_linear_workaround_ = NeedsScreenLinearWorkaround(props_.vendorID, props_.deviceName, props_.driverVersion);
    if (screen_linear_workaround_) LOGI("vulkan: experimental Mali-G51 r18p0 screen-linear interpolation workaround");
    vkGetPhysicalDeviceMemoryProperties(gpu_, &mem_props_);
    ubo_align_ = std::max<VkDeviceSize>(props_.limits.minUniformBufferOffsetAlignment, 16);

    std::vector<const char*> want = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &en, exts.data());
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME)) continue;
        VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        q.pNext = &cbf;
        vkGetPhysicalDeviceFeatures2(gpu_, &q);
        // without a format: one sampler fits every texture format the stage may hold
        custom_border_ = cbf.customBorderColors && cbf.customBorderColorWithoutFormat;
    }
    if (custom_border_) want.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(gpu_, &have);
    bc_ = have.textureCompressionBC;
    aniso_ = have.samplerAnisotropy;
    wireframe_ = have.fillModeNonSolid;
    large_points_ = have.largePoints;
    VkPhysicalDeviceFeatures on{};
    on.textureCompressionBC = have.textureCompressionBC;
    on.samplerAnisotropy = have.samplerAnisotropy;
    on.fillModeNonSolid = have.fillModeNonSolid;
    on.largePoints = have.largePoints;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features = on;
    cbf.customBorderColors = cbf.customBorderColorWithoutFormat = VK_TRUE;
    if (custom_border_) f2.pNext = &cbf;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = uint32_t(want.size());
    ci.ppEnabledExtensionNames = want.data();
    VK_TRY(vkCreateDevice(gpu_, &ci, nullptr, &dev_));
    vkGetDeviceQueue(dev_, family_, 0, &queue_);
    // D24S8 where it can be drawn to and sampled, else D32S8
    auto usable = [&](VkFormat f) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu_, f, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        return (fp.optimalTilingFeatures & need) == need;
    };
    depth_fmt_ = usable(VK_FORMAT_D24_UNORM_S8_UINT) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    auto filters = [&](VkFormat f) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu_, f, &fp);
        return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    };
    depth_linear_ = filters(depth_fmt_) && filters(VK_FORMAT_D32_SFLOAT);
    LOGR("Vulkan: %s, API %u.%u, driver %08X, BC textures %s, depth %s%s, border colours %s", props_.deviceName,
         VK_API_VERSION_MAJOR(props_.apiVersion), VK_API_VERSION_MINOR(props_.apiVersion), props_.driverVersion,
         bc_ ? "yes" : "no (decoded)", depth_fmt_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
         depth_linear_ ? " filtered" : " unfiltered", custom_border_ ? "exact" : "nearest");
    return true;
}

bool VulkanBackend::CreateSwapchain() {
    if (!surface_) return false;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps) != VK_SUCCESS) return false;
    if (!caps.currentExtent.width || !caps.currentExtent.height) return false;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
    if (formats.empty()) return false;
    VkSurfaceFormatKHR fmt = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            fmt = f;
            break;
        }
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        extent = {uint32_t(w), uint32_t(h)};
    }
    // Android tells the size as the user sees it; the images are in the panel's own orientation
    if (caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR))
        std::swap(extent.width, extent.height);
    if (present_scaled_) {
        // same aspect as the window so the stretch is uniform; never larger than the window
        float f;
        {
            std::lock_guard lock(app::UiLock());
            f = std::clamp(app::Get().render_percent / 100.0f, 0.1f, 1.0f);
        }
        extent.width = std::clamp(uint32_t(extent.width * f + 0.5f), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(uint32_t(extent.height * f + 0.5f), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    uint32_t count = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = count;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // the panel's own orientation spares the compositor a rotation pass: PresentBlit turns the frame
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;
    VkSwapchainKHR sc;
    if (vkCreateSwapchainKHR(dev_, &ci, nullptr, &sc) != VK_SUCCESS) return false;
    DestroySwapchain();
    swapchain_ = sc;
    swap_fmt_ = fmt.format;
    swap_w_ = extent.width;
    swap_h_ = extent.height;
    transform_ = caps.currentTransform;
    app::g_display_applied++;  // refresh the cached output/aspect after a surface size change
    vkGetSwapchainImagesKHR(dev_, swapchain_, &n, nullptr);
    swap_images_.resize(n);
    vkGetSwapchainImagesKHR(dev_, swapchain_, &n, swap_images_.data());
    for (VkImage im : swap_images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_fmt_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(dev_, &vi, nullptr, &v);
        swap_views_.push_back(v);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(dev_, &si, nullptr, &s);
        rendered_.push_back(s);
    }
    LOGI("vulkan: swapchain %ux%u, %u images, format %d, transform %d", swap_w_, swap_h_, n, swap_fmt_, transform_);
    return true;
}

// the old swapchain's views and semaphores; the swapchain itself unless a new one replaced it
void VulkanBackend::DestroySwapchain() {
    vkDeviceWaitIdle(dev_);
    for (VkImageView v : swap_views_) {
        ForgetView(v);
        vkDestroyImageView(dev_, v, nullptr);
    }
    for (VkSemaphore s : rendered_) vkDestroySemaphore(dev_, s, nullptr);
    swap_views_.clear();
    rendered_.clear();
    swap_images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

// set 0: b0 b1 the constants (dynamic offsets), t0 to t3 at 2 to 5, s0 to s3 at 6 to 9
bool VulkanBackend::CreateLayouts() {
    VkDescriptorSetLayoutBinding b[10] = {};
    for (uint32_t i = 0; i < 10; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                              : i < 6 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                      : VK_DESCRIPTOR_TYPE_SAMPLER;
        b[i].stageFlags = i < 2 ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 10;
    li.pBindings = b;
    VK_TRY(vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_));
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &set_layout_;
    VK_TRY(vkCreatePipelineLayout(dev_, &pi, nullptr, &layout_));
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    linear_ = EnsureSampler(si);
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    point_ = EnsureSampler(si);
    if (!linear_ || !point_) LOGE("vulkan: no samplers");
    return linear_ && point_;
}

bool VulkanBackend::CreateFrames() {
    if (!fences_.Init(dev_)) return false;
    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = family_;
        VK_TRY(vkCreateCommandPool(dev_, &pci, nullptr, &f.pool));
        VkCommandBuffer cbs[2];
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 2;
        VK_TRY(vkAllocateCommandBuffers(dev_, &ai, cbs));
        f.up = cbs[0];
        f.cmd = cbs[1];
        if (profile_ && timestamp_bits_) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = kTimestampQueries;
            VK_TRY(vkCreateQueryPool(dev_, &qi, nullptr, &f.timestamps));
        }
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_TRY(vkCreateSemaphore(dev_, &si, nullptr, &f.acquired));
        VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!CreateBuffer(f.ring, kRingSize, usage,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            LOGE("vulkan: no upload ring");
            return false;
        }
    }
    return true;
}

std::filesystem::path PipelineCacheFile() { return app::ExeDir() / "shadercache" / "vulkan_pipelines.bin"; }
std::filesystem::path SpirvDir() { return app::ExeDir() / "shadercache" / "vulkan"; }

void VulkanBackend::LoadPipelineCache() {
    std::vector<char> data;
    if (std::ifstream f(PipelineCacheFile(), std::ios::binary); f && !cold_cache_) {
        data.assign(std::istreambuf_iterator<char>(f), {});
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.data();
    // a cache from another driver is refused: start empty
    if (vkCreatePipelineCache(dev_, &ci, nullptr, &cache_) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        vkCreatePipelineCache(dev_, &ci, nullptr, &cache_);
    }
    LOGI("vulkan: pipeline cache %zu bytes", data.size());
}

void VulkanBackend::SavePipelines() {
    std::lock_guard lock(cache_lock_);
    if (!cache_ || !cache_dirty_ || cold_cache_) return;
    size_t n = 0;
    vkGetPipelineCacheData(dev_, cache_, &n, nullptr);
    std::vector<char> data(n);
    if (vkGetPipelineCacheData(dev_, cache_, &n, data.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories(PipelineCacheFile().parent_path(), ec);
    std::ofstream(PipelineCacheFile(), std::ios::binary).write(data.data(), std::streamsize(n));
    cache_dirty_ = false;
}

uint32_t VulkanBackend::MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++) {
        if ((bits >> i & 1) && (mem_props_.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return kNone;
}

bool VulkanBackend::CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev_, &ci, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == kNone || vkAllocateMemory(dev_, &ai, nullptr, &b.mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev_, b.buf, nullptr);
        b = {};
        return false;
    }
    vkBindBufferMemory(dev_, b.buf, b.mem, 0);
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.map));
    b.size = size;
    return true;
}

void VulkanBackend::DestroyBuffer(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);
    b = {};
}

bool VulkanBackend::CreateImage(Image& im, const ImageDesc& d) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = d.view == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = d.view == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = d.fmt;
    ci.extent = {d.w, d.h, d.depth};
    ci.mipLevels = d.levels;
    ci.arrayLayers = d.layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = d.usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ci, nullptr, &im.image) != VK_SUCCESS) {
        im = {};
        return false;
    }
    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    requirements.pNext = &dedicated;
    VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    info.image = im.image;
    vkGetImageMemoryRequirements2(dev_, &info, &requirements);
    const auto& req = requirements.memoryRequirements;
    uint32_t type = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == kNone || !image_memory_.Allocate(dev_, req, type, dedicated.requiresDedicatedAllocation,
                                                im.image, im.alloc)) {
        LOGE("vulkan: no memory for a %ux%u image", d.w, d.h);
        DestroyImage(im);
        return false;
    }
    if (vkBindImageMemory(dev_, im.image, im.alloc.memory, im.alloc.offset) != VK_SUCCESS) {
        DestroyImage(im);
        return false;
    }
    im.fmt = d.fmt;
    im.w = d.w;
    im.h = d.h;
    bool depth = IsDepth(d.fmt);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = d.view;
    vi.format = d.fmt;
    vi.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0,
                           d.levels, 0, d.layers};
    if (vkCreateImageView(dev_, &vi, nullptr, &im.view) != VK_SUCCESS) {
        DestroyImage(im);
        return false;
    }
    if (depth && (d.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        if (HasStencil(d.fmt)) vi.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (vkCreateImageView(dev_, &vi, nullptr, &im.attach) != VK_SUCCESS) {
            DestroyImage(im);
            return false;
        }
    }
    return true;
}

void VulkanBackend::DestroyImage(Image& im) {
    ForgetView(im.view);
    ForgetView(im.attach);
    if (im.view) vkDestroyImageView(dev_, im.view, nullptr);
    if (im.attach) vkDestroyImageView(dev_, im.attach, nullptr);
    if (im.image) vkDestroyImage(dev_, im.image, nullptr);
    image_memory_.Free(dev_, im.alloc);
    im = {};
}

VkImageAspectFlags Aspects(VkFormat f) {
    if (!IsDepth(f)) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

void ImageBarrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags src_stage, VkAccessFlags src, VkPipelineStageFlags dst_stage, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// a new image into GENERAL, cleared (colour 0, depth 1), ahead of the frame's draws
void VulkanBackend::InitTargetImage(Image& im, bool clear) {
    VkCommandBuffer cb = Cur().up;
    VkImageAspectFlags aspect = Aspects(im.fmt);
    ImageBarrier(cb, im.image, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
    if (clear && IsDepth(im.fmt)) {
        VkClearDepthStencilValue v{1.0f, 0};
        vkCmdClearDepthStencilImage(cb, im.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    } else if (clear) {
        VkClearColorValue v{};
        vkCmdClearColorImage(cb, im.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
}

void VulkanBackend::CreateDummies() {
    struct {
        VkImageViewType view;
        uint32_t layers;
        VkFormat fmt;
    } kinds[4] = {{VK_IMAGE_VIEW_TYPE_2D, 1, kBgra},
                  {VK_IMAGE_VIEW_TYPE_3D, 1, kBgra},
                  {VK_IMAGE_VIEW_TYPE_CUBE, 6, kBgra},
                  {VK_IMAGE_VIEW_TYPE_2D, 1, VK_FORMAT_D32_SFLOAT}};
    VkCommandBuffer cb = Cur().up;
    for (int i = 0; i < 4; i++) {
        ImageDesc d{kinds[i].fmt, 1, 1};
        d.layers = kinds[i].layers;
        d.view = kinds[i].view;
        d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!CreateImage(dummy_[i], d)) continue;
        VkImageAspectFlags aspect = Aspects(d.fmt);
        ImageBarrier(cb, dummy_[i].image, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange range{aspect, 0, 1, 0, d.layers};
        if (IsDepth(d.fmt)) {
            VkClearDepthStencilValue v{1.0f, 0};
            vkCmdClearDepthStencilImage(cb, dummy_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        } else {
            VkClearColorValue v{};
            vkCmdClearColorImage(cb, dummy_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        }
        ImageBarrier(cb, dummy_[i].image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    ImageDesc ld{kBgra, 256, 1};
    ld.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CreateImage(lut_, ld);
    ImageBarrier(cb, lut_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0);
}

VulkanBackend::Alloc VulkanBackend::Upload(VkDeviceSize size, VkDeviceSize align) {
    if (profile_) stats_.upload_bytes += size;
    Frame& f = Cur();
    VkDeviceSize o = (f.used + align - 1) / align * align;
    if (o + size <= kRingSize) {
        f.used = o + size;
        return {f.ring.buf, o, f.ring.map + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    Buffer b;
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!CreateBuffer(b, size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        LOGE("vulkan: no %llu byte upload buffer", (unsigned long long)size);
        static std::vector<uint8_t> sink;
        sink.resize(size);
        return {f.ring.buf, 0, sink.data()};
    }
    f.dead_buffers.push_back(b);
    return {b.buf, 0, b.map};
}

void VulkanBackend::BeginFrame() {
    Frame& f = Cur();
    {
        CpuTimer timer(profile_ ? &stats_.wait_ms : nullptr);
        CheckGpu(fences_.Begin(frame_ % kFrames));
    }
    if (f.timestamps_pending) {
        std::array<uint64_t, kTimestampQueries> ticks{};
        uint32_t count = 2 + f.timed_count * 2;
        if (vkGetQueryPoolResults(dev_, f.timestamps, 0, count, count * sizeof(uint64_t), ticks.data(), sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            uint64_t mask = UINT64_MAX >> (64 - timestamp_bits_);
            auto elapsed = [&](uint32_t begin, uint32_t end) {
                return double((ticks[end] - ticks[begin]) & mask) * props_.limits.timestampPeriod / 1e6;
            };
            stats_.gpu_ms += elapsed(0, 1);
            stats_.gpu_samples++;
            for (uint32_t i = 0; i < f.timed_count; i++) {
                const auto& pass = f.timed_passes[i];
                auto& total = pass_stats_[pass.key];
                total.ms += elapsed(2 + i * 2, 3 + i * 2);
                total.count++;
                total.draws += pass.draws;
            }
        }
        f.timestamps_pending = false;
    }
    f.sets.clear();
    for (Image& im : f.dead_images) DestroyImage(im);
    for (Buffer& b : f.dead_buffers) DestroyBuffer(b);
    f.dead_images.clear();
    f.dead_buffers.clear();
    f.used = 0;
    for (VkDescriptorPool p : f.pools) vkResetDescriptorPool(dev_, p, 0);
    f.set_pool = 0;
    vkResetCommandPool(dev_, f.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.up, &bi);
    vkBeginCommandBuffer(f.cmd, &bi);
    if (f.timestamps) {
        vkCmdResetQueryPool(f.up, f.timestamps, 0, kTimestampQueries);
        vkCmdWriteTimestamp(f.up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.timestamps, 0);
    }
    f.timed_count = 0;
    timing_pass_ = false;
    active_ = other_ = false;
    active_rt_ = active_ds_ = nullptr;
}

void VulkanBackend::Submit(VkSemaphore wait, VkSemaphore signal) {
    Frame& f = Cur();
    EndRendering(PassEnd::Submit);
    if (f.timestamps) {
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.timestamps, 1);
        f.timestamps_pending = true;
    }
    vkEndCommandBuffer(f.up);
    vkEndCommandBuffer(f.cmd);
    VkCommandBuffer cbs[2] = {f.up, f.cmd};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = wait ? 1 : 0;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 2;
    si.pCommandBuffers = cbs;
    si.signalSemaphoreCount = signal ? 1 : 0;
    si.pSignalSemaphores = &signal;
    CheckGpu(fences_.Submit(queue_, frame_ % kFrames, si));
}

void VulkanBackend::Flush() {
    if (profile_) stats_.flushes++;
    Submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    BeginFrame();
}

void VulkanBackend::GlobalBarrier() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(Cur().cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb,
                         0, nullptr, 0, nullptr);
}

// render passes by attachment formats; the load op and layout make no difference to which pipelines fit them
VkRenderPass VulkanBackend::Pass(VkFormat color, bool depth, bool clear, VkImageLayout layout) {
    uint64_t key = uint64_t(color) | uint64_t(depth) << 32 | uint64_t(clear) << 33 | uint64_t(layout) << 34;
    auto it = passes_.find(key);
    if (it != passes_.end()) return it->second;
    VkAttachmentDescription at[2] = {};
    VkAttachmentReference cref{0, layout}, dref{0, VK_IMAGE_LAYOUT_GENERAL};
    uint32_t n = 0;
    if (color) {
        at[n] = {0, color, VK_SAMPLE_COUNT_1_BIT, clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                 VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 layout, layout};
        cref.attachment = n++;
    }
    if (depth) {
        at[n] = {0, depth_fmt_, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                 VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_GENERAL};
        dref.attachment = n++;
    }
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = color ? 1 : 0;
    sp.pColorAttachments = color ? &cref : nullptr;
    sp.pDepthStencilAttachment = depth ? &dref : nullptr;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = n;
    ci.pAttachments = at;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev_, &ci, nullptr, &rp) != VK_SUCCESS) LOGE("vulkan: render pass creation failed");
    return passes_[key] = rp;
}

VkFramebuffer VulkanBackend::Framebuffer(VkRenderPass rp, VkImageView color, VkImageView depth, uint32_t w,
                                         uint32_t h) {
    uint64_t key = Hash(&color, sizeof(color), Hash(&depth, sizeof(depth), Hash(&rp, sizeof(rp), uint64_t(w) << 32 | h)));
    auto it = fbs_.find(key);
    if (it != fbs_.end()) return it->second.fb;
    VkImageView views[2];
    uint32_t n = 0;
    if (color) views[n++] = color;
    if (depth) views[n++] = depth;
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = rp;
    ci.attachmentCount = n;
    ci.pAttachments = views;
    ci.width = w;
    ci.height = h;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev_, &ci, nullptr, &fb) != VK_SUCCESS) LOGE("vulkan: framebuffer creation failed");
    fbs_[key] = {fb, color, depth};
    return fb;
}

// framebuffers go with the views they hold
void VulkanBackend::ForgetView(VkImageView v) {
    if (!v) return;
    for (auto it = fbs_.begin(); it != fbs_.end();) {
        if (it->second.color == v || it->second.depth == v) {
            vkDestroyFramebuffer(dev_, it->second.fb, nullptr);
            it = fbs_.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanBackend::BeginPass(VkRenderPass rp, VkFramebuffer fb, uint32_t w, uint32_t h, uint64_t timing_key) {
    if (profile_) stats_.passes++;
    trace_draws_ = 0;
    if (trace_passes_) LOGI("vk-trace: begin target=%08X size=%ux%u", uint32_t(timing_key), w, h);
    Frame& f = Cur();
    assert(!active_ && !timing_pass_ && f.timed_count <= kTimedPasses);
    timing_pass_ = profile_passes_ && f.timestamps && f.timed_count < kTimedPasses;
    if (timing_pass_) {
        f.timed_passes[f.timed_count] = {timing_key, 0};
        // Bottom-of-pipe boundaries give serialized intervals, including attachment load/store and pass barriers.
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.timestamps, 2 + f.timed_count * 2);
    }
    VkClearValue clear{};
    clear.color = {{0, 0, 0, 1}};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea.extent = {w, h};
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass(Cur().cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanBackend::BeginRendering(Target* rt, Target* ds) {
    if (active_ && !other_ && active_rt_ == rt && active_ds_ == ds) return;
    EndRendering(PassEnd::Targets);
    uint32_t w = rt ? rt->w : ds->w, h = rt ? rt->h : ds->h;
    VkRenderPass rp = Pass(rt ? kBgra : VK_FORMAT_UNDEFINED, ds != nullptr, false, VK_IMAGE_LAYOUT_GENERAL);
    uint64_t key = uint64_t(rt ? rt->data : ds->data) | uint64_t(w) << 32 | uint64_t(h) << 48;
    BeginPass(rp, Framebuffer(rp, rt ? rt->im.view : VK_NULL_HANDLE, ds ? ds->im.attach : VK_NULL_HANDLE, w, h), w, h, key);
    active_ = true;
    other_ = false;
    active_rt_ = rt;
    active_ds_ = ds;
    if (rt) rt->drawn = true;
    if (ds) ds->drawn = true;
}

void VulkanBackend::BeginRenderingView(VkImageView view, VkFormat fmt, VkImageLayout layout, uint32_t w, uint32_t h,
                                       bool clear) {
    EndRendering(PassEnd::Auxiliary);
    VkRenderPass rp = Pass(fmt, false, clear, layout);
    BeginPass(rp, Framebuffer(rp, view, VK_NULL_HANDLE, w, h), w, h, uint64_t(w) << 32 | uint64_t(h) << 48);
    active_ = true;
    other_ = true;
    active_rt_ = active_ds_ = nullptr;
}

void VulkanBackend::EndRendering(PassEnd reason) {
    if (!active_) return;
    if (profile_) stats_.pass_ends[size_t(reason)]++;
    if (trace_passes_) {
        const char* names[] = {"targets", "auxiliary", "feedback", "copy", "readback", "resize", "present", "submit"};
        static_assert(std::size(names) == size_t(PassEnd::Count));
        LOGI("vk-trace: end reason=%s color=%08X depth=%08X draws=%u", names[size_t(reason)],
             active_rt_ ? active_rt_->data : 0, active_ds_ ? active_ds_->data : 0, trace_draws_);
    }
    vkCmdEndRenderPass(Cur().cmd);
    if (narrow_barriers_) RenderPassBarrier(Cur().cmd);
    else GlobalBarrier();
    if (timing_pass_) {
        Frame& f = Cur();
        assert(f.timed_count < kTimedPasses);
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.timestamps, 3 + f.timed_count * 2);
        f.timed_count++;
        timing_pass_ = false;
    }
    active_ = other_ = false;
    active_rt_ = active_ds_ = nullptr;
}

// D3D's y up: a flipped viewport keeps the generated shaders' clip space and their winding
void VulkanBackend::SetViewport(float x, float y, float w, float h) {
    VkViewport vp{x, y + h, w, -h, 0, 1};
    vkCmdSetViewport(Cur().cmd, 0, 1, &vp);
}

// ---- targets

Target* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    if (IsShadowSurface(s, depth) && !depth) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(TargetKey(data, depth));
    return it != targets_.end() && it->second.gw == gw && it->second.gh == gh ? &it->second : nullptr;
}

Target* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    // the shadow map's colour surface has no memory and colour writes off: depth alone is bound
    bool shadow = IsShadowSurface(s, depth);
    if (shadow && !depth) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    uint64_t key = TargetKey(data, depth);
    auto it = targets_.find(key);
    if (it != targets_.end() && it->second.gw == gw && it->second.gh == gh) return &it->second;
    if (it != targets_.end()) {
        Target& old = it->second;
        if (active_rt_ == &old || active_ds_ == &old) EndRendering(PassEnd::Resize);
        if (draw_rt_ == &old) draw_rt_ = nullptr;
        if (draw_ds_ == &old) draw_ds_ = nullptr;
        Release(old.im);
        Release(old.fb);
        targets_.erase(it);
    }
    Target t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    float sc = shadow && shadow_res_ ? float(shadow_res_) / gw : EffectTarget(gw, gh) ? std::min(scale_, 1.0f) : scale_;
    t.w = std::max(1u, uint32_t(std::lround(gw * sc)));
    t.h = std::max(1u, uint32_t(std::lround(gh * sc)));
    ImageDesc d{depth ? depth_fmt_ : kBgra, t.w, t.h};
    d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!CreateImage(t.im, d)) return nullptr;
    InitTargetImage(t.im, true);
    if (shadow) LOGI("vulkan: shadow map %ux%u", t.w, t.h);
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    return &(targets_[key] = std::move(t));
}

// BGRA pixels into a new w x h image, left for sampling
bool VulkanBackend::CreatePixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                     uint32_t h) {
    ImageDesc d{kBgra, w, h};
    d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateImage(im, d)) return false;
    VkCommandBuffer cb = Cur().up;
    ImageBarrier(cb, im.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    StageLevel(cb, im, 0, 0, w, h, 1, px, pitch, false);
    ImageBarrier(cb, im.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    return true;
}

// CPU pixels of a colour surface into its target, scaled
void VulkanBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    Image tmp;
    if (!CreatePixelImage(tmp, px, pitch, d.width, d.height)) return;
    BeginRenderingView(t.im.view, kBgra, VK_IMAGE_LAYOUT_GENERAL, t.w, t.h, false);
    SetViewport(0, 0, float(t.w), float(t.h));
    VkRect2D all{{0, 0}, {t.w, t.h}};
    vkCmdSetScissor(Cur().cmd, 0, 1, &all);
    BlitArgs b;
    b.src = tmp.view;
    b.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.alpha = true;
    b.over = over;
    Blit(b);
    EndRendering(PassEnd::Auxiliary);
    Release(tmp);
    t.drawn = true;
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// the console reads the pixels it is about to overwrite: the draw gets a copy
VkImageView VulkanBackend::EnsureFeedbackCopy(Target& t) {
    EndRendering(PassEnd::Feedback);
    if (!t.fb.image) {
        ImageDesc d{t.im.fmt, t.w, t.h};
        d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!CreateImage(t.fb, d)) return VK_NULL_HANDLE;
        InitTargetImage(t.fb, false);
    }
    VkImageCopy c{};
    c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.dstSubresource = c.srcSubresource;
    c.extent = {t.w, t.h, 1};
    vkCmdCopyImage(Cur().cmd, t.im.image, VK_IMAGE_LAYOUT_GENERAL, t.fb.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    GlobalBarrier();
    return t.fb.view;
}

// ---- textures

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

void VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                      DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return;
    stages.rt[stage] = true;
    stages.levels[stage] = 1;
    if (profile_) stats_.tex_rt++, stats_.tex_texels += uint64_t(t.w) * t.h;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / t.gw;
    if (!effect_single_tap_ && !t.depth && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    if (trace_passes_) {
        LOGI("vk-trace: sample stage=%d target=%08X depth=%u dst=%08X self=%u ps=%08X modes=%05X ffop=%u wmask=%08X",
             stage, t.data, uint32_t(t.depth), draw_rt_ ? draw_rt_->data : 0, uint32_t(&t == draw_rt_),
             s.ps ? s.ps->handle : 0, s.rs[kRsPsTextureModes], s.tss[size_t(stage)][kTssColorOp],
             SimpleState(s, kMColorWriteEnable));
    }
    stages.bind[stage] = {&t == draw_rt_ ? EnsureFeedbackCopy(t) : t.im.view, VK_IMAGE_LAYOUT_GENERAL};
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.im.image && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    Release(tx.im);
    tx = Tex{};
    tx.d = d;
    uint32_t w = d.width, h = d.height;
    tx.levels = std::max(d.levels, 1u);
    bool bc = fi.compressed;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    VkFormat fmt = kBgra;
    if (fi.host == Host::R32F) fmt = VK_FORMAT_D32_SFLOAT;
    if (bc && bc_) {
        fmt = fi.host == Host::BC1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK
              : fi.host == Host::BC2 ? VK_FORMAT_BC2_UNORM_BLOCK
                                     : VK_FORMAT_BC3_UNORM_BLOCK;
    }
    ImageDesc id{fmt, w, h};
    id.depth = d.cube ? 1 : d.depth;
    id.layers = d.cube ? 6 : 1;
    id.levels = tx.levels;
    id.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    id.view = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    if (!CreateImage(tx.im, id)) {
        textures_.erase(d.data);
        return false;
    }
    return true;
}

// one level of one face from px (rows pitch apart) through the ring into the image
void VulkanBackend::StageLevel(VkCommandBuffer cb, const Image& im, uint32_t face, uint32_t level, uint32_t lw,
                               uint32_t lh, uint32_t ld, const std::vector<uint8_t>& px, uint32_t pitch, bool blocks) {
    uint32_t bpp = 4;
    uint32_t row = blocks ? (lw + 3) / 4 * (im.fmt == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : 16) : lw * bpp;
    uint32_t rows = blocks ? (lh + 3) / 4 : lh;
    VkDeviceSize bytes = VkDeviceSize(row) * rows * ld;
    Alloc a = Upload(bytes, 16);
    size_t src_rows = pitch ? px.size() / pitch / std::max(ld, 1u) : 0;
    for (uint32_t z = 0; z < ld; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = a.cpu + (size_t(z) * rows + y) * row;
            if (y < src_rows) {
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row));
                if (pitch < row) std::memset(dst + pitch, 0, row - pitch);
            } else {
                std::memset(dst, 0, row);
            }
        }
    }
    VkBufferImageCopy c{};
    c.bufferOffset = a.off;
    c.imageSubresource = {VkImageAspectFlags(IsDepth(im.fmt) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                          level, face, 1};
    c.imageExtent = {lw, lh, ld};
    vkCmdCopyBufferToImage(cb, a.buf, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, bool same) {
    VkCommandBuffer cb = Cur().up;
    VkImageAspectFlags aspect = IsDepth(tx.im.fmt) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    ImageBarrier(cb, tx.im.image, aspect, same ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    bool blocks = fi.compressed && bc_;
    bool decode = fi.compressed && !bc_;
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px, rgba;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, nullptr, px, pitch);
            uint32_t lw = std::max(tx.im.w >> l, 1u), lh = std::max(tx.im.h >> l, 1u);
            uint32_t ld = d.cube ? 1 : std::max(d.depth >> l, 1u);
            if (decode) {
                DecodeBc(fi.host, px, pitch, lw, lh, rgba);
                StageLevel(cb, tx.im, f, l, lw, lh, 1, rgba, lw * 4, false);
            } else {
                StageLevel(cb, tx.im, f, l, lw, lh, ld, px, pitch, blocks);
            }
        }
    }
    ImageBarrier(cb, tx.im.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
void VulkanBackend::BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages) {
    Tex& tx = textures_[d.data];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi)) return;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same) {
        tx.frame = presents_ + 1;
        uint64_t sig = MemSig(d.data, TotalSize(d)) ^ DirtyEpoch(d.data);
        if (sig != tx.sig || !same) {
            tx.sig = sig;
            UploadTexture(tx, d, fi, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), same);
        }
    }
    if (fi.depth) stages.info[stage].shadow = ShadowDepthKind(d.format);
    stages.levels[stage] = tx.levels;
    if (profile_) {
        bool bc = fi.compressed && bc_;
        (bc ? stats_.tex_bc : stats_.tex_raw)++;
        (tx.levels > 1 ? stats_.tex_mip : stats_.tex_nomip)++;
        if (tx.levels == 1 && d.width >= 256 && d.height >= 256) stats_.tex_nomip_big++;
        if (fi.compressed && d.levels > 1 && tx.levels == 1) stats_.tex_bc_padded++;
        stats_.tex_texels += uint64_t(d.width) * d.height;
    }
    stages.bind[stage] = {tx.im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

void VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    stages.info[stage] = {};
    stages.bind[stage] = {};
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return;
    const FormatInfo& fi = Format(d.format);
    stages.info[stage].kind = TextureKind(d);
    stages.info[stage].rect = d.linear;
    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(TargetKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        BindTargetTexture(s, stage, d, tit->second, stages);
        return;
    }
    BindGuestTexture(stage, d, fi, stages);
}

// border: the exact border colour (ARGB) a custom border sampler carries
VkSampler VulkanBackend::EnsureSampler(const VkSamplerCreateInfo& ci, uint64_t border) {
    VkSamplerCreateInfo k = ci;
    k.pNext = nullptr;
    uint64_t key = Hash(&k, sizeof(k), border);
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(dev_, &ci, nullptr, &s) != VK_SUCCESS) LOG_FIRST(4, "vulkan: sampler creation failed");
    return samplers_[key] = s;
}

VkSampler VulkanBackend::StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    ci.magFilter = lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = lin(min) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    uint32_t forced = app::Get().aniso;
    if (forced && lin(min) && mip && stages.levels[i] > 1 && !stages.info[i].shadow) an = forced;
    if (an > 1 && aniso_) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(float(an), props_.limits.maxSamplerAnisotropy);
        if (profile_) stats_.tex_aniso++;
    }
    ci.addressModeU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    ci.addressModeV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    ci.addressModeW = Address(t[kTssAddressW]);
    ci.mipLodBias = std::clamp(Bits(t[kTssMipLodBias]), -props_.limits.maxSamplerLodBias,
                               props_.limits.maxSamplerLodBias);
    ci.minLod = float(t[kTssMaxMipLevel]);
    ci.maxLod = mip ? VK_LOD_CLAMP_NONE : ci.minLod;
    ci.borderColor = Border(t[kTssBorderColor]);
    bool border = ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                  ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                  ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    VkSamplerCustomBorderColorCreateInfoEXT cb{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border && custom_border_) {
        Color(t[kTssBorderColor], cb.customBorderColor.float32);
        ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        ci.pNext = &cb;
    }
    if (stages.info[i].shadow) {
        // the console tests texel OP r, Vulkan r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        // a depth format the GPU only reads unfiltered (some Adreno and Mali) takes the nearest texel
        bool linear = (lin(min) || lin(mag)) && depth_linear_;
        ci.magFilter = ci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.compareEnable = VK_TRUE;
        ci.compareOp = VkCompareOp(swap[Cmp(s.rs[kRsShadowFunc])]);
        ci.anisotropyEnable = VK_FALSE;
        ci.maxAnisotropy = 0;
    }
    return EnsureSampler(ci, ci.pNext ? (uint64_t(t[kTssBorderColor]) | 1ull << 32) : 0);
}

void VulkanBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 2048;
    constexpr uint64_t kIdleFrames = 600;
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            Release(it->second.im);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- shaders and pipelines

// SPIR-V from HLSL, from the disk cache when it was made before; null when it fails to compile
VkShaderModule VulkanBackend::Module(const std::string& hlsl, EShLanguage stage, const char* entry) {
    std::string text = ForGlslang(screen_linear_workaround_ ? VulkanScreenLinearHlsl(hlsl) : hlsl);
    uint64_t hash = Hash(text.data(), text.size(), Hash(entry, std::strlen(entry), uint64_t(stage)));
    auto it = modules_.find(hash);
    if (it != modules_.end()) return it->second;
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.spv", (unsigned long long)hash);
    std::filesystem::path file = SpirvDir() / name;
    std::vector<uint32_t> spv;
    if (std::ifstream f(file, std::ios::binary | std::ios::ate); f && !cold_cache_) {
        spv.resize(size_t(f.tellg()) / 4);
        f.seekg(0);
        f.read(reinterpret_cast<char*>(spv.data()), std::streamsize(spv.size() * 4));
    }
    if (spv.empty()) {
        CpuTimer timer(profile_ ? &stats_.shader_ms : nullptr);
        if (profile_) stats_.shaders++;
        glslang::TShader sh(stage);
        const char* src = text.c_str();
        sh.setStrings(&src, 1);
        sh.setEntryPoint(entry);
        sh.setSourceEntryPoint(entry);
        sh.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
        sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        sh.setHlslIoMapping(true);
        sh.setAutoMapLocations(true);
        sh.setShiftBinding(glslang::EResUbo, 0);
        sh.setShiftBinding(glslang::EResTexture, 2);
        sh.setShiftBinding(glslang::EResSampler, 6);
        auto msg = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
        glslang::TProgram prog;
        bool ok = sh.parse(GetDefaultResources(), 100, false, msg);
        if (ok) {
            prog.addShader(&sh);
            ok = prog.link(msg) && prog.mapIO();
        }
        if (!ok) {
            LOG_FIRST(16, "vulkan: %s shader failed: %s %s\n%s", stage == EShLangVertex ? "vertex" : "pixel",
                      sh.getInfoLog(), prog.getInfoLog(), text.c_str());
            return modules_[hash] = VK_NULL_HANDLE;
        }
        glslang::GlslangToSpv(*prog.getIntermediate(stage), spv);
        std::error_code ec;
        if (!cold_cache_) std::filesystem::create_directories(SpirvDir(), ec);
        if (!cold_cache_) std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(spv.data()),
                                                    std::streamsize(spv.size() * 4));
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spv.size() * 4;
    ci.pCode = spv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev_, &ci, nullptr, &m) != VK_SUCCESS) LOG_FIRST(4, "vulkan: shader module failed");
    return modules_[hash] = m;
}

VkShaderModule VulkanBackend::VertexModule(const State& s, const Assembled& a, const DrawShaders& sh) {
    auto it = vmods_.find(sh.vkey);
    if (it != vmods_.end()) return it->second;
    std::string src = sh.prog ? VertexProgramHlsl(s.vs->program, a.regs, a.decode)
                              : FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
    src = VulkanVertexHlsl(std::move(src), sh.prog, (a.regs & (1 << 6)) != 0, large_points_);
    if (src.empty()) {
        LOGE("vulkan: unrecognized generated vertex shader");
        return vmods_[sh.vkey] = VK_NULL_HANDLE;
    }
    return vmods_[sh.vkey] = Module(sh.quad ? QuadHlsl(std::move(src)) : src, EShLangVertex, "main");
}

VkShaderModule VulkanBackend::PixelModule(const PixelKey& pk, uint64_t pkey) {
    auto it = pmods_.find(pkey);
    if (it != pmods_.end()) return it->second;
    return pmods_[pkey] = Module(PixelHlsl(pk), EShLangFragment, "main");
}

VkPrimitiveTopology PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    if (a.topology == Topology::Lines) return a.strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    return a.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, const DrawShaders& sh, Target* rt, Target* ds,
                                     PipeKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = sh.vkey;
    k.ps = sh.pkey ^ kPixelKeySalt;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.buffers = a.buffer_count;
    for (uint32_t i = 0; i < a.buffer_count; i++) k.strides[i] = a.buffers[i].stride;
    k.topo = PrimitiveTopology(a);
    k.rtfmt = rt ? kBgra : 0;
    k.ds = ds != nullptr;
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // the console's enables are one per byte, A R G B; Vulkan wants bits R G B A
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
    // a depth buffer attached for the pass's sake while the draw has depth off tests and writes nothing
    bool depth_on = ds && (s.rs[kRsZEnable] || s.rs[kRsStencilEnable]);
    if (depth_on) {
        k.zen = s.rs[kRsZEnable] != 0;
        if (k.zen) {
            k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
            k.zfunc = SimpleState(s, kMZFunc);
        }
        k.sten = s.rs[kRsStencilEnable] != 0;
        if (k.sten) {
            k.sfunc = SimpleState(s, kMStencilFunc);
            k.smask = SimpleState(s, kMStencilMask) & 0xFF;
            k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
            k.sfail = s.rs[kRsStencilFail];
            k.szfail = SimpleState(s, kMStencilZFail);
            k.spass = SimpleState(s, kMStencilPass);
        }
        if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
            k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
            k.zslope = Bits(SimpleState(s, kMSlopeScale)) * float(ds->w) / ds->gw;
        }
    }
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
    (void)sh;
}

VkPipeline VulkanBackend::EnsurePipeline(const PipeKey& k, VkShaderModule vs, VkShaderModule ps) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = pipelines_.find(h);
    if (it != pipelines_.end()) return it->second;
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = ps;
    st[1].pName = "main";
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t na = 0;
    for (uint32_t i = 0; i < k.buffers; i++) vb[i] = {i, k.strides[i], VK_VERTEX_INPUT_RATE_VERTEX};
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        va[na++] = {r, (e >> 8) & 0xFF, VertexFormat(uint8_t(e & 0xFF)), e >> 16};
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = k.buffers;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VkPrimitiveTopology(k.topo);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.fill == kFillWireframe && wireframe_ ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zbias || k.zslope;
    rs.depthBiasConstantFactor = float(k.zbias);
    rs.depthBiasSlopeFactor = k.zslope;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = k.zen;
    dss.depthWriteEnable = k.zwrite;
    dss.depthCompareOp = VkCompareOp(Cmp(k.zfunc));
    dss.stencilTestEnable = k.sten;
    VkStencilOpState so{StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), VkCompareOp(Cmp(k.sfunc)),
                        k.smask, k.swmask, 0};
    dss.front = dss.back = so;
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = k.blend;
    cb.srcColorBlendFactor = Blend(k.src, false);
    cb.dstColorBlendFactor = Blend(k.dst, false);
    cb.colorBlendOp = BlendOp(k.op);
    cb.srcAlphaBlendFactor = Blend(k.src, true);
    cb.dstAlphaBlendFactor = Blend(k.dst, true);
    cb.alphaBlendOp = BlendOp(k.op);
    cb.colorWriteMask = k.wmask;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = k.rtfmt ? 1 : 0;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 4;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.renderPass = Pass(VkFormat(k.rtfmt), k.ds, false, VK_IMAGE_LAYOUT_GENERAL);
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &dss;
    ci.pColorBlendState = &cbs;
    ci.pDynamicState = &ds;
    ci.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    CpuTimer timer(profile_ ? &stats_.pipeline_ms : nullptr);
    if (profile_) stats_.pipelines++;
    if (vkCreateGraphicsPipelines(dev_, cache_, 1, &ci, nullptr, &p) != VK_SUCCESS) {
        LOG_FIRST(8, "vulkan: pipeline creation failed");
    }
    cache_dirty_ = true;
    return pipelines_[h] = p;
}

VkPipeline VulkanBackend::EnsureBlitPipeline(VkFormat fmt, bool over) {
    uint64_t h = Hash(&fmt, sizeof(fmt), over ? 0xB117 : 0xB11);
    auto it = pipelines_.find(h);
    if (it != pipelines_.end()) return it->second;
    VkShaderModule vs = Module(kBlitHlsl, EShLangVertex, "vs");
    VkShaderModule ps = Module(kBlitHlsl, EShLangFragment, "psmain");
    if (!vs || !ps) return pipelines_[h] = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "vs";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = ps;
    st[1].pName = "psmain";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    if (over) {
        // premultiplied colour, the target keeps its alpha
        cb.blendEnable = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    }
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.renderPass = Pass(fmt, false, false, VK_IMAGE_LAYOUT_GENERAL);
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &dss;
    ci.pColorBlendState = &cbs;
    ci.pDynamicState = &ds;
    ci.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    CpuTimer timer(profile_ ? &stats_.pipeline_ms : nullptr);
    if (profile_) stats_.pipelines++;
    if (vkCreateGraphicsPipelines(dev_, cache_, 1, &ci, nullptr, &p) != VK_SUCCESS) {
        LOGE("vulkan: blit pipeline failed");
    }
    return pipelines_[h] = p;
}

VkDescriptorSet VulkanBackend::AllocSet() {
    Frame& f = Cur();
    for (;;) {
        if (f.set_pool == f.pools.size()) {
            VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSetsPerPool * 2},
                                             {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerPool * 4},
                                             {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerPool * 4}};
            VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            ci.maxSets = kSetsPerPool;
            ci.poolSizeCount = 3;
            ci.pPoolSizes = sizes;
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(dev_, &ci, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
            f.pools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.pools[f.set_pool];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(dev_, &ai, &set) == VK_SUCCESS) return set;
        f.set_pool++;
    }
}

void VulkanBackend::BindSet(const Alloc& vc, VkDeviceSize vc_size, const Alloc& dc, VkDeviceSize dc_size,
                            const StageView views[4], const VkSampler samplers[4]) {
    CpuTimer timer(profile_ ? &stats_.descriptor_ms : nullptr);
    DescriptorKey key;
    key.buffers = {vc.buf, dc.buf};
    key.ranges = {vc_size, dc_size};
    for (size_t i = 0; i < 4; i++) {
        key.views[i] = views[i].view;
        key.layouts[i] = views[i].layout;
        key.samplers[i] = samplers[i];
    }
    uint32_t offsets[2] = {uint32_t(vc.off), uint32_t(dc.off)};
    if (descriptor_cache_) {
        auto it = Cur().sets.find(key);
        if (it != Cur().sets.end()) {
            if (profile_) stats_.hits++;
            vkCmdBindDescriptorSets(Cur().cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &it->second, 2, offsets);
            return;
        }
    }
    VkDescriptorSet set = AllocSet();
    if (!set) return;
    if (profile_) stats_.sets++;
    VkDescriptorBufferInfo bi[2] = {{vc.buf, 0, vc_size}, {dc.buf, 0, dc_size}};
    VkDescriptorImageInfo ii[4], si[4];
    VkWriteDescriptorSet w[10];
    for (uint32_t i = 0; i < 10; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        if (i < 2) {
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w[i].pBufferInfo = &bi[i];
        } else if (i < 6) {
            ii[i - 2] = {VK_NULL_HANDLE, views[i - 2].view, views[i - 2].layout};
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[i].pImageInfo = &ii[i - 2];
        } else {
            si[i - 6] = {samplers[i - 6], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            w[i].pImageInfo = &si[i - 6];
        }
    }
    vkUpdateDescriptorSets(dev_, 10, w, 0, nullptr);
    if (descriptor_cache_) Cur().sets.emplace(key, set);
    vkCmdBindDescriptorSets(Cur().cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 2, offsets);
}

// a quad over the viewport the caller set, into the pass the caller began
void VulkanBackend::Blit(const BlitArgs& b) {
    VkPipeline p = EnsureBlitPipeline(b.fmt, b.over);
    if (!p || !b.src) return;
    VkCommandBuffer cmd = Cur().cmd;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    float c[16] = {b.uv[0], b.uv[1], b.uv[2], b.uv[3], b.gamma ? 1.0f : 0.0f, b.alpha ? 1.0f : 0.0f, float(b.taps),
                   b.mul};
    std::memcpy(c + 8, b.xf, sizeof(b.xf));
    std::memcpy(c + 12, b.rot, sizeof(b.rot));
    Alloc cb = Upload(sizeof(c), ubo_align_);
    std::memcpy(cb.cpu, c, sizeof(c));
    StageView views[4] = {{b.src, b.layout},
                          {lut_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                          {dummy_[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                          {dummy_[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkSampler smp[4] = {linear_, point_, point_, point_};
    BindSet(cb, sizeof(c), cb, sizeof(c), views, smp);
    vkCmdDraw(cmd, 4, 1, 0, 0);
    if (timing_pass_) Cur().timed_passes[Cur().timed_count].draws++;
    if (trace_passes_) trace_draws_++;
}

// ---- the title's calls

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    CpuTimer timer(profile_ ? &stats_.draw_ms : nullptr);
    if (profile_) stats_.draws++;
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    // a level's first frame draws everything once: past the ring every upload would be a buffer of its own
    if (Cur().used > kRingSize - kRingReserve) Flush();
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return;
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    draw_rt_ = rt;
    draw_ds_ = ds;

    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog && !s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
    sh.vkey = VertexKey(s, a, sh);

    DrawTextures stages;
    bool reads_depth = false;
    uint32_t sampled = skip_unused_stages_ ? SampledTextureStages(s) : 0xF;
    for (int i = 0; i < 4; i++) {
        if (!(sampled & (1u << i))) {
            if (profile_ && s.textures[size_t(i)]) stats_.unused_stages++;
            continue;
        }
        BindTexture(s, i, stages);
        reads_depth |= active_ds_ && stages.bind[i].view == active_ds_->im.view;
    }
    if (!draw_rt_ && !draw_ds_) return;  // a texture's target took the draw's place
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    VkShaderModule vs = VertexModule(s, a, sh);
    VkShaderModule ps = PixelModule(pk, sh.pkey);
    if (!vs || !ps) return;

    // the depth buffer of the draws before stays attached when this one has depth off: the pass goes on
    if (!ds && rt && !reads_depth && active_ && !other_ && active_rt_ == rt && active_ds_) ds = active_ds_;
    BeginRendering(rt, ds);
    PipeKey k;
    BuildPipelineKey(s, a, sh, rt, ds, k);
    VkPipeline pipe = EnsurePipeline(k, vs, ps);
    if (!pipe) return;
    VkCommandBuffer cmd = Cur().cmd;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

    GuestSize size;
    SurfaceSize(rt ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    auto point_size = [](float value, float fallback) { return std::isfinite(value) && value > 0 ? value : fallback; };
    float point_max = large_points_ ? props_.limits.pointSizeRange[1] : 1.0f;
    float point_min = large_points_ ? props_.limits.pointSizeRange[0] : 1.0f;
    c.hdr[3] = std::clamp(point_size(Bits(s.rs[kRsPointSizeMax]), point_max), point_min, point_max);
    c.surf[3] = std::clamp(point_size(Bits(s.rs[kRsPointSizeMin]), 1.0f), point_min, c.hdr[3]);
    c.hdr[2] = point_size(Bits(s.rs[kRsPointSize]), 1.0f);
    FillCombinerConstants(s, c);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    Alloc vc = Upload(sizeof(s.vsc), ubo_align_);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    Alloc dc = Upload(sizeof(c), ubo_align_);
    std::memcpy(dc.cpu, &c, sizeof(c));
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    VkSampler smp[4];
    for (int i = 0; i < 4; i++) {
        smp[i] = StageSampler(s, i, stages, clamp);
        if (!stages.bind[i].view) {
            const TexInfo& ti = stages.info[i];
            int kind = ti.shadow ? 3 : ti.kind == kTex3D ? 1 : ti.kind == kTexCube ? 2 : 0;
            stages.bind[i] = {dummy_[kind].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
    }
    BindSet(vc, sizeof(s.vsc), dc, sizeof(c), stages.bind, smp);

    // the viewport covers the target, the guest viewport is in the shader; the scissor clips to it
    Target* t = rt ? rt : ds;
    SetViewport(0, 0, float(t->w), float(t->h));
    float sc = float(t->w) / size.w;
    int32_t x0 = int32_t(s.viewport.x * sc), y0 = int32_t(s.viewport.y * sc);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc), y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc));
        y0 = std::max(y0, int32_t(s.scissors[1] * sc));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc));
        y1 = std::min(y1, int32_t(s.scissors[3] * sc));
    }
    x0 = std::clamp(x0, 0, int32_t(t->w));
    y0 = std::clamp(y0, 0, int32_t(t->h));
    x1 = std::clamp(x1, x0, int32_t(t->w));
    y1 = std::clamp(y1, y0, int32_t(t->h));
    VkRect2D scissor{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    vkCmdSetBlendConstants(cmd, bf);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);

    VkBuffer bufs[16];
    VkDeviceSize offs[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(std::max<uint32_t>(b.bytes, 4), 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        bufs[i] = vb.buf;
        offs[i] = vb.off;
    }
    if (timing_pass_) Cur().timed_passes[Cur().timed_count].draws++;
    if (trace_passes_) trace_draws_++;
    if (a.buffer_count) vkCmdBindVertexBuffers(cmd, 0, a.buffer_count, bufs, offs);
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        vkCmdBindIndexBuffer(cmd, ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        vkCmdDraw(cmd, a.draw_count, 1, 0, 0);
    }
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                          uint32_t stencil) {
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) {
        Clear(s, count, rects, flags & ~(kClearZ | kClearStencil), color, z, stencil);
        rt = nullptr;
    }
    if (!rt && !ds) return;
    // inside the pass that has them, else a pass of its own
    Target* use_rt = rt;
    Target* use_ds = ds;
    if (active_ && !other_ && (!rt || rt == active_rt_) && (!ds || ds == active_ds_)) {
        use_rt = active_rt_;
        use_ds = active_ds_;
    }
    bool new_pass = !active_ || other_ || active_rt_ != use_rt || active_ds_ != use_ds;
    BeginRendering(use_rt, use_ds);
    Target* t = use_rt ? use_rt : use_ds;
    float sc = float(t->w) / (rt ? rt->gw : ds->gw);
    std::vector<VkClearRect> rs;
    auto add = [&](int x0, int y0, int x1, int y1) {
        int32_t l = std::clamp(int32_t(x0 * sc), 0, int32_t(t->w)), tp = std::clamp(int32_t(y0 * sc), 0, int32_t(t->h));
        int32_t r = std::clamp(int32_t(x1 * sc), 0, int32_t(t->w)), b = std::clamp(int32_t(y1 * sc), 0, int32_t(t->h));
        if (r > l && b > tp) rs.push_back({{{l, tp}, {uint32_t(r - l), uint32_t(b - tp)}}, 0, 1});
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
            add(r[0], r[1], r[2], r[3]);
        }
    } else {
        const auto& v = s.viewport;
        add(int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h));
    }
    if (rs.empty()) return;
    VkClearAttachment at[2];
    uint32_t n = 0;
    if (rt) {
        at[n] = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        Color(color, at[n].clearValue.color.float32);
        n++;
    }
    if (ds) {
        VkImageAspectFlags aspect = (flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0;
        if ((flags & kClearStencil) && HasStencil(depth_fmt_)) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (aspect) {
            at[n] = {aspect, 0, {}};
            at[n].clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
            n++;
        }
    }
    if (profile_ && n) {
        bool full = FullAttachmentClear(rs.data(), rs.size(), t->w, t->h);
        stats_.clears++;
        if (new_pass && full) {
            stats_.full_clear_starts++;
            uint64_t pixels = uint64_t(t->w) * t->h;
            for (uint32_t i = 0; i < n; i++) {
                if (at[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) stats_.clear_color_pixels += pixels;
                if (at[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) stats_.clear_depth_pixels += pixels;
                if (at[i].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) stats_.clear_stencil_pixels += pixels;
            }
        }
    }
    if (n) vkCmdClearAttachments(Cur().cmd, n, at, uint32_t(rs.size()), rs.data());
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                         uint32_t dst_points) {
    Target* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    EndRendering(PassEnd::Copy);
    float sc = float(dst_t->w) / dst_t->gw;
    if (float ss = float(src_t->w) / src_t->gw; std::fabs(ss - sc) > 1e-3f) {
        // E1 targets keep the console's size: each side scales its own rectangle, filtered
        auto at = [](float v, float s, uint32_t lim) { return int32_t(std::min(uint32_t(std::max(v, 0.0f) * s + 0.5f), lim)); };
        std::vector<VkImageBlit> blits;
        bool listed = count && src_rects;
        for (uint32_t i = 0; i < (listed ? count : 1); i++) {
            float l = 0, t = 0, r = float(std::min(src_t->gw, dst_t->gw)), b = float(std::min(src_t->gh, dst_t->gh));
            if (listed) {
                uint32_t p = src_rects + i * 16;
                l = float(int32_t(kernel::R32(p))), t = float(int32_t(kernel::R32(p + 4)));
                r = float(int32_t(kernel::R32(p + 8))), b = float(int32_t(kernel::R32(p + 12)));
            }
            float dx = l, dy = t;
            if (listed && dst_points) {
                dx = float(int32_t(kernel::R32(dst_points + i * 8)));
                dy = float(int32_t(kernel::R32(dst_points + i * 8 + 4)));
            }
            VkImageBlit bl{};
            bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bl.dstSubresource = bl.srcSubresource;
            bl.srcOffsets[0] = {at(l, ss, src_t->w), at(t, ss, src_t->h), 0};
            bl.srcOffsets[1] = {at(r, ss, src_t->w), at(b, ss, src_t->h), 1};
            bl.dstOffsets[0] = {at(dx, sc, dst_t->w), at(dy, sc, dst_t->h), 0};
            bl.dstOffsets[1] = {at(dx + r - l, sc, dst_t->w), at(dy + b - t, sc, dst_t->h), 1};
            if (bl.srcOffsets[1].x <= bl.srcOffsets[0].x || bl.srcOffsets[1].y <= bl.srcOffsets[0].y ||
                bl.dstOffsets[1].x <= bl.dstOffsets[0].x || bl.dstOffsets[1].y <= bl.dstOffsets[0].y)
                continue;
            blits.push_back(bl);
        }
        if (blits.empty()) return;
        vkCmdBlitImage(Cur().cmd, src_t->im.image, VK_IMAGE_LAYOUT_GENERAL, dst_t->im.image, VK_IMAGE_LAYOUT_GENERAL,
                       uint32_t(blits.size()), blits.data(), VK_FILTER_LINEAR);
        GlobalBarrier();
        dst_t->drawn = true;
        return;
    }
    auto scaled = [&](int32_t v) { return uint32_t(uint32_t(v) * sc); };
    std::vector<VkImageCopy> regions;
    bool listed = count && src_rects;
    for (uint32_t i = 0; i < (listed ? count : 1); i++) {
        uint32_t l = 0, t = 0, r = std::min(src_t->w, dst_t->w), b = std::min(src_t->h, dst_t->h), dx = 0, dy = 0;
        if (listed) {
            uint32_t at = src_rects + i * 16;
            l = scaled(int32_t(kernel::R32(at)));
            t = scaled(int32_t(kernel::R32(at + 4)));
            r = std::min(scaled(int32_t(kernel::R32(at + 8))), src_t->w);
            b = std::min(scaled(int32_t(kernel::R32(at + 12))), src_t->h);
            dx = l;
            dy = t;
            if (dst_points) {
                dx = scaled(int32_t(kernel::R32(dst_points + i * 8)));
                dy = scaled(int32_t(kernel::R32(dst_points + i * 8 + 4)));
            }
        }
        if (r <= l || b <= t || dx >= dst_t->w || dy >= dst_t->h) continue;
        r = std::min(r, l + dst_t->w - dx);
        b = std::min(b, t + dst_t->h - dy);
        VkImageCopy c{};
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.dstSubresource = c.srcSubresource;
        c.srcOffset = {int32_t(l), int32_t(t), 0};
        c.dstOffset = {int32_t(dx), int32_t(dy), 0};
        c.extent = {r - l, b - t, 1};
        regions.push_back(c);
    }
    if (regions.empty()) return;
    vkCmdCopyImage(Cur().cmd, src_t->im.image, VK_IMAGE_LAYOUT_GENERAL, dst_t->im.image, VK_IMAGE_LAYOUT_GENERAL,
                   uint32_t(regions.size()), regions.data());
    GlobalBarrier();
    dst_t->drawn = true;
}

// the CPU reads a surface the GPU drew: bring the pixels back, at the console's size
void VulkanBackend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    if (profile_) stats_.syncs++;
    Target& t = it->second;
    Image down;
    VkImage src = GuestSizedSource(t, down);
    if (!src) return;
    Buffer rb;
    VkDeviceSize bytes = VkDeviceSize(t.gw) * t.gh * 4;
    if (!CreateBuffer(rb, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
        !CreateBuffer(rb, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        Release(down);
        return;
    }
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {t.gw, t.gh, 1};
    vkCmdCopyImageToBuffer(Cur().cmd, src, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &c);
    HostBarrier();
    Release(down);
    Flush();
    std::vector<uint8_t> px(rb.map, rb.map + bytes);
    DestroyBuffer(rb);
    // colour targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// the target as a BGRA image at the console's size, shrunk into down (made when empty) when it is upscaled; outside
// any pass, null when it fails
VkImage VulkanBackend::GuestSizedSource(Target& t, Image& down) {
    EndRendering(PassEnd::Readback);
    if (t.w == t.gw && t.h == t.gh) return t.im.image;
    if (!down.image) {
        ImageDesc d{kBgra, t.gw, t.gh};
        d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!CreateImage(down, d)) return VK_NULL_HANDLE;
        InitTargetImage(down, false);
    }
    BeginRenderingView(down.view, kBgra, VK_IMAGE_LAYOUT_GENERAL, t.gw, t.gh, false);
    SetViewport(0, 0, float(t.gw), float(t.gh));
    VkRect2D all{{0, 0}, {t.gw, t.gh}};
    vkCmdSetScissor(Cur().cmd, 0, 1, &all);
    BlitArgs b;
    b.src = t.im.view;
    b.alpha = true;
    b.taps = uint32_t(std::ceil(float(t.w) / t.gw - 1e-3f));
    Blit(b);
    EndRendering(PassEnd::Readback);
    return down.image;
}

// copies to buffers the CPU reads once the frame's fence is through
void VulkanBackend::HostBarrier() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cur().cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
}

// any thread: the newest finished copy into guest memory, false when there is none recent
bool VulkanBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = fences_.Completed(), now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kLateSlots; i++) {
        bool finished = l.serial[i] && l.serial[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kLateSlots) + 1;
        if (finished && recent && (best < 0 || l.serial[i] > l.serial[best])) best = i;
    }
    if (best < 0) return false;
    if (l.serial[best] > l.delivered) {
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) {
            std::memcpy(dst + size_t(y) * pitch, l.rb[best].map + size_t(y) * l.gw * 4, l.gw * 4);
        }
        l.delivered = l.serial[best];
    }
    return true;
}

// render thread, in order with the draws: a copy of the surface for a later lock
void VulkanBackend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end()) return;
    Target& t = it->second;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        for (Buffer& b : l.rb) {
            if (b.buf) Cur().dead_buffers.push_back(b);
        }
        Release(l.down);
        l = Late{};
        l.gw = t.gw;
        l.gh = t.gh;
    }
    // unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.serial[slot] > fences_.Completed()) return;
    VkDeviceSize bytes = VkDeviceSize(t.gw) * t.gh * 4;
    if (!l.rb[slot].buf &&
        !CreateBuffer(l.rb[slot], bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
        !CreateBuffer(l.rb[slot], bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        return;
    }
    VkImage src = GuestSizedSource(t, l.down);
    if (!src) return;
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {t.gw, t.gh, 1};
    vkCmdCopyImageToBuffer(Cur().cmd, src, VK_IMAGE_LAYOUT_GENERAL, l.rb[slot].buf, 1, &c);
    HostBarrier();
    GlobalBarrier();
    l.serial[slot] = fences_.NextSerial();  // the next submission carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kLateSlots;
}

void VulkanBackend::UploadGammaRamp(uint32_t brightness, uint32_t contrast) {
    ramp_set_ = brightness << 8 | contrast;
    float k = std::exp2((float(contrast) - 50.0f) / 50.0f);
    float g = std::exp2((50.0f - float(brightness)) / 50.0f);
    std::vector<uint8_t> px(256 * 4);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        float x = i / 255.0f;
        float y = x < 0.5f ? 0.5f * std::pow(2.0f * x, k) : 1.0f - 0.5f * std::pow(2.0f - 2.0f * x, k);
        uint8_t o = uint8_t(std::lround(std::clamp(std::pow(y, g), 0.0f, 1.0f) * 255.0f));
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = o;
        px[i * 4 + 3] = 255;
        identity &= o == i;
    }
    gamma_ = !identity;
    VkCommandBuffer cb = Cur().up;
    ImageBarrier(cb, lut_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    StageLevel(cb, lut_, 0, 0, 256, 1, 1, px, 256 * 4, false);
    ImageBarrier(cb, lut_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

// the frame onto the swapchain image, at the game's aspect, turned to the panel's orientation
void VulkanBackend::PresentBlit(const Target& t, bool pillarbox) {
    bool sideways = transform_ & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    float lw = float(sideways ? swap_h_ : swap_w_);
    float lh = float(sideways ? swap_w_ : swap_h_);
    float aspect = app::Aspect();
    float vw = std::min(lw, lh * aspect);
    float vh = vw / aspect;
    if (pillarbox) vw = vh * 4.0f / 3.0f;
    BlitArgs b;
    b.src = t.im.view;
    b.gamma = gamma_;
    b.fmt = swap_fmt_;
    b.xf[0] = vw / lw;
    b.xf[1] = vh / lh;
    float r[4] = {1, 0, 0, 1};
    // clip space y up, as the blit's quad: the panel turned 90 degrees takes the frame turned back
    if (transform_ & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) r[0] = 0, r[1] = 1, r[2] = -1, r[3] = 0;
    if (transform_ & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) r[0] = -1, r[3] = -1;
    if (transform_ & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) r[0] = 0, r[1] = -1, r[2] = 1, r[3] = 0;
    std::memcpy(b.rot, r, sizeof(r));
    SetViewport(0, 0, float(swap_w_), float(swap_h_));
    VkRect2D all{{0, 0}, {swap_w_, swap_h_}};
    vkCmdSetScissor(Cur().cmd, 0, 1, &all);
    Blit(b);
}

// false without an image this frame: the window is gone or the swapchain is being made again
bool VulkanBackend::AcquireImage(uint32_t& index) {
    CpuTimer timer(profile_ ? &stats_.acquire_ms : nullptr);
    if (recreate_ || !swapchain_) {
        if (swapchain_) DestroySwapchain();
        if (surface_ && !CreateSwapchain()) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (!surface_ && CreateSurface() && !CreateSwapchain()) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        recreate_ = false;
        if (!swapchain_) return false;
    }
    VkResult r = vkAcquireNextImageKHR(dev_, swapchain_, 1000000000ull, Cur().acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) recreate_ = true;
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

void VulkanBackend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        app::g_fps = float(fps_n_ / el);
        if (presents_ % 600 < fps_n_) LOGI("vulkan: %.1f fps", app::g_fps.load());
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void VulkanBackend::LogFrame() {
    auto now = std::chrono::steady_clock::now();
    double ms = frame_t_ == decltype(frame_t_){} ? 0 : std::chrono::duration<double, std::milli>(now - frame_t_).count();
    uint64_t shaders = stats_.shaders - frame_shaders_, pipelines = stats_.pipelines - frame_pipelines_;
    double shader_ms = stats_.shader_ms - frame_shader_ms_, pipeline_ms = stats_.pipeline_ms - frame_pipeline_ms_;
    if (ms > 25 || shaders || pipelines)
        LOGI("vk-frame: present=%llu interval=%.2fms compile=%llu/%.2fms pipelines=%llu/%.2fms",
             (unsigned long long)presents_, ms, (unsigned long long)shaders, shader_ms, (unsigned long long)pipelines,
             pipeline_ms);
    frame_t_ = now;
}

void VulkanBackend::LogStats() {
    LogFrame();
    if (++stats_.frames < 120) {
        frame_shaders_ = stats_.shaders, frame_pipelines_ = stats_.pipelines;
        frame_shader_ms_ = stats_.shader_ms, frame_pipeline_ms_ = stats_.pipeline_ms;
        return;
    }
    const auto& s = stats_;
    double n = double(s.frames);
    LOGI("vk-perf: cache=%s frames=%llu draw=%.2fms descriptors=%.2fms wait=%.2fms acquire=%.2fms present=%.2fms "
         "gpu=%.2fms/%llu submits draws=%.1f sets=%.1f hits=%.1f writes=%.1f upload=%.2fMiB passes=%.1f "
         "flush=%llu sync=%llu compile=%llu/%.2fms pipelines=%llu/%.2fms",
         descriptor_cache_ ? "on" : "off", (unsigned long long)s.frames, s.draw_ms / n, s.descriptor_ms / n,
         s.wait_ms / n, s.acquire_ms / n, s.present_ms / n, s.gpu_samples ? s.gpu_ms / s.gpu_samples : 0,
         (unsigned long long)s.gpu_samples, s.draws / n, s.sets / n, s.hits / n, 10.0 * s.sets / n,
         s.upload_bytes / n / (1024 * 1024), s.passes / n, (unsigned long long)s.flushes, (unsigned long long)s.syncs,
         (unsigned long long)s.shaders, s.shader_ms, (unsigned long long)s.pipelines, s.pipeline_ms);
    LOGI("vk-lifetime: ends/frame targets=%.1f auxiliary=%.1f feedback=%.1f copy=%.1f readback=%.1f resize=%.1f "
         "present=%.1f submit=%.1f clears=%.1f full-start=%.1f clear-Mpixels color=%.3f depth=%.3f stencil=%.3f",
         s.pass_ends[0] / n, s.pass_ends[1] / n, s.pass_ends[2] / n, s.pass_ends[3] / n,
         s.pass_ends[4] / n, s.pass_ends[5] / n, s.pass_ends[6] / n, s.pass_ends[7] / n,
         s.clears / n, s.full_clear_starts / n, s.clear_color_pixels / n / 1e6,
         s.clear_depth_pixels / n / 1e6, s.clear_stencil_pixels / n / 1e6);
    LOGI("vk-stages: skip-unused=%s skipped/frame=%.1f", skip_unused_stages_ ? "on" : "off", s.unused_stages / n);
    LOGI("vk-textures: binds/frame bc=%.1f raw=%.1f mip=%.1f nomip=%.1f nomip>=256=%.1f bc-mips-dropped=%.1f "
         "rt=%.1f aniso-samplers=%.1f avg-texels=%.0f",
         s.tex_bc / n, s.tex_raw / n, s.tex_mip / n, s.tex_nomip / n, s.tex_nomip_big / n, s.tex_bc_padded / n,
         s.tex_rt / n, s.tex_aniso / n,
         double(s.tex_texels) / std::max<uint64_t>(s.tex_bc + s.tex_raw + s.tex_rt, 1));
    LOGI("vk-barriers: pass-end=%s pass-timestamps=%s", narrow_barriers_ ? "narrow" : "global",
         profile_passes_ ? "on" : "off");
    std::vector<std::pair<uint64_t, PassStats>> top(pass_stats_.begin(), pass_stats_.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    LOGI("vk-passes: sampled=%zu gpu-submissions=%llu (first %u passes per submission)",
         top.size(), (unsigned long long)s.gpu_samples, kTimedPasses);
    for (size_t i = 0; i < std::min<size_t>(top.size(), 32); i++) {
        const auto& [key, p] = top[i];
        double samples = double(std::max<uint64_t>(s.gpu_samples, 1));
        LOGI("vk-pass: target=%08X size=%ux%u ms/submit=%.3f passes/submit=%.2f draws/pass=%.1f",
             uint32_t(key), uint32_t((key >> 32) & 0xFFFF), uint32_t(key >> 48), p.ms / samples,
             p.count / samples, double(p.draws) / p.count);
    }
    pass_stats_.clear();
    stats_ = {};
    frame_shaders_ = frame_pipelines_ = 0;
    frame_shader_ms_ = frame_pipeline_ms_ = 0;
    descriptor_cache_ = DebugFlag("debug.scda.vk_descriptor_cache", true);
    skip_unused_stages_ = DebugFlag("debug.scda.vk_skip_unused_stages", true);
    narrow_barriers_ = DebugFlag("debug.scda.vk_narrow_barriers");
    // Trace one frame per report while requested; exclude trace windows from benchmarks.
    trace_passes_ = DebugFlag("debug.scda.vk_trace");
}

// Called between frames, after the old frame was submitted. Retire its images with the new frame, not while the
// GPU (or a late readback) may still be using them. Shader and pipeline caches do not depend on target dimensions.
void VulkanBackend::ApplySettings() {
    float scale;
    uint32_t shadows;
    bool scaled, native;
    {
        std::lock_guard lock(app::UiLock());
        scale = app::RenderScale(guest_h_);
        shadows = app::Get().shadow_resolution;
        scaled = app::Get().scaled_presentation;
        native = app::Get().native_effect_size;
        effect_single_tap_ = app::Get().fast_glow;  // read per draw, no target change
    }
    if (scaled != present_scaled_) {
        present_scaled_ = scaled;
        recreate_ = true;  // the next present makes the swapchain at the new size
    }
    if (native != effect_native_) {
        effect_native_ = native;
        rebuild_targets_ = true;  // the effect targets are made again at their new sizes
    }
    if (std::fabs(scale - scale_) < 1e-4f && shadows == shadow_res_ && !rebuild_targets_) return;
    rebuild_targets_ = false;
    if (present_scaled_) recreate_ = true;  // the swapchain follows the render scale
    for (auto& [key, t] : targets_) {
        Release(t.im);
        Release(t.fb);
    }
    targets_.clear();
    draw_rt_ = draw_ds_ = nullptr;
    {
        std::lock_guard lock(late_lock_);
        for (auto& [data, l] : late_) {
            Release(l.down);
            for (Buffer& b : l.rb)
                if (b.buf) Cur().dead_buffers.push_back(b);
        }
        late_.clear();
    }
    scale_ = scale;
    shadow_res_ = shadows;
    LOGI("vulkan: settings applied, render scale %.3f, shadow resolution %u, effect targets %s, effect taps %s", scale_,
         shadow_res_, effect_native_ ? "console size" : "scaled", effect_single_tap_ ? "single" : "averaged");
}

void VulkanBackend::Present(const Surface& front, bool pillarbox) {
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (const app::Settings& set = app::Get(); (set.brightness << 8 | set.contrast) != ramp_set_)
        UploadGammaRamp(set.brightness, set.contrast);
    EndRendering(PassEnd::Present);
    uint32_t img = kNone;
    if (!AcquireImage(img)) img = kNone;
    VkCommandBuffer cmd = Cur().cmd;
    if (img != kNone) {
        ImageBarrier(cmd, swap_images_[img], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        BeginRenderingView(swap_views_[img], swap_fmt_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, swap_w_, swap_h_, true);
        if (t) PresentBlit(*t, pillarbox);
        EndRendering(PassEnd::Present);
        ImageBarrier(cmd, swap_images_[img], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    Submit(img != kNone ? Cur().acquired : VK_NULL_HANDLE, img != kNone ? rendered_[img] : VK_NULL_HANDLE);
    fences_.Present(presents_);
    if (img != kNone) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered_[img];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &img;
        CpuTimer timer(profile_ ? &stats_.present_ms : nullptr);
        VkResult r = vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR || r == VK_SUBOPTIMAL_KHR) recreate_ = true;
    }
    trace_passes_ = false;
    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    BeginFrame();
    ReleaseOldTextures();
    ApplySettings();
    CountFps();
    if (profile_) LogStats();
    // Android ends the app without notice: the pipelines go to disk now and then
    constexpr uint64_t kSaveEvery = 600;
    if (presents_ % kSaveEvery == 0) SavePipelines();
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<VulkanBackend>(); }

void SavePipelineCache() {
    if (g_vk) g_vk->SavePipelines();
}

}  // namespace gpu
