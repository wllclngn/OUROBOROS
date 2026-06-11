#include "SimpleTest.hpp"
#include "util/Fft.hpp"

#include <cmath>
#include <numbers>
#include <vector>

using ouroboros::util::Fft;

namespace {
constexpr size_t N = 1024;
constexpr float PI2 = 2.0f * std::numbers::pi_v<float>;
}

TEST_CASE(test_fft_impulse_flat_spectrum) {
    // delta[0] = 1: every bin magnitude = 2/N (flat)
    Fft fft(N);
    std::vector<float> x(N, 0.0f);
    x[0] = 1.0f;
    fft.transform(x.data());

    const auto& mag = fft.magnitudes();
    float expected = 2.0f / static_cast<float>(N);
    for (size_t i = 0; i < mag.size(); ++i) {
        ASSERT_NEAR(mag[i], expected, 1e-5f);
    }
}

TEST_CASE(test_fft_dc_bin_zero_only) {
    // Constant 1.0: all energy in bin 0, nothing elsewhere
    Fft fft(N);
    std::vector<float> x(N, 1.0f);
    fft.transform(x.data());

    const auto& mag = fft.magnitudes();
    ASSERT_NEAR(mag[0], 2.0f, 1e-3f);  // norm 2/N * N = 2 for DC
    for (size_t i = 1; i < mag.size(); ++i) {
        ASSERT_NEAR(mag[i], 0.0f, 1e-4f);
    }
}

TEST_CASE(test_fft_single_tone_bin_placement) {
    // Full-scale sine exactly on bin 64: magnitude ~1.0 there, ~0 elsewhere
    Fft fft(N);
    constexpr size_t TONE_BIN = 64;
    std::vector<float> x(N);
    for (size_t i = 0; i < N; ++i) {
        x[i] = std::sin(PI2 * static_cast<float>(TONE_BIN) * static_cast<float>(i) /
                        static_cast<float>(N));
    }
    fft.transform(x.data());

    const auto& mag = fft.magnitudes();
    ASSERT_NEAR(mag[TONE_BIN], 1.0f, 1e-3f);
    for (size_t i = 0; i < mag.size(); ++i) {
        if (i == TONE_BIN) continue;
        ASSERT_TRUE(mag[i] < 1e-3f);
    }
}

TEST_CASE(test_fft_two_tones_resolved) {
    // Two on-bin tones at different amplitudes both land where they should
    Fft fft(N);
    std::vector<float> x(N);
    for (size_t i = 0; i < N; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(N);
        x[i] = std::sin(PI2 * 32.0f * t) + 0.5f * std::sin(PI2 * 200.0f * t);
    }
    fft.transform(x.data());

    const auto& mag = fft.magnitudes();
    ASSERT_NEAR(mag[32], 1.0f, 1e-3f);
    ASSERT_NEAR(mag[200], 0.5f, 1e-3f);
}

TEST_CASE(test_fft_parseval_energy_conserved) {
    // Sum of squared samples == sum of squared spectrum (with our 2/N norm,
    // time energy * 2/N == sum mag^2 / 2 for real signals; verify via ratio)
    Fft fft(N);
    std::vector<float> x(N);
    for (size_t i = 0; i < N; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(N);
        x[i] = std::sin(PI2 * 7.0f * t) + 0.3f * std::sin(PI2 * 100.0f * t) + 0.1f;
    }

    float time_energy = 0.0f;
    for (float v : x) time_energy += v * v;

    fft.transform(x.data());
    const auto& mag = fft.magnitudes();

    // Undo the 2/N display norm: raw |X[k]|^2 = (mag*N/2)^2.
    // Parseval (real input, half spectrum): sum x^2 = (1/N) * sum_full |X|^2
    //   = (1/N) * (|X[0]|^2 + 2 * sum_{1..N/2-1} |X[k]|^2)
    float half = static_cast<float>(N) / 2.0f;
    float spec_sum = 0.0f;
    for (size_t i = 1; i < mag.size(); ++i) {
        float raw = mag[i] * half;
        spec_sum += 2.0f * raw * raw;
    }
    float dc_raw = mag[0] * half;
    spec_sum += dc_raw * dc_raw;
    float freq_energy = spec_sum / static_cast<float>(N);

    ASSERT_NEAR(time_energy / freq_energy, 1.0f, 1e-2f);
}

TEST_CASE(test_fft_hann_window_applied) {
    // Hann zeroes the endpoints and preserves the midpoint
    Fft fft(N);
    std::vector<float> x(N, 1.0f);
    fft.hann_window(x.data());

    ASSERT_NEAR(x[0], 0.0f, 1e-6f);
    ASSERT_NEAR(x[N - 1], 0.0f, 1e-6f);
    ASSERT_NEAR(x[N / 2], 1.0f, 1e-3f);
}

TEST_CASE(test_fft_log_bins_normalized_range) {
    // Full-scale tone: every display bin lands in [0,1]; the bin holding the
    // tone reads ~1.0 (0 dB), empty bins read 0.0 (at/below the floor)
    Fft fft(N);
    constexpr size_t TONE_BIN = 64;
    std::vector<float> x(N);
    for (size_t i = 0; i < N; ++i) {
        x[i] = std::sin(PI2 * static_cast<float>(TONE_BIN) * static_cast<float>(i) /
                        static_cast<float>(N));
    }
    fft.transform(x.data());

    constexpr size_t BINS = 64;
    float out[BINS];
    fft.map_log_bins(out, BINS, 44100);

    float peak = 0.0f;
    for (size_t b = 0; b < BINS; ++b) {
        ASSERT_TRUE(out[b] >= 0.0f);
        ASSERT_TRUE(out[b] <= 1.0f);
        if (out[b] > peak) peak = out[b];
    }
    ASSERT_NEAR(peak, 1.0f, 1e-2f);
}

int main() {
    return ouroboros::test::TestRunner::instance().run_all();
}
