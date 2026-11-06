#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gpu/staging.hpp"
#include "gpu/texture.hpp"
#include "gpu/video_pass.hpp"
#include "gpu/vulkan.hpp"
#include "wallpaper/scene/mdl.hpp"
#include "wallpaper/scene/plan.hpp"
#include "wallpaper/scene/project.hpp"
#include "wallpaper/scene/scene.hpp"
#include "wallpaper/scene/shader.hpp"
#include "wallpaper/scene/text.hpp"
#include "wallpaper/scene/vfs.hpp"
#include "wallpaper/video/decoder.hpp"

namespace glint {
class Workers;
namespace gpu {
class Context;
class VideoPass;
}  // namespace gpu
}  // namespace glint

// Everything of a scene that does not depend on the output: the parsed files, the textures on
// the GPU, the compiled shaders and their pipelines. Shared by every output that shows the
// scene; files, shaders and pipelines prepared on a worker, uploads finished on the main thread.
namespace glint::scene {

inline constexpr VkFormat scene_target_format = VK_FORMAT_R8G8B8A8_UNORM;

struct SceneTexture {
    std::shared_ptr<gpu::Texture> texture;
    std::uint32_t tex_width = 1, tex_height = 1;  // stored size (padded)
    std::uint32_t img_width = 1, img_height = 1;  // the picture inside
    bool clamp = false;
    bool nearest = false;
    // A sprite sheet: frames in a grid, their size as a share of the stored texture.
    std::uint32_t frames = 0;
    float frame_width = 1, frame_height = 1;
    int format = 0;   // the shaders' FORMAT_ code: 0 RGBA8, 4 DXT5, 6 DXT3, 7 DXT1, 8 RG88, 9 R8
    float units = 1;  // canvas units per texel: text is rasterised finer than the canvas
    VkImageView external = VK_NULL_HANDLE;  // a video texture's picture, drawn each frame

    VkImageView view() const { return texture ? texture->view() : external; }
    std::uint32_t levels() const { return texture ? texture->levels() : 1; }
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
    // Worker thread: reads, decodes, compiles shaders and prepares graphics pipelines.
    // Does not submit queue work or touch the main thread's pipeline cache.
    // Returns null with an error.
    static std::shared_ptr<SceneAssets> load(const std::filesystem::path& item,
                                             const std::filesystem::path& assets,
                                             const std::filesystem::path& shader_cache,
                                             gpu::Context& gpu, gpu::StagingRing& staging,
                                             std::string& error);
    // Main thread, once, before anything draws.
    bool finish();
    // Main thread: uploads at most one texture/video per call. True once finalized.
    // Return to the event loop between calls so existing wallpapers keep drawing.
    bool finish_step();
    ~SceneAssets();
    SceneAssets(const SceneAssets&) = delete;
    SceneAssets& operator=(const SceneAssets&) = delete;

    const Project& project() const { return project_; }
    const SceneDesc& scene() const { return scene_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    double load_ms() const { return load_ms_; }
    double pipeline_ms() const { return pipeline_ms_; }
    double finish_ms() const { return finish_ms_; }
    double max_finish_step_ms() const { return max_finish_step_ms_; }
    int prepared_pipelines() const { return prepared_pipelines_; }
    int compiled() const { return compiled_; }
    int cached() const { return cached_; }

    // White when a file is missing; transparent black for a render target (_rt_...) glint
    // does not make, so a missing layer adds nothing instead of covering the scene.
    const SceneTexture& texture(const std::string& name) const;
    // Video textures (an mp4 inside a .tex): the frame due at the scene time is decoded,
    // uploaded and converted into each one's picture. Recorded into cmd before the draws.
    void advance_videos(VkCommandBuffer cmd, double time);
    bool has_videos() const { return !videos_.empty(); }
    // A texture or shader a layer made at run time needs: read, compiled and uploaded now,
    // on the main thread. False when it cannot be had.
    bool ensure_texture(const std::string& name);
    SceneProgram* compile_program(const ShaderRequest& request);
    // A text object set anew (a script changed its string): the texture, uploaded now, and
    // the block's centre from the origin. False when nothing could be drawn.
    bool make_text(const TextObject& text, const std::string& value, SceneTexture& out,
                   Vec2& center);
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
    // Rasterises a text object into the pending texture `_text_<id>`.
    void read_text(const TextObject& text, const Values& values);
    bool rasterize_text(const TextObject& text, const std::string& value, PendingTexture& out,
                        Vec2& center);
    TextRenderer text_;

    gpu::Context& gpu_;
    gpu::StagingRing& staging_;
    Vfs vfs_;
    Project project_;
    SceneDesc scene_;
    std::vector<std::string> warnings_;
    std::map<std::string, SceneTexture> textures_;
    std::vector<PendingTexture> pending_;
    std::filesystem::path shader_cache_;
    // A video texture: its decoder on its own thread, the YUV planes the frames go into and
    // the RGBA picture the scene samples.
    struct VideoTexture {
        std::string name;
        std::unique_ptr<video::Decoder> decoder;
        struct Plane {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            std::uint32_t width = 0, height = 0;
            bool sampled = false;
        };
        std::array<Plane, 3> planes{};
        std::array<gpu::Buffer, 2> staging;
        std::uint32_t slot = 0;
        VkImage image = VK_NULL_HANDLE;  // the picture
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkDeviceSize memory = 0;
        std::uint32_t width = 0, height = 0;
        double shown = -1;
        bool complained = false;
    };
    struct PendingVideo {
        std::string name;
        std::filesystem::path file;
        std::uint32_t width = 0, height = 0;
        std::unique_ptr<video::Decoder> decoder;  // opened on the worker, first frame ready
    };
    std::vector<PendingVideo> pending_videos_;
    std::vector<std::unique_ptr<VideoTexture>> videos_;
    std::unique_ptr<gpu::VideoPass> video_pass_;
    void start_video(PendingVideo& pending);
    void release_videos();
    std::set<std::string> missing_;  // textures asked for at run time that were not there
    bool upload_pending(bool at_most_one = false);
    // Worker only, before publication. Uses a private cache, never the rendering cache.
    void prepare_pipelines(const Plan& plan);
    SceneTexture white_;
    SceneTexture clear_;  // transparent black, for render targets glint does not make
    std::map<std::string, SceneProgram> programs_;
    std::map<std::string, Mesh> meshes_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache preparation_cache_ = VK_NULL_HANDLE;
    VkSampler samplers_[4] = {};
    double load_ms_ = 0;
    double pipeline_ms_ = 0;
    double finish_ms_ = 0;
    double max_finish_step_ms_ = 0;
    int prepared_pipelines_ = 0;
    int compiled_ = 0, cached_ = 0;
    bool animated_ = false;
};

// Texture names in materials are relative to materials/ and have no extension.
std::string texture_path(const std::string& name);

}  // namespace glint::scene
