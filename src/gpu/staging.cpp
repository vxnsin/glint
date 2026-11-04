#include "gpu/staging.hpp"

#include "gpu/context.hpp"

namespace glint::gpu {

namespace {

constexpr VkDeviceSize alignment = 256;

VkDeviceSize align(VkDeviceSize value) {
    return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

StagingRing::StagingRing(Context& context, VkDeviceSize capacity)
    : context_(context), capacity_(capacity) {}

void StagingRing::reclaim() {
    std::uint64_t done = context_.completed();
    while (!entries_.empty() && entries_.front().value != 0 && entries_.front().value <= done) {
        entries_.pop_front();
    }
}

StagingRing::Span StagingRing::allocate(VkDeviceSize size) {
    Span span;
    span.size = size;
    VkDeviceSize needed = align(size);
    {
        std::lock_guard lock(mutex_);
        if (needed <= capacity_) {
            if (!ring_) {
                ring_ =
                    Buffer(context_.allocator(), capacity_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            }
            reclaim();
            std::optional<VkDeviceSize> offset;
            if (entries_.empty()) {
                offset = 0;
            } else {
                VkDeviceSize head = entries_.back().offset + align(entries_.back().size);
                VkDeviceSize tail = entries_.front().offset;
                if (head > tail) {
                    if (head + needed <= capacity_) {
                        offset = head;
                    } else if (needed <= tail) {
                        offset = 0;
                    }
                } else if (head + needed <= tail) {
                    offset = head;
                }
            }
            if (offset && ring_) {
                span.buffer = ring_.get();
                span.offset = *offset;
                span.data = ring_.mapped() + *offset;
                span.id = next_id_++;
                entries_.push_back({span.id, *offset, size, 0});
                return span;
            }
        }
    }
    // Too big for the ring, or the ring is busy with earlier uploads.
    span.dedicated = std::make_shared<Buffer>(context_.allocator(), size,
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (!*span.dedicated) return {};
    span.buffer = span.dedicated->get();
    span.data = span.dedicated->mapped();
    return span;
}

void StagingRing::flush(const Span& span) {
    if (span.dedicated) {
        span.dedicated->flush(0, VK_WHOLE_SIZE);
    } else if (ring_) {
        ring_.flush(span.offset, span.size);
    }
}

void StagingRing::abandon(Span& span) {
    if (span.dedicated) {
        span.dedicated.reset();
        return;
    }
    std::lock_guard lock(mutex_);
    std::erase_if(entries_, [&](const Entry& entry) { return entry.id == span.id; });
}

void StagingRing::retire(Span& span, std::uint64_t value) {
    if (span.dedicated) {
        // Freed once the copy is done.
        context_.defer([buffer = std::move(span.dedicated)]() mutable { buffer.reset(); });
        return;
    }
    std::lock_guard lock(mutex_);
    for (Entry& entry : entries_) {
        if (entry.id == span.id) entry.value = value;
    }
}

}  // namespace glint::gpu
