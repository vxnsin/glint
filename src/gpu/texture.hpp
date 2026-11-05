#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "gpu/staging.hpp"
#include "gpu/vulkan.hpp"

VK_DEFINE_HANDLE(VmaAllocation)

namespace glint::gpu {

class Context;

// A sampled 2D image with a full mip chain, device-local. Destruction is deferred until the
// GPU is done with every frame submitted before it.
class Texture {
public:
    // One stored mip level inside a staging span.
    struct Level {
        VkDeviceSize offset;  // from the start of the span
        std::uint32_t width, height;
    };

    // Records and submits the upload of pixels in a staging span. With generate set, only
    // levels[0] is used and the rest of the mip chain is blitted on the GPU (not for block
    // compressed formats); otherwise every given level is copied as it is. The texture can be
    // drawn from in any later submit on the same queue.
    static std::shared_ptr<Texture> upload(Context& context, StagingRing& staging,
                                           StagingRing::Span& span, VkFormat format,
                                           std::span<const Level> levels, bool generate);

    // Tightly packed RGBA8 sRGB pixels with a generated mip chain (still pictures).
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
    VkFormat format() const { return format_; }

private:
    explicit Texture(Context& context) : context_(context) {}

    Context& context_;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    std::uint32_t width_ = 0, height_ = 0, levels_ = 1;
    VkDeviceSize memory_ = 0;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
};

}  // namespace glint::gpu
