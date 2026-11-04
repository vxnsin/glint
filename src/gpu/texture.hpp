#pragma once

#include <cstdint>
#include <memory>

#include "gpu/staging.hpp"
#include "gpu/vulkan.hpp"

VK_DEFINE_HANDLE(VmaAllocation)

namespace glint::gpu {

class Context;

// A sampled 2D image with a full mip chain, device-local. Destruction is deferred until the
// GPU is done with every frame submitted before it.
class Texture {
public:
    // Records and submits the upload of tightly packed RGBA8 pixels (sRGB) from a staging span:
    // copy into mip 0, then each further level blitted from the one above, all on the GPU.
    // The texture can be drawn from in any later submit on the same queue.
    static std::shared_ptr<Texture> upload_rgba(Context& context, StagingRing& staging,
                                                StagingRing::Span& span, std::uint32_t width,
                                                std::uint32_t height);
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    VkImageView view() const { return view_; }
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint32_t levels() const { return levels_; }
    VkDeviceSize memory() const { return memory_; }

private:
    explicit Texture(Context& context) : context_(context) {}

    Context& context_;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    std::uint32_t width_ = 0, height_ = 0, levels_ = 1;
    VkDeviceSize memory_ = 0;
};

}  // namespace glint::gpu
