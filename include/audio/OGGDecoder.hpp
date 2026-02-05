#pragma once

#include "AudioDecoder.hpp"
#include <vorbis/vorbisfile.h>

namespace audio {

class OGGDecoder : public AudioDecoder {
public:
    OGGDecoder();
    ~OGGDecoder() override;

    [[nodiscard]] bool open(const std::string& filepath) override;
    void close() override;

    [[nodiscard]] int read_pcm(float* buffer, int max_frames) override;

    [[nodiscard]] int get_sample_rate() const override { return sample_rate_; }
    [[nodiscard]] int get_channels() const override { return channels_; }
    [[nodiscard]] long get_total_frames() const override { return total_frames_; }
    [[nodiscard]] long get_position_frames() const override { return position_frames_; }

    [[nodiscard]] bool seek(long frame) override;
    [[nodiscard]] bool is_open() const override { return is_open_; }

private:
    OggVorbis_File vf_{};
    bool is_open_ = false;
    int sample_rate_ = 0;
    int channels_ = 0;
    long total_frames_ = 0;
    long position_frames_ = 0;
};

} // namespace audio
