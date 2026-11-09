#pragma once

#include <algorithm>
#include <cstdint>

namespace glint {

// Loading and first-frame setup do not consume the visible transition's duration.
class TransitionClock {
public:
    void reset() { started_ = false; }
    bool started() const { return started_; }
    void submitted(std::int64_t now) {
        if (!started_) {
            origin_ = now;
            started_ = true;
        }
    }
    float opacity(std::int64_t now, int duration_ms) const {
        if (duration_ms <= 0) return 1;
        if (!started_) return 0;
        return std::clamp(
            static_cast<float>(now - origin_) / (static_cast<float>(duration_ms) * 1e6f), 0.0f,
            1.0f);
    }
    void resume(std::int64_t paused_ns) {
        if (started_) origin_ += paused_ns;
    }

private:
    bool started_ = false;
    std::int64_t origin_ = 0;
};

}  // namespace glint
