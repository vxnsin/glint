#include "audio/fft.hpp"

#include <cassert>
#include <cmath>
#include <numbers>
#include <utility>

namespace glint::audio {

Fft::Fft(std::size_t size) : size_(size) {
    assert(is_power_of_two(size) && size >= 2);
    twiddles_.resize(size / 2);
    for (std::size_t k = 0; k < size / 2; ++k) {
        const double angle =
            -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(size);
        twiddles_[k] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }
    reverse_.resize(size);
    unsigned bits = 0;
    while ((std::size_t{1} << bits) < size) ++bits;
    for (std::size_t i = 0; i < size; ++i) {
        std::uint32_t r = 0;
        for (unsigned b = 0; b < bits; ++b) {
            if (i & (std::size_t{1} << b)) r |= std::uint32_t{1} << (bits - 1 - b);
        }
        reverse_[i] = r;
    }
}

void Fft::forward(std::span<std::complex<float>> data) const {
    assert(data.size() == size_);
    for (std::size_t i = 0; i < size_; ++i) {
        const std::size_t j = reverse_[i];
        if (i < j) std::swap(data[i], data[j]);
    }
    // Butterflies, stage by stage: pairs, then quads, up to the whole buffer. The twiddle
    // for a stage of length `len` is every (size / len)th entry of the table.
    for (std::size_t len = 2; len <= size_; len <<= 1) {
        const std::size_t half = len / 2;
        const std::size_t step = size_ / len;
        for (std::size_t start = 0; start < size_; start += len) {
            for (std::size_t k = 0; k < half; ++k) {
                const std::complex<float> t = twiddles_[k * step] * data[start + k + half];
                const std::complex<float> u = data[start + k];
                data[start + k] = u + t;
                data[start + k + half] = u - t;
            }
        }
    }
}

}  // namespace glint::audio
