#include "gpu/buffer.hpp"

#include <vk_mem_alloc.h>

#include <utility>

namespace glint::gpu {

Buffer::Buffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, bool host,
               bool readback)
    : allocator_(allocator), size_(size) {
    VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo alloc{
        .flags = readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT
                 : host   ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT
                          : 0u,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VmaAllocationInfo result{};
    if (vmaCreateBuffer(allocator_, &info, &alloc, &buffer_, &allocation_, &result) != VK_SUCCESS) {
        buffer_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
        size_ = 0;
        return;
    }
    mapped_ = static_cast<std::byte*>(result.pMappedData);
}

Buffer::Buffer(Buffer&& other) noexcept
    : allocator_(other.allocator_),
      buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      size_(std::exchange(other.size_, 0)),
      mapped_(std::exchange(other.mapped_, nullptr)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        reset();
        allocator_ = other.allocator_;
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
        mapped_ = std::exchange(other.mapped_, nullptr);
    }
    return *this;
}

void Buffer::reset() {
    if (buffer_ != VK_NULL_HANDLE) vmaDestroyBuffer(allocator_, buffer_, allocation_);
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    size_ = 0;
    mapped_ = nullptr;
}

void Buffer::flush(VkDeviceSize offset, VkDeviceSize size) const {
    if (allocation_ != VK_NULL_HANDLE) vmaFlushAllocation(allocator_, allocation_, offset, size);
}

void Buffer::invalidate(VkDeviceSize offset, VkDeviceSize size) const {
    if (allocation_ != VK_NULL_HANDLE) {
        vmaInvalidateAllocation(allocator_, allocation_, offset, size);
    }
}

}  // namespace glint::gpu
