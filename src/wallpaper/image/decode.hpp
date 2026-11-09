#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
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

// The same for a whole PNG (image_format 13) or JPEG (2) in memory, as embedded in .tex
// files. Worker thread.
std::string decode_memory(std::span<const std::byte> data, int image_format,
                          std::uint32_t max_dimension, gpu::StagingRing& staging, Decoded& out);

// Writes BGRA or RGBA pixels as a PNG file. Returns an error message or empty.
std::string write_png(const std::filesystem::path& path, std::span<const std::byte> pixels,
                      std::uint32_t width, std::uint32_t height, bool bgra);

}  // namespace glint::image
