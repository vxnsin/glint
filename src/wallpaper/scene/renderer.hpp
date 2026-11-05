#pragma once

#include <memory>
#include <vector>

#include "core/math.hpp"
#include "gpu/buffer.hpp"
#include "gpu/vulkan.hpp"
#include "wallpaper/scene/assets.hpp"
#include "wallpaper/scene/particles.hpp"
#include "wallpaper/scene/plan.hpp"

VK_DEFINE_HANDLE(VmaAllocation)

namespace glint::gpu {
class Blit;
struct Target;
}  // namespace glint::gpu

namespace glint::scene {

// What changes from frame to frame.
struct FrameInputs {
    double time = 0;                  // seconds since the scene started, pauses excluded
    double delta = 0;                 // seconds since the last frame
    double daytime = 0;               // 0 to 1 over the local day
    float pointer[2] = {0.5f, 0.5f};  // 0 to 1 over the output, y down
    float pointer_last[2] = {0.5f, 0.5f};
    float parallax[2] = {0.5f, 0.5f};   // smoothed pointer for the camera parallax
    const float* audio_left = nullptr;  // 64 bands each, or null for silence
    const float* audio_right = nullptr;
};

// One scene on one output: the render targets sized to it, the flat list of draws, the
// uniform blocks. Built when the size or a property changes, never in the frame loop.
class SceneRenderer {
public:
    SceneRenderer(gpu::Context& gpu, gpu::Blit& blit, std::shared_ptr<SceneAssets> assets);
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // The property values; rebuilds the plan and the uniform blocks with the next frame.
    void set_values(const Values& values);
    const Values& values() const { return values_; }

    // Records every offscreen pass; the scene target is ready to sample afterwards.
    void record(VkCommandBuffer cmd, VkExtent2D extent, const FrameInputs& inputs);
    // Draws the scene target into the swapchain image (inside its rendering).
    void composite(const gpu::Target& target, float opacity);

    std::uint64_t memory() const;
    std::size_t draw_count() const { return draws_.size(); }
    bool uses_audio() const { return uses_audio_; }
    bool uses_pointer() const { return uses_pointer_; }
    const std::vector<std::string>& warnings() const { return plan_.warnings; }

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkExtent2D extent{0, 0};
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkDeviceSize memory = 0;
    };
    struct Dynamic {  // a uniform written every frame
        enum class What {
            time,
            daytime,
            frametime,
            pointer,
            pointer_last,
            parallax,
            mvp,
            mvp_inverse,
            model,
            model_inverse,  // object transforms under parallax
            audio,          // g_AudioSpectrum{16,32,64}{Left,Right}: bands and side below
            timeline,       // a material constant with a timeline: bound and uniform below
            alpha,          // the object's alpha with a timeline
        } what;
        std::uint32_t offset;
        int bands = 0;
        bool right = false;
        const Bound* bound = nullptr;
        const ShaderUniform* uniform = nullptr;
    };
    struct Draw {
        const PlanDraw* plan = nullptr;
        SceneProgram* program = nullptr;
        VkPipeline pipeline = VK_NULL_HANDLE;
        Image* target = nullptr;  // null: the scene target
        std::array<VkImageView, max_texture_slots> views{};
        std::array<VkSampler, max_texture_slots> samplers{};
        std::array<Image*, max_texture_slots> sampled{};  // targets read, for layout changes
        std::vector<std::byte> block;
        std::vector<Dynamic> dynamics;
        VkDeviceSize block_offset = 0;
        std::uint32_t first_vertex = 0;
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        bool copy = false;  // glint's copy pass
        float copy_uv[2] = {1, 1};
        VkImageView copy_view = VK_NULL_HANDLE;
        VkSampler copy_sampler = VK_NULL_HANDLE;
        bool framebuffer = false;  // samples the scene so far
        // A particle draw: its system, where its quads start in the particle vertex buffer,
        // and uniforms only it sets (g_RenderVar0/1, the orientation).
        ParticleSystem* particles = nullptr;
        std::uint32_t particle_offset = 0;
        std::map<std::string, std::vector<float>> extra;
        // A puppet posed by its animation: the mesh, its vertices as built (positions in the
        // rest pose, uvs scaled), and where the skinned copy goes each frame.
        const Mesh* skin = nullptr;
        std::vector<float> skin_base;
        std::uint32_t skin_offset = 0;
    };

    void build(VkExtent2D extent);
    void release();
    Image make_image(VkExtent2D extent, VkFormat format);
    void destroy_image(Image& image);
    void fill_block(Draw& draw);
    math::Mat4 parallax_model(int object, const FrameInputs& inputs) const;
    math::Mat4 particle_model(int particle, const FrameInputs& inputs) const;
    math::Mat4 model_matrix(int object, std::vector<int>& visiting, double time) const;
    // A particle object's transform, with its parents (images or particles).
    math::Mat4 particle_matrix(int particle, std::vector<int>& visiting, double time) const;
    // The transform of whatever object has this id, identity if none.
    math::Mat4 transform_of_id(int id, std::vector<int>& visiting, double time) const;
    void transition(VkCommandBuffer cmd, Image& image, VkImageLayout layout);

    gpu::Context& gpu_;
    gpu::Blit& blit_;
    std::shared_ptr<SceneAssets> assets_;
    Values values_;
    Plan plan_;
    bool stale_ = true;

    VkExtent2D extent_{0, 0};
    float canvas_scale_ = 1;  // output pixels per canvas unit
    math::Mat4 projection_;
    Image scene_;
    Image framebuffer_;
    // The plan's targets, sharing images: a target that is finished with for the frame
    // hands its image to the next one of the same size.
    std::vector<std::unique_ptr<Image>> images_;
    struct TargetUse {
        Image* image = nullptr;
        bool repeat = false;  // sampled wrapping, like the picture it holds
    };
    std::vector<TargetUse> targets_;
    std::vector<Draw> draws_;
    gpu::Buffer vertices_;
    gpu::Buffer indices_;
    gpu::Buffer zeros_;
    gpu::Buffer uniforms_;
    VkDeviceSize uniform_slot_ = 0;  // bytes per frame slot
    std::uint32_t frame_ = 0;
    float clear_[4] = {0, 0, 0, 1};
    bool clear_enabled_ = true;
    std::vector<math::Mat4> models_;  // per object, without parallax, at time 0
    std::vector<math::Mat4> particle_models_;
    // Particle systems, one per particle object (children inside), and the buffers their
    // quads are written to each frame: two slots like the uniforms, u32 indices.
    std::vector<std::unique_ptr<ParticleSystem>> systems_;
    std::array<gpu::Buffer, 2> particle_vertices_;
    gpu::Buffer particle_indices_;
    std::uint32_t particle_capacity_ = 0;  // particles over all systems
    // Skinned puppet vertices, written each frame: a slot per frame in flight.
    std::array<gpu::Buffer, 2> skinned_vertices_;
    std::uint32_t skinned_capacity_ = 0;  // vertices over all posed puppets
    float canvas_left_ = 0, canvas_bottom_ = 0, canvas_width_ = 1, canvas_height_ = 1;
    std::vector<bool> moving_;  // per object: a timeline moves its transform
    bool parallax_ = false;
    bool uses_audio_ = false;
    bool uses_pointer_ = false;
    float parallax_amount_ = 0;
};

}  // namespace glint::scene
