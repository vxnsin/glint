#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "gpu/vulkan.hpp"

namespace glint::gpu {

class Context;

// One plane of a dma-buf (a decoded video frame from VA-API) imported as a sampled Vulkan image,
// with the DRM format modifier and layout the exporter gave. The fd is duplicated; the
// original stays with its owner.
struct DmabufPlane {
    int fd = -1;
    std::uint64_t modifier = 0;
    std::uint64_t offset = 0;
    std::uint64_t pitch = 0;
    std::uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

class DmabufImage {
public:
    static std::unique_ptr<DmabufImage> import(Context& context, const DmabufPlane& plane,
                                               std::string& error);
    ~DmabufImage();
    DmabufImage(const DmabufImage&) = delete;
    DmabufImage& operator=(const DmabufImage&) = delete;

    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkDeviceSize size() const { return size_; }  // of the buffer object it lies in

private:
    explicit DmabufImage(Context& context) : context_(context) {}

    Context& context_;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
};

}  // namespace glint::gpu
