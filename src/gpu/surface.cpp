#include "gpu/surface.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

#include "core/log.hpp"
#include "gpu/context.hpp"

namespace glint::gpu {

namespace {

bool is_srgb(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

}  // namespace

Surface::Surface(Context& context, wl_display* display, wl_surface* surface) : context_(context) {
    VkWaylandSurfaceCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = display,
        .surface = surface,
    };
    if (VkResult r = vkCreateWaylandSurfaceKHR(context_.instance(), &info, nullptr, &surface_);
        r != VK_SUCCESS) {
        log::error("vkCreateWaylandSurfaceKHR: {}", result_name(r));
        surface_ = VK_NULL_HANDLE;
        return;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(context_.physical(), context_.queue_family(), surface_,
                                         &supported);
    if (!supported) log::warn("the queue cannot present to this surface");

    VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (Slot& slot : slots_) {
        slot.cmd = context_.allocate_command_buffer();
        vkCreateSemaphore(context_.device(), &semaphore_info, nullptr, &slot.acquired);
        slot.uniforms = Buffer(context_.allocator(), sizeof(FrameUniforms),
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    }
    if (context_.timestamp_period() > 0) {
        VkQueryPoolCreateInfo query_info{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = static_cast<std::uint32_t>(slots_.size() * 2),
        };
        vkCreateQueryPool(context_.device(), &query_info, nullptr, &queries_);
        vkResetQueryPool(context_.device(), queries_, 0, query_info.queryCount);
    }
}

Surface::~Surface() {
    // Wait for this surface's frames, and for the presents, which the timeline does not
    // cover: the swapchain and its semaphores must not go while the queue presents.
    for (const Slot& slot : slots_) context_.wait(slot.value);
    VkDevice device = context_.device();
    vkQueueWaitIdle(context_.queue());
    destroy_swapchain(swapchain_);
    for (VkSemaphore s : rendered_) vkDestroySemaphore(device, s, nullptr);
    for (Slot& slot : slots_) {
        context_.free_command_buffer(slot.cmd);
        vkDestroySemaphore(device, slot.acquired, nullptr);
        slot.uniforms.reset();
    }
    if (queries_ != VK_NULL_HANDLE) vkDestroyQueryPool(device, queries_, nullptr);
    if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(context_.instance(), surface_, nullptr);
}

void Surface::destroy_swapchain(VkSwapchainKHR swapchain) {
    if (swapchain == VK_NULL_HANDLE) return;
    for (VkImageView view : views_) vkDestroyImageView(context_.device(), view, nullptr);
    views_.clear();
    vkDestroySwapchainKHR(context_.device(), swapchain, nullptr);
}

bool Surface::resize(std::uint32_t width, std::uint32_t height) {
    if (surface_ == VK_NULL_HANDLE || width == 0 || height == 0) return false;
    if (swapchain_ != VK_NULL_HANDLE && width == extent_.width && height == extent_.height) {
        return true;
    }
    VkPhysicalDevice physical = context_.physical();
    VkDevice device = context_.device();

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface_, &caps);

    if (format_ == VK_FORMAT_UNDEFINED) {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &count, formats.data());
        auto srgb = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& f) {
            return is_srgb(f.format) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        format_ = srgb != formats.end() ? srgb->format : formats.at(0).format;
        encode_srgb_ = !is_srgb(format_);
    }

    // Everything in flight on the old swapchain finishes first, presents included; resizes
    // are rare.
    for (const Slot& slot : slots_) context_.wait(slot.value);
    if (swapchain_ != VK_NULL_HANDLE) vkQueueWaitIdle(context_.queue());

    can_capture_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    extent_ = {std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
               std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height)};
    VkCompositeAlphaFlagBitsKHR alpha =
        (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkSwapchainKHR old = swapchain_;
    VkSwapchainCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface_,
        .minImageCount = caps.minImageCount,
        .imageFormat = format_,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent_,
        .imageArrayLayers = 1,
        .imageUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            (can_capture_ ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT) : 0u),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VkResult r = vkCreateSwapchainKHR(device, &info, nullptr, &swapchain_);
    destroy_swapchain(old);
    if (r != VK_SUCCESS) {
        log::error("vkCreateSwapchainKHR: {}", result_name(r));
        swapchain_ = VK_NULL_HANDLE;
        return false;
    }

    std::uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain_, &count, images_.data());
    for (VkImage image : images_) {
        VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format_,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(device, &view_info, nullptr, &view);
        views_.push_back(view);
    }
    VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    while (rendered_.size() < count) {
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(device, &semaphore_info, nullptr, &s);
        rendered_.push_back(s);
    }
    log::debug("swapchain {}x{}, {} images, format {}", extent_.width, extent_.height, count,
               static_cast<int>(format_));
    return true;
}

