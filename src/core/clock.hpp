#pragma once

#include <time.h>

#include <cstdint>

namespace glint::clock {

// CLOCK_MONOTONIC in nanoseconds, the clock Wayland frame times are on.
inline std::int64_t now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

inline double ms_since(std::int64_t start_ns) {
    return static_cast<double>(now_ns() - start_ns) / 1e6;
}

}  // namespace glint::clock
