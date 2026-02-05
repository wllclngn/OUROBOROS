#pragma once

#include "AudioDecoder.hpp"

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;

namespace audio {

class M4ADecoder : public AudioDecoder {
public:
    M4ADecoder();
    ~M4ADecoder() override;

    [[nodiscard]] bool open(const std::string& filepath) override;
    void close() override;

    [[nodiscard]] int read_pcm(float* buffer, int max_frames) override;

    [[nodiscard]] int get_sample_rate() const override { return sample_rate_; }
    [[nodiscard]] int get_channels() const override { return channels_; }
    [[nodiscard]] long get_total_frames() const override { return total_frames_; }
    [[nodiscard]] long get_position_frames() const override { return position_frames_; }

    [[nodiscard]] bool seek(long frame) override;
    [[nodiscard]] bool is_open() const override { return format_ctx_ != nullptr; }

private:
    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;

    int audio_stream_index_ = -1;
    int sample_rate_ = 0;
    int channels_ = 0;
    long total_frames_ = 0;
    long position_frames_ = 0;

    // Residual buffer for frames not fully consumed
    float* residual_buffer_ = nullptr;
    int residual_frames_ = 0;
    int residual_offset_ = 0;
};

} // namespace audio
