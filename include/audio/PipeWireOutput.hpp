#pragma once

#include "audio/PipeWireContext.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

struct pw_thread_loop;
struct pw_stream;

namespace audio {

class PipeWireOutput {
public:
    PipeWireOutput();
    ~PipeWireOutput();

    // Now requires the shared context
    bool init(PipeWireContext& context, int sample_rate, int channels);
    void close();
    
    // Write audio data to PipeWire stream
    // Returns number of frames actually written
    size_t write(const float* data, size_t frames);
    void pause(bool paused);
    
    bool is_initialized() const { return stream_ != nullptr; }
    
    int get_sample_rate() const { return sample_rate_; }
    int get_channels() const { return channels_; }

    void set_volume(int percent);

private:
    uint32_t device_id_ = 0;
    int sample_rate_ = 0;
    int channels_ = 0;
    bool paused_ = false;
    int volume_ = 50;
    
    // Fallback support
    bool use_s16_ = false;
    std::vector<int16_t> s16_buffer_;

    // Per-stream state
    struct pw_stream* stream_ = nullptr;
    PipeWireContext* context_ = nullptr; // Non-owning
};

} // namespace audio