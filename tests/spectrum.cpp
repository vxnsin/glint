// Tests for the FFT and the 64-band audio spectrum, on synthetic signals only.

#include "audio/spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <vector>

#include "audio/fft.hpp"

namespace {

using namespace glint::audio;

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++checks;                                                                         \
        if (!(cond)) {                                                                    \
            ++failures;                                                                   \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (0)

// A deterministic generator, so a failure is the same failure every run.
struct Random {
    std::uint32_t state = 0x9e3779b9u;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);  // [0, 1)
    }
};

std::vector<std::complex<float>> naive_dft(const std::vector<std::complex<float>>& x) {
    const std::size_t n = x.size();
    std::vector<std::complex<float>> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        std::complex<double> sum;
        for (std::size_t t = 0; t < n; ++t) {
            const double angle =
                -2.0 * std::numbers::pi * static_cast<double>(k * t) / static_cast<double>(n);
            sum +=
                std::complex<double>(x[t]) * std::complex<double>(std::cos(angle), std::sin(angle));
        }
        out[k] = std::complex<float>(sum);
    }
    return out;
}

void test_fft_against_dft() {
    Random random;
    for (std::size_t n : {2u, 8u, 64u, 256u}) {
        std::vector<std::complex<float>> x(n);
        for (auto& v : x) v = {random.next() * 2.f - 1.f, random.next() * 2.f - 1.f};
        auto expected = naive_dft(x);
        Fft fft(n);
        fft.forward(x);
        float worst = 0.f;
        for (std::size_t k = 0; k < n; ++k) worst = std::max(worst, std::abs(x[k] - expected[k]));
        CHECK(worst < 1e-3f);
    }
}

void test_fft_impulse_and_tone() {
    Fft fft(16);
    std::vector<std::complex<float>> x(16);
    x[0] = 1.f;
    fft.forward(x);
    for (const auto& v : x) CHECK(std::abs(v - std::complex<float>(1.f, 0.f)) < 1e-5f);

    // A complex tone at bin 3 lands in bin 3 only, with magnitude n.
    for (std::size_t t = 0; t < 16; ++t) {
        const double angle = 2.0 * std::numbers::pi * 3.0 * static_cast<double>(t) / 16.0;
        x[t] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }
    fft.forward(x);
    for (std::size_t k = 0; k < 16; ++k) {
        const float expected = k == 3 ? 16.f : 0.f;
        CHECK(std::abs(std::abs(x[k]) - expected) < 1e-3f);
    }
}

// Options that make the result the plain spectrum of the last window: no smoothing, no
// tilt, no gain.
SpectrumOptions plain_options() {
    SpectrumOptions o;
    o.attack_ms = 0.f;
    o.release_ms = 0.f;
    return o;
}

// Feeds `seconds` of a sine at `hz` with the given amplitudes per channel.
void feed_sine(Spectrum& spectrum, float hz, float left, float right, float seconds) {
    const auto& o = spectrum.options();
    const std::size_t frames =
        static_cast<std::size_t>(seconds * static_cast<float>(o.sample_rate));
    std::vector<float> buffer(frames * o.channels);
    for (std::size_t f = 0; f < frames; ++f) {
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(hz) *
                             static_cast<double>(f) / static_cast<double>(o.sample_rate);
        const float s = static_cast<float>(std::sin(phase));
        buffer[f * o.channels] = left * s;
        if (o.channels > 1) buffer[f * o.channels + 1] = right * s;
    }
    spectrum.feed(buffer);
}

std::size_t band_for(const Spectrum& spectrum, float hz) {
    for (std::size_t b = 0; b < band_count; ++b) {
        if (hz >= spectrum.band_low_hz(b) && hz < spectrum.band_high_hz(b)) return b;
    }
    return band_count;
}

std::size_t argmax(const Bands& bands) {
    return static_cast<std::size_t>(std::ranges::max_element(bands) - bands.begin());
}

