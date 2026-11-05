#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gpu/staging.hpp"
#include "gpu/vulkan.hpp"
#include "wallpaper/scene/mdl.hpp"
#include "wallpaper/scene/plan.hpp"
#include "wallpaper/scene/project.hpp"
#include "wallpaper/scene/scene.hpp"
#include "wallpaper/scene/shader.hpp"
#include "wallpaper/scene/vfs.hpp"

namespace glint {
class Workers;
namespace gpu {
class Context;
class Texture;
}  // namespace gpu
}  // namespace glint

// Everything of a scene that does not depend on the output: the parsed files, the textures on
// the GPU, the compiled shaders and their pipelines. Shared by every output that shows the
// scene; loaded on a worker, finished on the main thread.
namespace glint::scene {

struct SceneTexture {
    std::shared_ptr<gpu::Texture> texture;
    std::uint32_t tex_width = 1, tex_height = 1;  // stored size (padded)
    std::uint32_t img_width = 1, img_height = 1;  // the picture inside
    bool clamp = false;
    bool nearest = false;
    // A sprite sheet: frames in a grid, their size as a share of the stored texture.
    std::uint32_t frames = 0;
    float frame_width = 1, frame_height = 1;
    int format = 0;  // the shaders' FORMAT_ code: 0 RGBA8, 4 DXT5, 6 DXT3, 7 DXT1, 8 RG88, 9 R8

    PlanTextureInfo plan_info() const { return {frames, format}; }
};

// How a pipeline reads its vertices.
enum class VertexLayout {
    image,     // position (3), texcoord (2)
    particle,  // ParticleVertex: the genericparticle layout with THICKFORMAT
};

struct SceneProgram {
    CompiledShader shader;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    std::map<std::string, VkPipeline> pipelines;  // by blending
};

class SceneAssets {
public:
    // Worker thread: reads, compiles, decodes. Returns null with an error.
    static std::shared_ptr<SceneAssets> load(const std::filesystem::path& item,
                                             const std::filesystem::path& assets,
                                             const std::filesystem::path& shader_cache,
                                             gpu::Context& gpu, gpu::StagingRing& staging,
                                             std::string& error);
    // Main thread, once, before anything draws.
    bool finish();
    ~SceneAssets();
    SceneAssets(const SceneAssets&) = delete;
    SceneAssets& operator=(const SceneAssets&) = delete;

    const Project& project() const { return project_; }
    const SceneDesc& scene() const { return scene_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    double load_ms() const { return load_ms_; }
    int compiled() const { return compiled_; }
    int cached() const { return cached_; }

    // White when a file is missing; transparent black for a render target (_rt_...) glint
    // does not make, so a missing layer adds nothing instead of covering the scene.
    const SceneTexture& texture(const std::string& name) const;
    std::optional<Vec2> picture_size(const std::string& name) const;
    SceneProgram* program(const ShaderRequest& request);
    // A puppet's rest mesh by .mdl path, null when it did not read.
    const Mesh* mesh(const std::string& path) const;
    // The pipeline for a program, blending mode and target format; made on first use.
    VkPipeline pipeline(SceneProgram& program, const std::string& blending, VkFormat format,
                        VertexLayout layout = VertexLayout::image);
    VkPipelineLayout layout() const { return layout_; }
    VkDescriptorSetLayout set_layout() const { return set_layout_; }
    VkSampler sampler(bool clamp, bool nearest) const;
    std::uint64_t memory() const;
    bool animated() const { return animated_; }
    const Vfs& vfs() const { return vfs_; }
    // The one wallpaper that plays the scene's sound, so two outputs do not play it twice.
    const void* sound_owner = nullptr;

private:
    SceneAssets(gpu::Context& gpu, gpu::StagingRing& staging, Vfs vfs)
        : gpu_(gpu), staging_(staging), vfs_(std::move(vfs)) {}

    struct PendingTexture {
        std::string name;
        VkFormat format = VK_FORMAT_UNDEFINED;
        gpu::StagingRing::Span span;
        std::vector<std::pair<VkDeviceSize, std::pair<std::uint32_t, std::uint32_t>>> levels;
        bool generate = false;
        SceneTexture info;
    };
    void read_texture(const std::string& name);

    gpu::Context& gpu_;
    gpu::StagingRing& staging_;
    Vfs vfs_;
    Project project_;
    SceneDesc scene_;
    std::vector<std::string> warnings_;
    std::map<std::string, SceneTexture> textures_;
    std::vector<PendingTexture> pending_;
    SceneTexture white_;
    SceneTexture clear_;  // transparent black, for render targets glint does not make
    std::map<std::string, SceneProgram> programs_;
    std::map<std::string, Mesh> meshes_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkSampler samplers_[4] = {};
    double load_ms_ = 0;
    int compiled_ = 0, cached_ = 0;
    bool animated_ = false;
};

// Texture names in materials are relative to materials/ and have no extension.
std::string texture_path(const std::string& name);

}  // namespace glint::scene
