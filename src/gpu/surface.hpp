#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gpu/buffer.hpp"
#include "gpu/vulkan.hpp"

struct wl_display;
struct wl_surface;

namespace glint::gpu {

class Context;

// The per-frame uniform block, set 0 binding 0 in every pipeline. Written once per frame.
struct FrameUniforms {
    float resolution[2];  // target size in pixels
    float time;           // seconds since the wallpaper started
    float delta;          // seconds since the last frame
    float pointer[2];     // pointer position in pixels
    float pad[2];
};

// What a wallpaper gets to draw with: a command buffer inside vkCmdBeginRendering on the
// output's current swapchain image, already cleared to the background colour.
struct Target {
    VkCommandBuffer cmd;
    VkExtent2D extent;
    VkFormat format;
    VkDescriptorBufferInfo frame_uniforms;
    // Set when the swapchain is not sRGB, so shaders must encode themselves.
    bool encode_srgb;
};

// One output's swapchain (FIFO) and the resources to record and present frames into it.
// Two frames in flight; each slot has its command buffer, acquire semaphore, uniform buffer
// and a pair of timestamp queries.
class Surface {
public:
    Surface(Context& context, wl_display* display, wl_surface* surface);
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    bool valid() const { return surface_ != VK_NULL_HANDLE; }

    // (Re)creates the swapchain for a pixel size. Cheap when the size is unchanged.
    bool resize(std::uint32_t width, std::uint32_t height);

    // begin() acquires an image and starts rendering with a clear to colour; nullopt-like
    // false when the swapchain is out of date (call resize again) or not ready.
    // before records offscreen work into the frame's command buffer ahead of the swapchain
    // pass (scene layers); it gets the output's pixel size.
    bool begin(const float clear[4], const FrameUniforms& uniforms, Target& target,
               const std::function<void(VkCommandBuffer, VkExtent2D)>& before = {});
    // Ends rendering, submits and presents.
    bool end();

    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    std::uint32_t image_count() const { return static_cast<std::uint32_t>(images_.size()); }

    // Hands the next presented frame back as pixels (glint screenshot). The frame waits for
    // the GPU once, so it costs a millisecond or two more than usual.
    struct Capture {
        std::vector<std::byte> pixels;  // tightly packed, 4 bytes per pixel
        std::uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    void capture_next(std::function<void(Capture)> done) { capture_ = std::move(done); }
    bool capture_pending() const { return static_cast<bool>(capture_); }
    // Whether the swapchain can be read back at all (TRANSFER_SRC).
    bool can_capture() const { return can_capture_; }
    // Answers a pending capture with no pixels: the output went away or stopped drawing.
    void cancel_capture() {
        if (!capture_) return;
        auto done = std::move(capture_);
        capture_ = {};
        done(Capture{});
    }

    // GPU time of the latest finished frame in milliseconds, -1 if not known yet.
    double gpu_ms();

private:
    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        std::uint64_t value = 0;  // timeline value of its last submit
        Buffer uniforms;
        bool timed = false;
    };

    void destroy_swapchain(VkSwapchainKHR swapchain);

    Context& context_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    bool encode_srgb_ = false;
    VkExtent2D extent_{0, 0};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> rendered_;  // per swapchain image, waited by present
    std::array<Slot, 2> slots_;
    std::uint32_t slot_ = 0;
    std::uint32_t image_ = 0;
    bool recording_ = false;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    std::function<void(Capture)> capture_;
    bool can_capture_ = false;
    double gpu_ms_ = -1;
    std::uint32_t last_timed_slot_ = 0;
};

}  // namespace glint::gpu
