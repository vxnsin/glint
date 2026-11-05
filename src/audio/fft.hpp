#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace glint::audio {

// A radix-2 FFT for one size, forward only, no scaling. Twiddles and the bit-reversal
// order are computed once in the constructor, so a transform allocates nothing. This is
// all the audio spectrum needs; FFTW would be a dependency for a few dozen lines.
class Fft {
public:
    // size must be a power of two and at least 2.
    explicit Fft(std::size_t size);

    std::size_t size() const { return size_; }

    // In place. data.size() must equal size().
    void forward(std::span<std::complex<float>> data) const;

private:
    std::size_t size_;
    std::vector<std::complex<float>> twiddles_;  // e^(-2 pi i k / n) for k < n/2
    std::vector<std::uint32_t> reverse_;         // bit-reversed index of every position
};

constexpr bool is_power_of_two(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

}  // namespace glint::audio
