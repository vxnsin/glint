#pragma once

#include <cstddef>

#include "gpu/vulkan.hpp"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace glint::gpu {

// A VMA buffer, host-visible and persistently mapped when created with host = true.
class Buffer {
public:
    Buffer() = default;
    // host: mapped for writing from the CPU; readback: mapped for reading (random access).
    Buffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, bool host,
           bool readback = false);
    ~Buffer() { reset(); }
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    void reset();
    explicit operator bool() const { return buffer_ != VK_NULL_HANDLE; }

    VkBuffer get() const { return buffer_; }
    VkDeviceSize size() const { return size_; }
    std::byte* mapped() const { return mapped_; }
    // Makes host writes visible to the device (no-op on coherent memory).
    void flush(VkDeviceSize offset, VkDeviceSize size) const;
    // Before the CPU reads what the GPU wrote (a readback buffer).
    void invalidate(VkDeviceSize offset, VkDeviceSize size) const;

private:
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    std::byte* mapped_ = nullptr;
};

}  // namespace glint::gpu
