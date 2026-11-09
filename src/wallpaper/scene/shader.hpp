#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

// Wallpaper Engine shaders to SPIR-V (docs/FORMATS.md, 8). The dialect is GLSL with HLSL
// habits and JSON annotations in comments. glint resolves includes, reads the annotations,
// lets glslang's preprocessor settle the combos, then rewrites the result into Vulkan GLSL:
// loose uniforms into one std140 block (set 0, binding 0), g_TextureN samplers at binding
// N + 1, attributes and varyings at explicit locations. The SPIR-V is cached on disk by a
// hash of the final source.
namespace glint::scene {

class Vfs;

// Where a texture slot's binding starts: binding 0 is the uniform block.
constexpr std::uint32_t texture_binding_base = 1;
constexpr std::uint32_t max_texture_slots = 8;

struct ShaderRequest {
    std::string name;                   // "genericimage4" -> shaders/genericimage4.vert|frag
    std::map<std::string, int> combos;  // from the material and its overrides
    std::uint32_t bound_textures = 0;   // bit N: g_TextureN has a texture
};

struct ShaderUniform {
    std::string name;          // g_Speed
    std::string type;          // float, vec2, ..., mat4
    std::uint32_t count = 1;   // array length
    std::uint32_t offset = 0;  // in the block
    std::uint32_t size = 0;    // bytes, with std140 array strides
    std::string material;      // annotation "material": the key in constantshadervalues
    nlohmann::json fallback;   // annotation "default"
};

struct ShaderSampler {
    std::string name;  // g_Texture1
    int slot = -1;     // N of g_TextureN, -1 for other names
    std::uint32_t binding = 0;
    std::string fallback;  // annotation "default", e.g. "_rt_FullFrameBuffer" or "util/white"
};

struct CompiledShader {
    std::vector<std::uint32_t> vertex;
    std::vector<std::uint32_t> fragment;
    std::vector<ShaderUniform> uniforms;
    std::uint32_t block_size = 0;  // 0: the shader has no uniforms
    std::vector<ShaderSampler> samplers;
    std::map<std::string, std::uint32_t> attributes;  // a_Position -> location
    std::map<std::string, int> combos;                // what it was compiled with
    std::string hash;
    bool cached = false;
    std::string error;  // empty on success

    const ShaderUniform* uniform(std::string_view name) const;
};

class ShaderCompiler {
public:
    // cache: a folder for SPIR-V, created on demand; empty for no cache.
    ShaderCompiler(const Vfs& vfs, std::filesystem::path cache);
    ~ShaderCompiler();

    CompiledShader compile(const ShaderRequest& request);

    // The source handed to glslang last, for error messages and tests.
    const std::string& last_vertex_source() const { return last_vertex_; }
    const std::string& last_fragment_source() const { return last_fragment_; }

private:
    const Vfs& vfs_;
    std::filesystem::path cache_;
    std::string last_vertex_, last_fragment_;
};

}  // namespace glint::scene
