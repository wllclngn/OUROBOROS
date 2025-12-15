#pragma once

#include "AudioDecoder.hpp"
#include <mpg123.h>

namespace audio {

class MP3Decoder : public AudioDecoder {
public:
    MP3Decoder();
    ~MP3Decoder() override;

    bool open(const std::string& filepath) override;
    void close() override;
    
    int read_pcm(float* buffer, int max_frames) override;
    
    int get_sample_rate() const override { return sample_rate_; }
    int get_channels() const override { return channels_; }
    long get_total_frames() const override { return total_frames_; }
    long get_position_frames() const override { return position_frames_; }
    
    bool seek(long frame) override;
    bool is_open() const override { return handle_ != nullptr; }

private:
    mpg123_handle* handle_ = nullptr;
    int sample_rate_ = 0;
    int channels_ = 0;
    long total_frames_ = 0;
    long position_frames_ = 0;
};

} // namespace audio
