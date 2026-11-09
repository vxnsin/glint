#pragma once

#include <unordered_map>

#include "gpu/pictures.hpp"
#include "gpu/vulkan.hpp"

namespace glint::gpu {

class Context;

// Draws a video frame from its Y, U and V planes (src/shaders/video.frag), placed like a
// picture. One pipeline per target format.
class VideoPass {
public:
    explicit VideoPass(Context& context);
    ~VideoPass();
    VideoPass(const VideoPass&) = delete;
    VideoPass& operator=(const VideoPass&) = delete;

    struct Frame {
        VkImageView y, u, v;  // v unused when interleaved
        std::uint32_t width, height;
        bool bt709 = true;
        bool full_range = false;
        bool interleaved = false;
    };
    void draw(const Target& target, const Frame& frame, Scaling scaling, const float color[3],
              float opacity);

private:
    VkPipeline pipeline(VkFormat format);

    Context& context_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkShaderModule vertex_ = VK_NULL_HANDLE;
    VkShaderModule fragment_ = VK_NULL_HANDLE;
    std::unordered_map<int, VkPipeline> pipelines_;
};

}  // namespace glint::gpu
