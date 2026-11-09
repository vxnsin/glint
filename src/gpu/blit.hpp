#pragma once

#include <unordered_map>

#include "gpu/vulkan.hpp"

namespace glint::gpu {

class Context;

// Draws a texture over the whole of the current render target (src/shaders/texture.frag):
// the copy into an image layer and the scene into the swapchain. One pipeline per format.
class Blit {
public:
    explicit Blit(Context& context);
    ~Blit();
    Blit(const Blit&) = delete;
    Blit& operator=(const Blit&) = delete;

    struct Params {
        float width, height;  // target size in pixels
        float u = 1, v = 1;   // the used part of the source
        float opacity = 1;
        bool decode = false;  // the target is sRGB and the source holds display values
        bool blend = false;   // alpha-blend onto the target instead of replacing it
    };
    void draw(VkCommandBuffer cmd, VkFormat format, VkImageView source, VkSampler sampler,
              const Params& params);

    VkSampler linear() const { return linear_; }

private:
    VkPipeline pipeline(VkFormat format, bool blend);

    Context& context_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vertex_ = VK_NULL_HANDLE;
    VkShaderModule fragment_ = VK_NULL_HANDLE;
    VkSampler linear_ = VK_NULL_HANDLE;
    std::unordered_map<int, VkPipeline> pipelines_;
};

}  // namespace glint::gpu
