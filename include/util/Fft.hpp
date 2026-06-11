#pragma once

#include <cstddef>
#include <vector>

namespace ouroboros::util {

// Iterative radix-2 Cooley-Tukey FFT for real input, sized once at init.
// Twiddle factors and bit-reversal table precomputed; transform() performs
// zero allocations (all working buffers member-owned and reused).
//
// Feeding the spectrogram: hann_window() -> transform() -> magnitudes(),
// then map_log_bins() folds the linear magnitude spectrum into display bins
// spaced logarithmically in frequency.
class Fft {
public:
    // window_size must be a power of 2 (default spectrogram window 1024).
    explicit Fft(size_t window_size);

    [[nodiscard]] size_t window_size() const { return n_; }
    [[nodiscard]] size_t spectrum_size() const { return n_ / 2; }

    // Apply a Hann window to samples (length window_size) in place.
    void hann_window(float* samples) const;

    // Real-input FFT: fills the internal spectrum with bin magnitudes
    // (length spectrum_size). Input length must be window_size.
    void transform(const float* samples);

    // Magnitude per bin from the last transform (length spectrum_size).
    [[nodiscard]] const std::vector<float>& magnitudes() const { return mag_; }

    // Fold the magnitude spectrum into out_bins display bins spaced
    // logarithmically from freq_min Hz to Nyquist (sample_rate/2).
    // Each display bin takes the MAX of its source bins (peaks survive),
    // converted to dB and normalized to [0,1] against a fixed floor:
    // 0.0 = floor_db (default -72 dB), 1.0 = 0 dB full scale.
    void map_log_bins(float* out, size_t out_bins, int sample_rate,
                      float freq_min = 30.0f, float floor_db = -72.0f) const;

private:
    size_t n_;
    std::vector<size_t> bit_rev_;     // bit-reversal permutation
    std::vector<float> tw_re_;        // twiddle factors, all stages packed
    std::vector<float> tw_im_;
    std::vector<float> re_;           // working buffers
    std::vector<float> im_;
    std::vector<float> mag_;          // last transform's magnitudes
    std::vector<float> hann_;         // precomputed Hann coefficients
};

}  // namespace ouroboros::util