void test_band_layout() {
    Spectrum spectrum(plain_options());
    CHECK(std::abs(spectrum.band_low_hz(0) - 25.f) < 1e-3f);
    CHECK(spectrum.band_high_hz(band_count - 1) <= 20000.f);
    for (std::size_t b = 1; b < band_count; ++b) {
        CHECK(spectrum.band_low_hz(b) > spectrum.band_low_hz(b - 1));
        CHECK(std::abs(spectrum.band_low_hz(b) - spectrum.band_high_hz(b - 1)) < 1e-2f);
    }
    // Logarithmic: every band is the same ratio wider than the one before.
    const float r0 = spectrum.band_high_hz(0) / spectrum.band_low_hz(0);
    const float r40 = spectrum.band_high_hz(40) / spectrum.band_low_hz(40);
    CHECK(std::abs(r0 - r40) < 1e-3f);
}

void test_silence_and_update_rate() {
    Spectrum spectrum(plain_options());
    std::vector<float> silence(48000 * 2, 0.f);
    // One second in odd-sized pieces still gives 30 updates.
    int updates = 0;
    std::size_t at = 0;
    while (at < silence.size()) {
        const std::size_t piece = std::min<std::size_t>(2 * 1234, silence.size() - at);
        if (spectrum.feed(std::span(silence).subspan(at, piece))) ++updates;
        at += piece;
    }
    CHECK(updates >= 25 && updates <= 30);
    for (float v : spectrum.left()) CHECK(v == 0.f);
    for (float v : spectrum.right()) CHECK(v == 0.f);
    CHECK(!spectrum.feed(std::span(silence).subspan(0, 2)));  // less than a hop
}

void test_sines() {
    // Full-scale sines read about 1 in their band and nothing elsewhere. The bass bands are
    // narrower than a bin, so a tone there spreads over its neighbours and reads lower.
    struct Case {
        float hz;
        float min, max;
    };
    const Case cases[] = {{50.f, 0.4f, 1.3f},   {100.f, 0.5f, 1.3f},  {440.f, 0.8f, 1.2f},
                          {1000.f, 0.8f, 1.2f}, {3000.f, 0.8f, 1.2f}, {8000.f, 0.8f, 1.2f},
                          {15000.f, 0.8f, 1.2f}};
    for (const Case& c : cases) {
        Spectrum spectrum(plain_options());
        feed_sine(spectrum, c.hz, 1.f, 0.f, 0.5f);
        const std::size_t band = band_for(spectrum, c.hz);
        CHECK(band < band_count);
        const float value = spectrum.left()[band];
        if (!(value >= c.min && value <= c.max)) {
            std::fprintf(stderr, "%g Hz: band %zu reads %g\n", static_cast<double>(c.hz), band,
                         static_cast<double>(value));
        }
        CHECK(value >= c.min && value <= c.max);
        const std::size_t peak = argmax(spectrum.left());
        CHECK(peak + 1 >= band && peak <= band + 1);
        // Nothing on the right channel; nothing on the left outside the window's main
        // lobe (two bins either side) and three bands away.
        for (float v : spectrum.right()) CHECK(v < 0.02f);
        const float bin_hz = 48000.f / 4096.f;
        for (std::size_t b = 0; b < band_count; ++b) {
            const bool far_in_bins = std::abs(spectrum.band_center_hz(b) - c.hz) > 3.f * bin_hz;
            const bool far_in_bands = b + 3 <= band || b >= band + 3;
            if (far_in_bins && far_in_bands) CHECK(spectrum.left()[b] < 0.1f);
        }
    }

    // Half amplitude on the right only.
    Spectrum spectrum(plain_options());
    feed_sine(spectrum, 1000.f, 0.f, 0.5f, 0.5f);
    const std::size_t band = band_for(spectrum, 1000.f);
    CHECK(spectrum.right()[band] > 0.4f && spectrum.right()[band] < 0.6f);
    CHECK(spectrum.left()[band] < 0.02f);
}

void test_mono_and_gain() {
    SpectrumOptions o = plain_options();
    o.channels = 1;
    o.gain = 2.f;
    Spectrum spectrum(o);
    feed_sine(spectrum, 1000.f, 1.f, 0.f, 0.5f);
    const std::size_t band = band_for(spectrum, 1000.f);
    CHECK(spectrum.left()[band] > 1.6f && spectrum.left()[band] < 2.4f);
    CHECK(spectrum.left() == spectrum.right());
}

