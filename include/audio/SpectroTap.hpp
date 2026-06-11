#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace audio {

// Lock-free single-writer tap of post-decode samples for the spectrogram.
// Writer: PipeWire on_process callback (RT thread) — mono-mixes interleaved
// frames into a ring. Reader: UI thread, assembles the most recent WINDOW
// samples at frame rate. Seqlock-style sequence counter: torn reads are
// detected and rejected; the reader keeps its previous window on failure.
// No locks, no allocations on either side.
class SpectroTap {
public:
    static constexpr size_t WINDOW = 4096;        // mono samples handed to the reader
    static constexpr size_t RING = WINDOW * 2;    // writer ring (power of 2)

    static SpectroTap& instance() {
        static SpectroTap tap;
        return tap;
    }

    // RT writer: mono-mix interleaved frames into the ring.
    void publish(const float* interleaved, size_t frames, int channels, int sample_rate) {
        if (!interleaved || frames == 0 || channels <= 0) return;

        uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);   // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);

        size_t pos = write_pos_;
        if (channels == 1) {
            for (size_t i = 0; i < frames; ++i) {
                ring_[pos & MASK] = interleaved[i];
                ++pos;
            }
        } else {
            float inv = 1.0f / static_cast<float>(channels);
            for (size_t i = 0; i < frames; ++i) {
                float acc = 0.0f;
                for (int c = 0; c < channels; ++c) {
                    acc += interleaved[i * channels + c];
                }
                ring_[pos & MASK] = acc * inv;
                ++pos;
            }
        }
        write_pos_ = pos;
        sample_rate_.store(sample_rate, std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);   // even: write complete
    }

    // UI reader: copy the most recent WINDOW samples into dst.
    // Returns false (dst untouched beyond scratch) if no stable read was
    // possible or fewer than WINDOW samples have ever been written.
    bool read_latest(float* dst, int& sample_rate) const {
        for (int attempt = 0; attempt < 3; ++attempt) {
            uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1) continue;  // write in progress

            size_t end = write_pos_;
            if (end < WINDOW) return false;  // not enough audio yet

            size_t start = end - WINDOW;
            size_t start_idx = start & MASK;
            size_t first = RING - start_idx;
            if (first >= WINDOW) {
                std::memcpy(dst, ring_ + start_idx, WINDOW * sizeof(float));
            } else {
                std::memcpy(dst, ring_ + start_idx, first * sizeof(float));
                std::memcpy(dst + first, ring_, (WINDOW - first) * sizeof(float));
            }
            int sr = sample_rate_.load(std::memory_order_relaxed);

            std::atomic_thread_fence(std::memory_order_acquire);
            uint64_t s2 = seq_.load(std::memory_order_relaxed);
            if (s1 == s2) {
                sample_rate = sr;
                return true;
            }
        }
        return false;  // torn three times; reader keeps previous window
    }

    // Zero the ring (track change / stop). Caller side-effect only; safe to
    // race with the writer (worst case: one stale window).
    void reset() {
        uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memset(const_cast<float*>(ring_), 0, sizeof(ring_));
        write_pos_ = 0;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }

private:
    SpectroTap() = default;
    SpectroTap(const SpectroTap&) = delete;
    SpectroTap& operator=(const SpectroTap&) = delete;

    static constexpr size_t MASK = RING - 1;
    static_assert((RING & MASK) == 0, "RING must be a power of 2");

    float ring_[RING] = {};
    size_t write_pos_ = 0;

    alignas(64) std::atomic<uint64_t> seq_{0};
    alignas(64) std::atomic<int> sample_rate_{44100};
};

}  // namespace audio
