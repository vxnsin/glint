#include "audio/spectrum.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace glint::audio {

namespace {

float smoothing_factor(float ms, float dt_seconds) {
    if (ms <= 0.f) return 1.f;
    return 1.f - std::exp(-dt_seconds / (ms / 1000.f));
}

}  // namespace

Spectrum::Spectrum(const SpectrumOptions& options)
    : options_(options),
      fft_(options.fft_size),
      window_(options.fft_size),
      ring_left_(options.fft_size, 0.f),
      ring_right_(options.fft_size, 0.f),
      hop_(
          std::max<std::size_t>(1, options.sample_rate / std::max(1u, options.updates_per_second))),
      work_(options.fft_size),
      power_left_(options.fft_size / 2 + 1, 0.f),
      power_right_(options.fft_size / 2 + 1, 0.f) {
    assert(is_power_of_two(options.fft_size) && options.fft_size >= 64);
    assert(options.sample_rate > 0 && options.channels > 0);

    const std::size_t n = options.fft_size;
    for (std::size_t i = 0; i < n; ++i) {
        window_[i] = 0.5f - 0.5f * static_cast<float>(
                                       std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                                static_cast<double>(n)));
    }

    const float dt = static_cast<float>(hop_) / static_cast<float>(options.sample_rate);
    attack_ = smoothing_factor(options.attack_ms, dt);
    release_ = smoothing_factor(options.release_ms, dt);

    // Band edges. Bin k covers (k - 0.5) * bin_hz to (k + 0.5) * bin_hz; a band takes the
    // part of each bin that overlaps it. The square root of that power, times `scale`,
    // reads 1 for a full-scale sine inside the band: with a Hann window its three main
    // bins carry (N/4)^2 * 1.5 of power.
    const float bin_hz = static_cast<float>(options.sample_rate) / static_cast<float>(n);
    const float norm = 4.f / (static_cast<float>(n) * std::sqrt(1.5f));
    const std::size_t last_bin = n / 2;  // Nyquist
    bands_.resize(band_count);
    for (std::size_t b = 0; b < band_count; ++b) {
        Band& band = bands_[b];
        const float lo = band_low_hz(b);
        const float hi = band_high_hz(b);
        band.first_bin = std::min(static_cast<std::size_t>(lo / bin_hz + 0.5f), last_bin);
        for (std::size_t k = band.first_bin; k <= last_bin; ++k) {
            const float bin_lo = (static_cast<float>(k) - 0.5f) * bin_hz;
            const float bin_hi = bin_lo + bin_hz;
            if (bin_lo >= hi) break;
            const float overlap = std::min(hi, bin_hi) - std::max(lo, bin_lo);
            band.weights.push_back(std::clamp(overlap / bin_hz, 0.f, 1.f));
        }
        float scale = norm;
        if (options.tilt != 0.f) {
            scale *= std::pow(2.f, options.tilt * std::log10(band_center_hz(b) / 1000.f));
        }
        band.scale = scale * options.gain;
    }
}

float Spectrum::band_low_hz(std::size_t band) const {
    const float high =
        std::min(options_.high_hz, 0.95f * static_cast<float>(options_.sample_rate) / 2.f);
    const float ratio = std::pow(high / options_.low_hz, 1.f / static_cast<float>(band_count));
    return options_.low_hz * std::pow(ratio, static_cast<float>(band));
}

float Spectrum::band_high_hz(std::size_t band) const {
    return band_low_hz(band + 1);
}

float Spectrum::band_center_hz(std::size_t band) const {
    return std::sqrt(band_low_hz(band) * band_high_hz(band));
}

void Spectrum::reset() {
    std::ranges::fill(ring_left_, 0.f);
    std::ranges::fill(ring_right_, 0.f);
    left_.fill(0.f);
    right_.fill(0.f);
    write_ = 0;
    pending_ = 0;
}

void Spectrum::push_frame(float l, float r) {
    ring_left_[write_] = l;
    ring_right_[write_] = r;
    write_ = (write_ + 1) % options_.fft_size;
}

bool Spectrum::feed(std::span<const float> samples) {
    const std::size_t channels = options_.channels;
    const std::size_t frames = samples.size() / channels;
    bool changed = false;
    for (std::size_t f = 0; f < frames; ++f) {
        const float* frame = samples.data() + f * channels;
        const float l = frame[0];
        const float r = channels > 1 ? frame[1] : l;
        push_frame(l, r);
        if (++pending_ >= hop_) {
            pending_ = 0;
            analyse();
            changed = true;
        }
    }
    return changed;
}

void Spectrum::analyse() {
    const std::size_t n = options_.fft_size;
    // Left in the real part, right in the imaginary part: one FFT for both channels.
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t at = (write_ + i) % n;
        work_[i] = {ring_left_[at] * window_[i], ring_right_[at] * window_[i]};
    }
    fft_.forward(work_);

    // Untangle: a real signal has a Hermitian spectrum, so with z = x + i y,
    // X[k] = (Z[k] + conj(Z[n-k])) / 2 and Y[k] = (Z[k] - conj(Z[n-k])) / (2i).
    power_left_[0] = work_[0].real() * work_[0].real();
    power_right_[0] = work_[0].imag() * work_[0].imag();
    for (std::size_t k = 1; k <= n / 2; ++k) {
        const std::complex<float> a = work_[k];
        const std::complex<float> b = std::conj(work_[n - k]);
        const std::complex<float> x = 0.5f * (a + b);
        const std::complex<float> y = std::complex<float>(0.f, -0.5f) * (a - b);
        power_left_[k] = std::norm(x);
        power_right_[k] = std::norm(y);
    }

    auto band_power = [](const Band& band, const std::vector<float>& power) {
        float sum = 0.f;
        for (std::size_t i = 0; i < band.weights.size(); ++i) {
            sum += band.weights[i] * power[band.first_bin + i];
        }
        return sum;
    };
    auto smooth = [this](float old, float value) {
        return old + (value - old) * (value > old ? attack_ : release_);
    };
    for (std::size_t b = 0; b < band_count; ++b) {
        const Band& band = bands_[b];
        left_[b] = smooth(left_[b], std::sqrt(band_power(band, power_left_)) * band.scale);
        right_[b] = smooth(right_[b], std::sqrt(band_power(band, power_right_)) * band.scale);
    }
}

std::array<float, 32> Spectrum::bands32(const Bands& bands) {
    std::array<float, 32> out{};
    for (std::size_t i = 0; i < 32; ++i) out[i] = 0.5f * (bands[2 * i] + bands[2 * i + 1]);
    return out;
}

std::array<float, 16> Spectrum::bands16(const Bands& bands) {
    std::array<float, 16> out{};
    for (std::size_t i = 0; i < 16; ++i) {
        out[i] = 0.25f * (bands[4 * i] + bands[4 * i + 1] + bands[4 * i + 2] + bands[4 * i + 3]);
    }
    return out;
}

}  // namespace glint::audio
