#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <span>
#include <vector>

#include "audio/fft.hpp"

// The audio spectrum for scenes: 64 bands per channel, as Wallpaper Engine hands them to
// shaders (g_AudioSpectrum64Left/Right) and to web wallpapers (the 128-float array, left
// then right, bass first). Values run from 0 (silence) to about 1 (a full-scale tone) and
// may go above 1 for loud material; scenes clamp what they need.
//
// What Wallpaper Engine computes exactly is not documented, so this is glint's own choice:
//
// - Bands are spaced logarithmically from `low_hz` to `high_hz`, each band holding the
//   power of the FFT bins inside it; a bin that straddles a band edge is split by how much
//   of it lies on each side. Pink noise, which music is close to, then reads about flat
//   across the bands, whether a band holds fifty bins or a fifth of one.
// - A Hann window of `fft_size` samples, analysed every `updates_per_second`th of a
//   second with overlap, both channels in one complex FFT.
// - Each band rises with `attack_ms` and falls with `release_ms`, so the values do not
//   flicker at the analysis rate.
//
// Whether a scene tuned against Wallpaper Engine looks the same here has to be checked
// against the real thing; `gain` and `tilt` are the knobs for that.
namespace glint::audio {

inline constexpr std::size_t band_count = 64;
using Bands = std::array<float, band_count>;

struct SpectrumOptions {
    unsigned sample_rate = 48000;
    unsigned channels = 2;  // interleaved; 1 copies the channel to both sides, more than
                            // 2 uses the first two
    unsigned fft_size = 4096;  // a power of two; 4096 at 48 kHz is 11.7 Hz per bin
    unsigned updates_per_second = 30;
    float low_hz = 25.f;
    float high_hz = 20000.f;  // capped below Nyquist
    float gain = 1.f;         // multiplies the result
    float tilt = 0.f;         // extra emphasis of high bands: 1 doubles the amplitude per
                              // decade above 1 kHz and halves it below, 0 leaves pink noise flat
    float attack_ms = 10.f;
    float release_ms = 150.f;
};

class Spectrum {
public:
    explicit Spectrum(const SpectrumOptions& options);

    const SpectrumOptions& options() const { return options_; }

    // Interleaved float samples, any count. Returns true when the bands changed, which
    // happens once per hop of sample_rate / updates_per_second frames.
    bool feed(std::span<const float> samples);

    const Bands& left() const { return left_; }
    const Bands& right() const { return right_; }

    // The coarser sets scenes also read, g_AudioSpectrum32* and g_AudioSpectrum16*: the
    // mean of two and four neighbouring bands.
    static std::array<float, 32> bands32(const Bands& bands);
    static std::array<float, 16> bands16(const Bands& bands);

    // Back to silence, with the input buffer cleared.
    void reset();

    float band_low_hz(std::size_t band) const;
    float band_high_hz(std::size_t band) const;
    float band_center_hz(std::size_t band) const;

private:
    struct Band {
        std::size_t first_bin;       // the first bin the band overlaps
        std::vector<float> weights;  // the overlap of each bin from first_bin on, 0 to 1
        float scale;                 // normalisation, tilt and gain in one factor
    };

    void analyse();
    void push_frame(float l, float r);

    SpectrumOptions options_;
    Fft fft_;
    std::vector<float> window_;
    std::vector<Band> bands_;
    std::vector<float> ring_left_, ring_right_;
    std::size_t write_ = 0;
    std::size_t pending_ = 0;
    std::size_t hop_;
    float attack_, release_;  // per-update smoothing factors
    std::vector<std::complex<float>> work_;
    std::vector<float> power_left_, power_right_;
    Bands left_{}, right_{};
};

}  // namespace glint::audio
