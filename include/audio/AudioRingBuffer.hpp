#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <algorithm>

namespace audio {

// Lock-free SPSC ring buffer for interleaved float audio samples.
// Producer: PlaybackCollector decode thread
// Consumer: PipeWire on_process callback (RT thread)
class AudioRingBuffer {
public:
    AudioRingBuffer() = default;
    ~AudioRingBuffer() { delete[] buffer_; }

    // Non-copyable, non-movable (atomics)
    AudioRingBuffer(const AudioRingBuffer&) = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;

    void init(size_t capacity_frames, int channels) {
        channels_ = channels;

        // Round up to next power of 2 in samples
        size_t raw = capacity_frames * channels;
        size_t cap = 1;
        while (cap < raw) cap <<= 1;

        delete[] buffer_;
        buffer_ = new float[cap]();
        capacity_samples_ = cap;
        mask_ = cap - 1;

        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
        total_consumed_frames_.store(0, std::memory_order_relaxed);
    }

    // Flush all data. Only safe when consumer is paused (no concurrent reads).
    void reset() {
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
    }

    // Producer: write interleaved frames into ring buffer.
    // Returns number of frames actually written.
    size_t write(const float* data, size_t frames) {
        size_t samples = frames * channels_;
        size_t w = write_pos_.load(std::memory_order_relaxed);
        size_t r = read_pos_.load(std::memory_order_acquire);
        size_t available = capacity_samples_ - (w - r);
        size_t to_write = std::min(samples, available);

        if (to_write == 0) return 0;

        // Wrap-aware copy (up to two segments)
        size_t pos = w & mask_;
        size_t first = std::min(to_write, capacity_samples_ - pos);
        std::memcpy(buffer_ + pos, data, first * sizeof(float));
        if (to_write > first) {
            std::memcpy(buffer_, data + first, (to_write - first) * sizeof(float));
        }

        write_pos_.store(w + to_write, std::memory_order_release);
        return to_write / channels_;
    }

    // Consumer: read interleaved frames from ring buffer.
    // Returns number of frames actually read.
    size_t read(float* dst, size_t frames) {
        size_t samples = frames * channels_;
        size_t w = write_pos_.load(std::memory_order_acquire);
        size_t r = read_pos_.load(std::memory_order_relaxed);
        size_t available = w - r;
        size_t to_read = std::min(samples, available);

        if (to_read == 0) return 0;

        // Round down to whole frames
        to_read = (to_read / channels_) * channels_;
        if (to_read == 0) return 0;

        // Wrap-aware copy
        size_t pos = r & mask_;
        size_t first = std::min(to_read, capacity_samples_ - pos);
        std::memcpy(dst, buffer_ + pos, first * sizeof(float));
        if (to_read > first) {
            std::memcpy(dst + first, buffer_, (to_read - first) * sizeof(float));
        }

        read_pos_.store(r + to_read, std::memory_order_release);
        total_consumed_frames_.fetch_add(to_read / channels_, std::memory_order_relaxed);
        return to_read / channels_;
    }

    size_t write_available_frames() const {
        size_t w = write_pos_.load(std::memory_order_relaxed);
        size_t r = read_pos_.load(std::memory_order_acquire);
        return (capacity_samples_ - (w - r)) / channels_;
    }

    size_t read_available_frames() const {
        size_t w = write_pos_.load(std::memory_order_acquire);
        size_t r = read_pos_.load(std::memory_order_relaxed);
        return (w - r) / channels_;
    }

    size_t frames_consumed() const {
        return total_consumed_frames_.load(std::memory_order_relaxed);
    }

    int channels() const { return channels_; }

private:
    float* buffer_ = nullptr;
    size_t capacity_samples_ = 0;
    size_t mask_ = 0;
    int channels_ = 0;

    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};
    alignas(64) std::atomic<size_t> total_consumed_frames_{0};
};

} // namespace audio
