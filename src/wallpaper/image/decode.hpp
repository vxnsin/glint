#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "gpu/staging.hpp"

namespace glint::image {

struct Decoded {
    gpu::StagingRing::Span pixels;  // tightly packed RGBA8, sRGB
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double decode_ms = 0;
};

// Decodes the first frame of a picture with FFmpeg straight into staging memory. Pictures
// larger than max_dimension on a side are scaled down to fit. Worker thread. Returns an error
// message or empty.
std::string decode(const std::filesystem::path& path, std::uint32_t max_dimension,
                   gpu::StagingRing& staging, Decoded& out);

}  // namespace glint::image
