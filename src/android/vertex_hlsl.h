#pragma once

#include <string>

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

}  // namespace gpu
