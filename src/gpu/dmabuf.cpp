#include "gpu/dmabuf.hpp"

#include <unistd.h>

#include "gpu/context.hpp"

namespace glint::gpu {

std::unique_ptr<DmabufImage> DmabufImage::import(Context& context, const DmabufPlane& plane,
                                                 std::string& error) {
    if (!context.dmabuf()) {
        error = "no dma-buf import on this device";
        return nullptr;
    }
    VkDevice device = context.device();
    std::unique_ptr<DmabufImage> self(new DmabufImage(context));

    VkSubresourceLayout layout{
        .offset = plane.offset,
        .size = 0,
        .rowPitch = plane.pitch,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = plane.modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &layout,
    };
    VkExternalMemoryImageCreateInfo external{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = plane.format,
        .extent = {plane.width, plane.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (VkResult r = vkCreateImage(device, &info, nullptr, &self->image_); r != VK_SUCCESS) {
        error = failed("vkCreateImage (dma-buf)", r);
        self->image_ = VK_NULL_HANDLE;
        return nullptr;
    }

    int fd = ::dup(plane.fd);
    if (fd < 0) {
        error = "dup of the dma-buf fd failed";
        return nullptr;
    }
    VkMemoryFdPropertiesKHR fd_props{.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    vkGetMemoryFdPropertiesKHR(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd,
                               &fd_props);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, self->image_, &requirements);
    std::uint32_t types = fd_props.memoryTypeBits & requirements.memoryTypeBits;
    if (types == 0) {
        ::close(fd);
        error = "no memory type for the dma-buf";
        return nullptr;
    }
    std::uint32_t type = 0;
    while ((types & (1u << type)) == 0) ++type;
    // The whole buffer object: the plane may start deep inside it.
    off_t end = ::lseek(fd, 0, SEEK_END);
    ::lseek(fd, 0, SEEK_SET);
    VkDeviceSize size =
        std::max<VkDeviceSize>(requirements.size, end > 0 ? static_cast<VkDeviceSize>(end) : 0);
    VkMemoryDedicatedAllocateInfo dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = self->image_,
    };
    VkImportMemoryFdInfoKHR import{
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = fd,
    };
    VkMemoryAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import,
        .allocationSize = size,
        .memoryTypeIndex = type,
    };
    if (VkResult r = vkAllocateMemory(device, &allocate, nullptr, &self->memory_);
        r != VK_SUCCESS) {
        ::close(fd);  // not taken over on failure
        error = failed("vkAllocateMemory (dma-buf)", r);
        self->memory_ = VK_NULL_HANDLE;
        return nullptr;
    }
    if (VkResult r = vkBindImageMemory(device, self->image_, self->memory_, 0); r != VK_SUCCESS) {
        error = failed("vkBindImageMemory (dma-buf)", r);
        return nullptr;
    }
    VkImageViewCreateInfo view{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = self->image_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = plane.format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCreateImageView(device, &view, nullptr, &self->view_);
    return self;
}

DmabufImage::~DmabufImage() {
    Context& context = context_;
    context.defer([&context, i = image_, m = memory_, v = view_] {
        VkDevice device = context.device();
        if (v != VK_NULL_HANDLE) vkDestroyImageView(device, v, nullptr);
        if (i != VK_NULL_HANDLE) vkDestroyImage(device, i, nullptr);
        if (m != VK_NULL_HANDLE) vkFreeMemory(device, m, nullptr);
    });
}

}  // namespace glint::gpu
