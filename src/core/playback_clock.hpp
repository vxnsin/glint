#pragma once

#include <cstdint>

namespace glint {

class PlaybackClock {
public:
    void set_running(bool running, std::int64_t now) {
        if (running == running_) return;
        if (running_) elapsed_ += now - since_;
        since_ = now;
        running_ = running;
    }
    double seconds(std::int64_t now) const {
        return static_cast<double>(elapsed_ + (running_ ? now - since_ : 0)) / 1e9;
    }

private:
    bool running_ = false;
    std::int64_t elapsed_ = 0;
    std::int64_t since_ = 0;
};

}  // namespace glint
