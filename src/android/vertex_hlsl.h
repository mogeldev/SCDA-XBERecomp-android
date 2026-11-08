#pragma once

#include <string>
#include <cstdint>

namespace gpu {

// Keep the Vulkan-only built-in out of the shared D3D vertex/pixel interface.
inline std::string VulkanVertexHlsl(std::string src, bool program, bool vertex_size, bool large_points) {
    size_t declaration = src.find("struct VOut {");
    size_t result = src.find("    return o;");
    if (declaration == std::string::npos || result == std::string::npos) return {};
    std::string size = program ? "oPts.x" : vertex_size ? "i.v6.x" : "hdr.z";
    // hdr.z: fixed size, surf.w/hdr.w: lower/upper size limits, supplied per draw.
    src.insert(result, "    o.point_size = " + (large_points ? "clamp(" + size + ", surf.w, hdr.w)" : "1.0") + ";\n");
    src.insert(declaration + std::string("struct VOut {").size(), "\n    [[vk::builtin(\"PointSize\")]] float point_size : PSIZE;");
    return src;
}

// Limit the workaround to the GPU/driver combination confirmed on the Huawei AGS3K.
inline bool NeedsScreenLinearWorkaround(uint32_t vendor, const char* device, uint32_t driver) {
    return vendor == 0x13B5 && std::string(device) == "Mali-G51" && driver == 0x04800000;
}

// Some older Mali compilers crash on NoPerspective. Reconstruct screen-linear e using FragCoord.w instead.
inline std::string VulkanScreenLinearHlsl(std::string src) {
    const std::string qualifier = "noperspective float4 e : TEXCOORD5;";
    size_t p = src.find(qualifier);
    if (p == std::string::npos) return src;
    src.replace(p, qualifier.size(), "float4 e : TEXCOORD5;");
    p = src.find("    return o;");
    if (p != std::string::npos) src.insert(p, "    o.e *= o.pos.w;\n");
    const std::string entry = "float4 main(VOut i, bool front : SV_IsFrontFace) : SV_Target {\n";
    p = src.find(entry);
    if (p != std::string::npos) src.insert(p + entry.size(), "    i.e *= i.pos.w;\n");
    return src;
}

}  // namespace gpu
