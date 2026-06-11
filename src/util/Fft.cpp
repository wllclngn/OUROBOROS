#include "util/Fft.hpp"

#include <cassert>
#include <cmath>
#include <numbers>

namespace ouroboros::util {

namespace {
    constexpr float TWO_PI = 2.0f * std::numbers::pi_v<float>;
}

Fft::Fft(size_t window_size) : n_(window_size) {
    assert(n_ >= 2 && (n_ & (n_ - 1)) == 0 && "window size must be a power of 2");

    // Bit-reversal permutation
    bit_rev_.resize(n_);
    size_t log2n = 0;
    while ((size_t{1} << log2n) < n_) ++log2n;
    for (size_t i = 0; i < n_; ++i) {
        size_t r = 0;
        for (size_t b = 0; b < log2n; ++b) {
            if (i & (size_t{1} << b)) r |= size_t{1} << (log2n - 1 - b);
        }
        bit_rev_[i] = r;
    }

    // Twiddle factors per stage, packed: stage with half-size m/2 stores
    // m/2 factors contiguously. Total = n - 1 across all stages.
    tw_re_.reserve(n_);
    tw_im_.reserve(n_);
    for (size_t m = 2; m <= n_; m <<= 1) {
        size_t half = m / 2;
        for (size_t k = 0; k < half; ++k) {
            float angle = -TWO_PI * static_cast<float>(k) / static_cast<float>(m);
            tw_re_.push_back(std::cos(angle));
            tw_im_.push_back(std::sin(angle));
        }
    }

    re_.resize(n_);
    im_.resize(n_);
    mag_.resize(n_ / 2);

    hann_.resize(n_);
    for (size_t i = 0; i < n_; ++i) {
        hann_[i] = 0.5f * (1.0f - std::cos(TWO_PI * static_cast<float>(i) /
                                           static_cast<float>(n_ - 1)));
    }
}

void Fft::hann_window(float* samples) const {
    for (size_t i = 0; i < n_; ++i) {
        samples[i] *= hann_[i];
    }
}

void Fft::transform(const float* samples) {
    // Bit-reversed copy into working buffers
    for (size_t i = 0; i < n_; ++i) {
        re_[bit_rev_[i]] = samples[i];
        im_[bit_rev_[i]] = 0.0f;
    }

    // Iterative butterflies
    size_t tw_base = 0;
    for (size_t m = 2; m <= n_; m <<= 1) {
        size_t half = m / 2;
        for (size_t start = 0; start < n_; start += m) {
            for (size_t k = 0; k < half; ++k) {
                float wr = tw_re_[tw_base + k];
                float wi = tw_im_[tw_base + k];
                size_t a = start + k;
                size_t b = a + half;

                float tr = wr * re_[b] - wi * im_[b];
                float ti = wr * im_[b] + wi * re_[b];

                re_[b] = re_[a] - tr;
                im_[b] = im_[a] - ti;
                re_[a] += tr;
                im_[a] += ti;
            }
        }
        tw_base += half;
    }

    // Magnitudes, normalized by N/2 so a full-scale sine reads ~1.0
    float norm = 2.0f / static_cast<float>(n_);
    for (size_t i = 0; i < n_ / 2; ++i) {
        mag_[i] = std::sqrt(re_[i] * re_[i] + im_[i] * im_[i]) * norm;
    }
}

void Fft::map_log_bins(float* out, size_t out_bins, int sample_rate,
                       float freq_min, float floor_db) const {
    if (out_bins == 0 || sample_rate <= 0) return;

    float nyquist = static_cast<float>(sample_rate) / 2.0f;
    if (freq_min < 1.0f) freq_min = 1.0f;
    if (freq_min >= nyquist) freq_min = nyquist / 2.0f;

    float hz_per_bin = nyquist / static_cast<float>(mag_.size());
    float log_min = std::log(freq_min);
    float log_span = std::log(nyquist) - log_min;

    for (size_t b = 0; b < out_bins; ++b) {
        // Frequency edges of this display bin (log spacing)
        float f0 = std::exp(log_min + log_span * static_cast<float>(b) /
                            static_cast<float>(out_bins));
        float f1 = std::exp(log_min + log_span * static_cast<float>(b + 1) /
                            static_cast<float>(out_bins));

        auto i0 = static_cast<size_t>(f0 / hz_per_bin);
        auto i1 = static_cast<size_t>(f1 / hz_per_bin);
        if (i0 >= mag_.size()) i0 = mag_.size() - 1;
        if (i1 >= mag_.size()) i1 = mag_.size() - 1;
        if (i1 < i0) i1 = i0;

        // MAX over source bins: narrow peaks survive the fold
        float peak = 0.0f;
        for (size_t i = i0; i <= i1; ++i) {
            if (mag_[i] > peak) peak = mag_[i];
        }

        // dB against full scale, clamped to floor, normalized to [0,1]
        float db = (peak > 0.0f) ? 20.0f * std::log10(peak) : floor_db;
        if (db < floor_db) db = floor_db;
        if (db > 0.0f) db = 0.0f;
        out[b] = 1.0f - (db / floor_db);
    }
}

}  // namespace ouroboros::util