// Pink noise from white noise with Paul Kellet's filter: constant power per octave, so
// every logarithmic band holds the same power. One window of noise is random (the amplitude
// in a band swings by a factor of two either way), so the bands are averaged in power over
// ten seconds.
Bands pink_average(const SpectrumOptions& o, float seconds) {
    Spectrum spectrum(o);
    Random random;
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    const std::size_t hop = o.sample_rate / o.updates_per_second;
    std::vector<float> buffer(hop * 2);
    std::array<double, band_count> sum{};
    int updates = 0;
    const int hops = static_cast<int>(seconds * static_cast<float>(o.updates_per_second));
    for (int h = 0; h < hops; ++h) {
        for (std::size_t f = 0; f < hop; ++f) {
            const float white = random.next() * 2.f - 1.f;
            b0 = 0.99886f * b0 + white * 0.0555179f;
            b1 = 0.99332f * b1 + white * 0.0750759f;
            b2 = 0.96900f * b2 + white * 0.1538520f;
            b3 = 0.86650f * b3 + white * 0.3104856f;
            b4 = 0.55000f * b4 + white * 0.5329522f;
            b5 = -0.7616f * b5 - white * 0.0168980f;
            const float pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f) * 0.11f;
            b6 = white * 0.115926f;
            buffer[f * 2] = pink;
            buffer[f * 2 + 1] = pink;
        }
        if (!spectrum.feed(buffer)) continue;
        ++updates;
        for (std::size_t b = 0; b < band_count; ++b) {
            sum[b] += static_cast<double>(spectrum.left()[b]) * spectrum.left()[b];
        }
    }
    Bands average{};
    for (std::size_t b = 0; b < band_count; ++b) {
        average[b] = static_cast<float>(std::sqrt(sum[b] / updates));
    }
    return average;
}

void test_pink_is_flat() {
    SpectrumOptions o = plain_options();
    const Bands flat = pink_average(o, 10.f);
    float lowest = 1e9f, highest = 0.f;
    for (std::size_t b = 1; b < band_count - 1; ++b) {
        lowest = std::min(lowest, flat[b]);
        highest = std::max(highest, flat[b]);
    }
    std::printf("pink: bands read %.3f to %.3f\n", static_cast<double>(lowest),
                static_cast<double>(highest));
    CHECK(lowest > 0.f);
    CHECK(highest / lowest < 2.f);

    // With tilt, the top reads higher than the bottom: a factor of two per decade.
    o.tilt = 1.f;
    const Bands tilted = pink_average(o, 3.f);
    CHECK(tilted[60] > 3.f * tilted[4]);
}

void test_smoothing() {
    SpectrumOptions o;  // the defaults: 10 ms attack, 150 ms release
    Spectrum spectrum(o);
    feed_sine(spectrum, 1000.f, 1.f, 1.f, 1.f);
    const std::size_t band = band_for(spectrum, 1000.f);
    const float steady = spectrum.left()[band];
    CHECK(steady > 0.8f && steady < 1.2f);

    // Silence: the band falls with the release time, not at once.
    std::vector<float> silence(o.sample_rate / 30 * 2, 0.f);  // one hop
    float previous = steady;
    float after_two_hops = 0.f;
    for (int hop = 1; hop <= 30; ++hop) {
        CHECK(spectrum.feed(silence));
        const float now = spectrum.left()[band];
        CHECK(now < previous);
        previous = now;
        if (hop == 2) after_two_hops = now;  // about 67 ms in
    }
    CHECK(after_two_hops > 0.3f * steady);
    CHECK(previous < 0.05f);

    spectrum.reset();
    for (float v : spectrum.left()) CHECK(v == 0.f);
}

void test_coarse_bands() {
    Bands bands{};
    for (std::size_t b = 0; b < band_count; ++b) bands[b] = static_cast<float>(b);
    const auto b32 = Spectrum::bands32(bands);
    const auto b16 = Spectrum::bands16(bands);
    CHECK(b32[0] == 0.5f && b32[31] == 62.5f);
    CHECK(b16[0] == 1.5f && b16[15] == 61.5f);
}

}  // namespace

int main() {
    test_fft_against_dft();
    test_fft_impulse_and_tone();
    test_band_layout();
    test_silence_and_update_rate();
    test_sines();
    test_mono_and_gain();
    test_pink_is_flat();
    test_smoothing();
    test_coarse_bands();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
