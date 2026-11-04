#include "gpu/texture.hpp"

#include <vk_mem_alloc.h>

#include <bit>

#include "core/log.hpp"
#include "gpu/context.hpp"

namespace glint::gpu {

namespace {

constexpr VkFormat texture_format = VK_FORMAT_R8G8B8A8_SRGB;

void barrier(VkCommandBuffer cmd, VkImage image, std::uint32_t level, std::uint32_t count,
             VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
             VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout from,
             VkImageLayout to) {
    VkImageMemoryBarrier2 b{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1},
    };
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace

std::shared_ptr<Texture> Texture::upload_rgba(Context& context, StagingRing& staging,
                                              StagingRing::Span& span, std::uint32_t width,
                                              std::uint32_t height) {
    std::shared_ptr<Texture> texture(new Texture(context));
    texture->width_ = width;
    texture->height_ = height;
    texture->levels_ = static_cast<std::uint32_t>(std::bit_width(std::max(width, height)));

    VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = texture_format,
        .extent = {width, height, 1},
        .mipLevels = texture->levels_,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo result{};
    if (VkResult r = vmaCreateImage(context.allocator(), &image_info, &alloc, &texture->image_,
                                    &texture->allocation_, &result);
        r != VK_SUCCESS) {
        log::error("vmaCreateImage {}x{}: {}", width, height, result_name(r));
        texture->image_ = VK_NULL_HANDLE;
        staging.abandon(span);
        return nullptr;
    }
    texture->memory_ = result.size;

    VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = texture_format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture->levels_, 0, 1},
    };
    vkCreateImageView(context.device(), &view_info, nullptr, &texture->view_);

    staging.flush(span);
    VkCommandBuffer cmd = context.allocate_command_buffer();
    VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &begin);
    VkImage image = texture->image_;
    std::uint32_t levels = texture->levels_;

    barrier(cmd, image, 0, levels, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{
        .bufferOffset = span.offset,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {width, height, 1},
    };
    vkCmdCopyBufferToImage(cmd, span.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    auto w = static_cast<std::int32_t>(width);
    auto h = static_cast<std::int32_t>(height);
    for (std::uint32_t level = 1; level < levels; ++level) {
        barrier(cmd, image, level - 1, 1,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        std::int32_t nw = std::max(w / 2, 1);
        std::int32_t nh = std::max(h / 2, 1);
        VkImageBlit blit{
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1},
            .srcOffsets = {{0, 0, 0}, {w, h, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
            .dstOffsets = {{0, 0, 0}, {nw, nh, 1}},
        };
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        w = nw;
        h = nh;
    }
    // All levels but the last are in TRANSFER_SRC now, the last in TRANSFER_DST.
    if (levels > 1) {
        barrier(cmd, image, 0, levels - 1, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    barrier(cmd, image, levels - 1, 1, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkEndCommandBuffer(cmd);

    std::uint64_t value = context.submit(cmd, {}, {});
    staging.retire(span, value);
    context.defer([&context, cmd] { context.free_command_buffer(cmd); });
    return texture;
}

Texture::~Texture() {
    if (image_ == VK_NULL_HANDLE) return;
    Context& context = context_;
    context.defer([&context, image = image_, allocation = allocation_, view = view_] {
        vkDestroyImageView(context.device(), view, nullptr);
        vmaDestroyImage(context.allocator(), image, allocation);
    });
}

}  // namespace glint::gpu
