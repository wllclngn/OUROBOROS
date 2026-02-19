#pragma once

#include "AudioDecoder.hpp"
#include <array>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace audio {

class DSDDecoder : public AudioDecoder {
public:
    DSDDecoder();
    ~DSDDecoder() override;

    [[nodiscard]] bool open(const std::string& filepath) override;
    void close() override;

    [[nodiscard]] int read_pcm(float* buffer, int max_frames) override;

    [[nodiscard]] int get_sample_rate() const override { return OUTPUT_RATE; }
    [[nodiscard]] int get_channels() const override { return channels_; }
    [[nodiscard]] long get_total_frames() const override { return total_output_frames_; }
    [[nodiscard]] long get_position_frames() const override { return output_position_; }

    [[nodiscard]] bool seek(long frame) override;
    [[nodiscard]] bool is_open() const override { return file_ != nullptr; }

private:
    static constexpr int OUTPUT_RATE = 352800;

    // DSF file state
    FILE* file_ = nullptr;
    int channels_ = 0;
    uint32_t dsd_sample_rate_ = 0;
    uint64_t total_dsd_samples_ = 0;  // per channel
    uint32_t block_size_ = 0;         // bytes per channel per interleave block
    uint64_t data_offset_ = 0;        // byte offset to DSD sample data
    uint64_t data_size_ = 0;

    // Decimation parameters
    int bytes_per_output_ = 0;        // decimation_ratio / 8
    int num_groups_ = 0;              // filter length in byte groups
    long total_output_frames_ = 0;
    long output_position_ = 0;

    // Byte lookup tables: lut_[group][byte_value]
    // For 1-bit FIR: each entry = sum of +/- coefficients based on bit pattern
    std::vector<std::array<float, 256>> lut_;

    // Per-channel overlap from previous block (num_groups_ - 1 bytes)
    std::vector<std::vector<uint8_t>> overlap_;

    // Block reading
    std::vector<uint8_t> block_buffer_;           // raw interleaved DSF block
    std::vector<std::vector<uint8_t>> ch_data_;   // de-interleaved per-channel data
    int block_output_frames_ = 0;                 // output frames per block
    int block_frames_consumed_ = 0;               // how many output frames consumed from current block

    // Residual output (partial frames from previous read_pcm call)
    std::vector<float> residual_;
    int residual_frames_ = 0;
    int residual_offset_ = 0;

    bool parse_dsf_header();
    bool build_lut(const float* coeffs, int num_groups);
    bool read_next_block();
    int process_block(float* output, int max_frames);
    float compute_sample(const uint8_t* data, int pos);
};

} // namespace audio
