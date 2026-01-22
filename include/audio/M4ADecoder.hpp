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

    bool open(const std::string& filepath) override;
    void close() override;

    int read_pcm(float* buffer, int max_frames) override;

    int get_sample_rate() const override { return sample_rate_; }
    int get_channels() const override { return channels_; }
    long get_total_frames() const override { return total_frames_; }
    long get_position_frames() const override { return position_frames_; }

    bool seek(long frame) override;
    bool is_open() const override { return format_ctx_ != nullptr; }

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
