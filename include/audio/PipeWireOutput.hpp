#pragma once

#include "audio/PipeWireContext.hpp"
#include "audio/AudioRingBuffer.hpp"
#include <cstddef>
#include <cstdint>
#include <atomic>

struct pw_thread_loop;
struct pw_stream;

namespace audio {

class PipeWireOutput {
public:
    PipeWireOutput();
    ~PipeWireOutput();

    // Now requires the shared context
    [[nodiscard]] bool init(PipeWireContext& context, int sample_rate, int channels);
    void close();

    // Ring buffer: producer (PlaybackCollector) writes decoded PCM here.
    // Consumer (on_process) pulls from it on PipeWire's RT thread.
    AudioRingBuffer& ring_buffer() { return ring_; }

    // Flush ring buffer (for seek/clear). Only call when stream is paused.
    void flush_ring();

    void pause(bool paused);

    bool is_initialized() const { return stream_ != nullptr; }

    int get_sample_rate() const { return sample_rate_; }
    int get_channels() const { return channels_; }

    void set_volume(int percent);

    // Frames consumed by PipeWire callback (for position tracking)
    size_t frames_consumed() const { return ring_.frames_consumed(); }

private:
    friend void on_process_callback(void* userdata);

    uint32_t device_id_ = 0;
    int sample_rate_ = 0;
    int channels_ = 0;
    bool paused_ = false;
    std::atomic<int> volume_{50};

    AudioRingBuffer ring_;

    // Per-stream state
    struct pw_stream* stream_ = nullptr;
    PipeWireContext* context_ = nullptr; // Non-owning
};

} // namespace audio