bool Surface::begin(const float clear[4], const FrameUniforms& uniforms, Target& target,
                    const std::function<void(VkCommandBuffer, VkExtent2D)>& before) {
    if (swapchain_ == VK_NULL_HANDLE || recording_) return false;
    // After a suboptimal or out-of-date present the swapchain must be made anew first.
    if (extent_.width == 0 || extent_.height == 0) return false;
    VkDevice device = context_.device();
    Slot& slot = slots_[slot_];
    context_.wait(slot.value);

    // The previous use of this slot is finished: its timestamps are readable.
    if (slot.timed) {
        std::uint64_t ticks[2];
        if (vkGetQueryPoolResults(device, queries_, slot_ * 2, 2, sizeof(ticks), ticks,
                                  sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            gpu_ms_ = static_cast<double>(ticks[1] - ticks[0]) * context_.timestamp_period() / 1e6;
        }
        slot.timed = false;
    }

    VkResult r = vkAcquireNextImageKHR(device, swapchain_, UINT64_MAX, slot.acquired,
                                       VK_NULL_HANDLE, &image_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        extent_ = {0, 0};
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        log::error("vkAcquireNextImageKHR: {}", result_name(r));
        return false;
    }

    std::memcpy(slot.uniforms.mapped(), &uniforms, sizeof(uniforms));
    slot.uniforms.flush(0, sizeof(uniforms));

    VkCommandBuffer cmd = slot.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &begin_info);
    if (queries_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, queries_, slot_ * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries_, slot_ * 2);
    }

    if (before) before(cmd, extent_);

    VkImageMemoryBarrier2 to_attachment{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = images_[image_],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_attachment,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);

    VkRenderingAttachmentInfo color{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = views_[image_],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    std::memcpy(color.clearValue.color.float32, clear, sizeof(float) * 4);
    VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, extent_},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height),
                        0, 1};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    target = {
        .cmd = cmd,
        .extent = extent_,
        .format = format_,
        .frame_uniforms = {slot.uniforms.get(), 0, sizeof(FrameUniforms)},
        .encode_srgb = encode_srgb_,
    };
    recording_ = true;
    return true;
}

bool Surface::end() {
    if (!recording_) return false;
    recording_ = false;
    Slot& slot = slots_[slot_];
    VkCommandBuffer cmd = slot.cmd;
    vkCmdEndRendering(cmd);

    // A screenshot: the image goes through a host buffer on its way to the compositor.
    std::shared_ptr<Buffer> readback;
    VkImageLayout before_present = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (capture_ && can_capture_) {
        VkDeviceSize bytes = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
        readback = std::make_shared<Buffer>(context_.allocator(), bytes,
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
        VkImageMemoryBarrier2 to_copy{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image = images_[image_],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkDependencyInfo copy_dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_copy,
        };
        vkCmdPipelineBarrier2(cmd, &copy_dependency);
        VkBufferImageCopy region{
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {extent_.width, extent_.height, 1},
        };
        vkCmdCopyImageToBuffer(cmd, images_[image_], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback->get(), 1, &region);
        before_present = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    VkImageMemoryBarrier2 to_present{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask =
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask = VK_ACCESS_2_NONE,
        .oldLayout = before_present,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .image = images_[image_],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_present,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
    if (queries_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, slot_ * 2 + 1);
        slot.timed = true;
        last_timed_slot_ = slot_;
    }
    vkEndCommandBuffer(cmd);

    Context::Wait wait{slot.acquired, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore rendered = rendered_[image_];
    slot.value = context_.submit(cmd, {&wait, 1}, {&rendered, 1});
    if (readback) {
        context_.wait(slot.value);
        Capture capture;
        capture.width = extent_.width;
        capture.height = extent_.height;
        capture.format = format_;
        readback->invalidate(0, VK_WHOLE_SIZE);
        capture.pixels.assign(readback->mapped(), readback->mapped() + readback->size());
        auto done = std::move(capture_);
        capture_ = {};
        done(std::move(capture));
    }

    VkPresentInfoKHR present{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &rendered,
        .swapchainCount = 1,
        .pSwapchains = &swapchain_,
        .pImageIndices = &image_,
    };
    VkResult r = vkQueuePresentKHR(context_.queue(), &present);
    slot_ = (slot_ + 1) % static_cast<std::uint32_t>(slots_.size());
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        extent_ = {0, 0};  // the next resize() recreates
        return r == VK_SUBOPTIMAL_KHR;
    }
    if (r != VK_SUCCESS) {
        log::error("vkQueuePresentKHR: {}", result_name(r));
        return false;
    }
    return true;
}

double Surface::gpu_ms() {
    Slot& slot = slots_[last_timed_slot_];
    if (slot.timed && context_.completed() >= slot.value) {
        std::uint64_t ticks[2];
        if (vkGetQueryPoolResults(context_.device(), queries_, last_timed_slot_ * 2, 2,
                                  sizeof(ticks), ticks, sizeof(std::uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            gpu_ms_ = static_cast<double>(ticks[1] - ticks[0]) * context_.timestamp_period() / 1e6;
        }
        slot.timed = false;
    }
    return gpu_ms_;
}

}  // namespace glint::gpu
