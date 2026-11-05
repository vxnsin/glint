#pragma once

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gpu/vulkan.hpp"

struct wl_display;
VK_DEFINE_HANDLE(VmaAllocator)

namespace glint::gpu {

// The instance, the device that drives the outputs, its queue, the allocator, and the
// timeline everything is ordered on. One per daemon, used from the main thread only.
class Context {
public:
    // Picks the physical device whose DRM node is main_device (what the compositor renders
    // with), else the first discrete GPU that can present to Wayland, else any that can.
    static std::unique_ptr<Context> create(wl_display* display, std::optional<dev_t> main_device,
                                           std::string& error);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    std::uint32_t queue_family() const { return queue_family_; }
    VmaAllocator allocator() const { return allocator_; }
    VkPipelineCache pipeline_cache() const { return pipeline_cache_; }
    const std::string& device_name() const { return device_name_; }
    std::uint32_t max_image_dimension() const { return max_image_dimension_; }
    // dma-buf import with DRM format modifiers (decoded video frames without a copy).
    bool dmabuf() const { return dmabuf_; }

    // Nanoseconds per timestamp tick; 0 if the queue has no timestamps.
    double timestamp_period() const { return timestamp_period_; }

    // Submits one command buffer. Every submit signals the next value on the timeline; the
    // value is returned, and completed() says when the GPU is past it.
    struct Wait {
        VkSemaphore semaphore;
        VkPipelineStageFlags2 stage;
    };
    std::uint64_t submit(VkCommandBuffer cmd, std::span<const Wait> waits,
                         std::span<const VkSemaphore> signals);
    std::uint64_t submitted() const { return submitted_; }
    std::uint64_t completed() const;
    void wait(std::uint64_t value) const;

    // Runs destroy once the GPU has finished everything submitted so far.
    void defer(std::function<void()> destroy);
    // Runs what is due, without waiting. True if something is still pending.
    bool collect();
    // Waits for the GPU and runs everything pending.
    void drain();

    VkCommandBuffer allocate_command_buffer() const;
    void free_command_buffer(VkCommandBuffer cmd) const;

    // Writes the pipeline cache to $XDG_CACHE_HOME/glint if it grew.
    void save_pipeline_cache();

private:
    Context() = default;
    std::string init(wl_display* display, std::optional<dev_t> main_device);
    void load_pipeline_cache();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    std::size_t pipeline_cache_size_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::uint64_t submitted_ = 0;
    std::string device_name_;
    std::uint32_t max_image_dimension_ = 4096;
    double timestamp_period_ = 0;
    bool dmabuf_ = false;

    struct Deferred {
        std::uint64_t value;
        std::function<void()> destroy;
    };
    std::vector<Deferred> deferred_;
};

}  // namespace glint::gpu
