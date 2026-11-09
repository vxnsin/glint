#pragma once

#include <cstdint>
#include <unordered_map>

#include "gpu/surface.hpp"
#include "gpu/vulkan.hpp"

namespace glint::gpu {

class Context;
class Texture;

enum class Scaling { fill, fit, center, tile, stretch };

// The pass that draws a still picture: one fullscreen triangle, the picture placed by its
// scaling mode, background colour around it, blended at an opacity. One pipeline per target
// format, built on first use from the pipeline cache.
class Pictures {
public:
    explicit Pictures(Context& context);
    ~Pictures();
    Pictures(const Pictures&) = delete;
    Pictures& operator=(const Pictures&) = delete;

    // color is linear RGB.
    void draw(const Target& target, const Texture& texture, Scaling scaling, const float color[3],
              float opacity);

private:
    VkPipeline pipeline(VkFormat format);

    Context& context_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkSampler clamp_ = VK_NULL_HANDLE;
    VkSampler repeat_ = VK_NULL_HANDLE;
    VkShaderModule vertex_ = VK_NULL_HANDLE;
    VkShaderModule fragment_ = VK_NULL_HANDLE;
    std::unordered_map<int, VkPipeline> pipelines_;
};

}  // namespace glint::gpu
