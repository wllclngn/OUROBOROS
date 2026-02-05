#pragma once

#include <string>
#include <cstddef>
#include <cstdint>

namespace audio {

class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    [[nodiscard]] virtual bool open(const std::string& filepath) = 0;
    virtual void close() = 0;
    
    [[nodiscard]] virtual int read_pcm(float* buffer, int max_frames) = 0;
    
    [[nodiscard]] virtual int get_sample_rate() const = 0;
    [[nodiscard]] virtual int get_channels() const = 0;
    [[nodiscard]] virtual long get_total_frames() const = 0;
    [[nodiscard]] virtual long get_position_frames() const = 0;
    
    [[nodiscard]] virtual bool seek(long frame) = 0;
    [[nodiscard]] virtual bool is_open() const = 0;
    
    [[nodiscard]] virtual bool seek_to_ms(int64_t ms) {
        if (get_sample_rate() == 0) return false;
        long frame = (ms * get_sample_rate()) / 1000;
        return seek(frame);
    }
    
    [[nodiscard]] long get_position_ms() const {
        if (get_sample_rate() == 0) return 0;
        return (get_position_frames() * 1000) / get_sample_rate();
    }
    
    [[nodiscard]] long get_duration_ms() const {
        if (get_sample_rate() == 0) return 0;
        return (get_total_frames() * 1000) / get_sample_rate();
    }
};

} // namespace audio
