#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

#include "gpu/buffer.hpp"

namespace glint::gpu {

class Context;

// Host-visible memory that decoders write into and uploads copy from. One persistently mapped
// ring buffer, allocated on first use; a request that does not fit gets a buffer of its own.
// allocate() may run on any thread, retire() and the copies on the main thread.
class StagingRing {
public:
    struct Span {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        std::byte* data = nullptr;
        std::uint64_t id = 0;               // ring entry, 0 for a dedicated buffer
        std::shared_ptr<Buffer> dedicated;  // set when the ring was too small or full
    };

    StagingRing(Context& context, VkDeviceSize capacity);

    Span allocate(VkDeviceSize size);
    // The span's copy was submitted with this timeline value; it is reused after that.
    void retire(Span& span, std::uint64_t value);
    // Makes the decoder's writes visible to the device; main thread, before the copy.
    void flush(const Span& span);
    // The span was never copied from (the decode failed); any thread.
    void abandon(Span& span);

    VkDeviceSize capacity() const { return capacity_; }

private:
    struct Entry {
        std::uint64_t id;
        VkDeviceSize offset;
        VkDeviceSize size;
        std::uint64_t value;  // 0 while not submitted
    };
    void reclaim();

    Context& context_;
    VkDeviceSize capacity_;
    std::mutex mutex_;
    Buffer ring_;
    std::deque<Entry> entries_;
    std::uint64_t next_id_ = 1;
};

}  // namespace glint::gpu
