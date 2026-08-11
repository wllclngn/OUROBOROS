#include "SimpleTest.hpp"
#include "audio/AudioRingBuffer.hpp"
#include "audio/SpectroTap.hpp"
#include "backend/SnapshotPublisher.hpp"

#include <atomic>
#include <thread>
#include <vector>

// AudioRingBuffer -- SPSC correctness

TEST_CASE(test_ring_basic_write_read) {
    audio::AudioRingBuffer ring;
    ring.init(64, 2);  // 64 frames, stereo

    float in[8] = {1, 2, 3, 4, 5, 6, 7, 8};  // 4 stereo frames
    ASSERT_EQ(ring.write(in, 4), static_cast<size_t>(4));
    ASSERT_EQ(ring.read_available_frames(), static_cast<size_t>(4));

    float out[8] = {0};
    ASSERT_EQ(ring.read(out, 4), static_cast<size_t>(4));
    for (int i = 0; i < 8; ++i) ASSERT_NEAR(out[i], in[i], 1e-6f);
    ASSERT_EQ(ring.read_available_frames(), static_cast<size_t>(0));
}

TEST_CASE(test_ring_wraparound) {
    audio::AudioRingBuffer ring;
    ring.init(8, 1);  // small, forces wrap (rounds to power of 2)

    // Push/pop repeatedly so the read/write positions wrap past the end.
    float next = 0.0f, expect = 0.0f;
    for (int iter = 0; iter < 100; ++iter) {
        float buf[3] = {next, next + 1, next + 2};
        size_t w = ring.write(buf, 3);
        next += static_cast<float>(w);
        float out[3] = {0};
        size_t r = ring.read(out, w);
        for (size_t i = 0; i < r; ++i) {
            ASSERT_NEAR(out[i], expect, 1e-6f);
            expect += 1.0f;
        }
    }
}

TEST_CASE(test_ring_overflow_truncates) {
    audio::AudioRingBuffer ring;
    ring.init(4, 1);  // capacity rounds to 4 samples
    float big[16];
    for (int i = 0; i < 16; ++i) big[i] = static_cast<float>(i);
    size_t written = ring.write(big, 16);
    ASSERT_TRUE(written <= 4);  // never overruns capacity
    ASSERT_EQ(ring.read_available_frames(), written);
}

TEST_CASE(test_ring_spsc_threaded) {
    audio::AudioRingBuffer ring;
    ring.init(1024, 1);
    constexpr size_t N = 200000;

    std::thread producer([&] {
        size_t sent = 0;
        float v = 0;
        while (sent < N) {
            float chunk[64];
            size_t n = std::min<size_t>(64, N - sent);
            for (size_t i = 0; i < n; ++i) chunk[i] = v + static_cast<float>(i);
            size_t w = ring.write(chunk, n);
            v += static_cast<float>(w);
            sent += w;
        }
    });

    size_t got = 0;
    float expect = 0;
    bool ordered = true;
    while (got < N) {
        float out[64];
        size_t r = ring.read(out, 64);
        for (size_t i = 0; i < r; ++i) {
            if (out[i] != expect) ordered = false;
            expect += 1.0f;
        }
        got += r;
    }
    producer.join();
    ASSERT_TRUE(ordered);
    ASSERT_EQ(got, N);
}

// SpectroTap -- seqlock window mirror

TEST_CASE(test_spectrotap_publish_read) {
    auto& tap = audio::SpectroTap::instance();
    tap.reset();

    // Publish more than one full window of a ramp (mono).
    std::vector<float> block(audio::SpectroTap::WINDOW * 2);
    for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<float>(i % 97);
    tap.publish(block.data(), block.size(), 1, 48000);

    std::vector<float> win(audio::SpectroTap::WINDOW);
    int sr = 0;
    ASSERT_TRUE(tap.read_latest(win.data(), sr));
    ASSERT_EQ(sr, 48000);
    // The window should hold the most recent WINDOW samples of the ramp.
    size_t start = block.size() - audio::SpectroTap::WINDOW;
    for (size_t i = 0; i < audio::SpectroTap::WINDOW; ++i) {
        ASSERT_NEAR(win[i], block[start + i], 1e-6f);
    }
}

TEST_CASE(test_spectrotap_reset_clears) {
    auto& tap = audio::SpectroTap::instance();
    tap.reset();
    // After reset with no data, a read cannot produce a stable full window.
    std::vector<float> win(audio::SpectroTap::WINDOW);
    int sr = 0;
    ASSERT_FALSE(tap.read_latest(win.data(), sr));
}

TEST_CASE(test_spectrotap_threaded_no_crash) {
    auto& tap = audio::SpectroTap::instance();
    tap.reset();
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        std::vector<float> blk(512);
        float v = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            for (auto& s : blk) s = v++;
            tap.publish(blk.data(), blk.size(), 2, 44100);
        }
    });
    std::vector<float> win(audio::SpectroTap::WINDOW);
    int reads = 0, ok = 0;
    for (int i = 0; i < 5000; ++i) {
        int sr = 0;
        if (tap.read_latest(win.data(), sr)) { ++ok; ASSERT_EQ(sr, 44100); }
        ++reads;
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    ASSERT_TRUE(reads == 5000);  // completed without deadlock/crash
    (void)ok;
}

// SnapshotPublisher -- immutable published snapshots, monotonic seq

TEST_CASE(test_snapshot_publish_increments_seq_and_carries_state) {
    ouroboros::backend::SnapshotPublisher pub;
    uint64_t s0 = pub.get_current()->seq;
    pub.update([](ouroboros::model::Snapshot& s) { s.player.volume_percent = 42; });
    auto snap = pub.get_current();
    ASSERT_TRUE(snap->seq == s0 + 1);
    ASSERT_EQ(snap->player.volume_percent, 42);
}

TEST_CASE(test_snapshot_seq_monotonic_single_writer) {
    ouroboros::backend::SnapshotPublisher pub;
    uint64_t last = pub.get_current()->seq;
    for (int i = 0; i < 10000; ++i) {
        pub.update([i](ouroboros::model::Snapshot& s) { s.player.playback_position_ms = i; });
        uint64_t s = pub.get_current()->seq;
        ASSERT_TRUE(s == last + 1);
        last = s;
    }
}

// A handle outlives any number of publishes. This is the invariant the previous
// double-buffer could not offer: it recycled the buffer a reader was still in.
TEST_CASE(test_snapshot_handle_survives_later_publishes) {
    ouroboros::backend::SnapshotPublisher pub;
    pub.update([](ouroboros::model::Snapshot& s) { s.player.volume_percent = 7; });
    auto held = pub.get_current();
    const uint64_t held_seq = held->seq;
    for (int i = 0; i < 5000; ++i) {
        pub.update([i](ouroboros::model::Snapshot& s) { s.player.volume_percent = i % 100; });
    }
    ASSERT_EQ(held->player.volume_percent, 7);   // never rewritten underneath us
    ASSERT_TRUE(held->seq == held_seq);
}

// Concurrent readers see seq advance and never go backwards. This assertion was
// removed once because the double-buffer design could not honour it; publishing
// immutable snapshots does, so it is a real gate now rather than a comment.
TEST_CASE(test_snapshot_seq_monotonic_under_concurrent_readers) {
    ouroboros::backend::SnapshotPublisher pub;
    std::atomic<bool> stop{false};
    std::atomic<bool> regressed{false};
    std::atomic<int> observed{0};

    auto reader = [&] {
        uint64_t last = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            auto snap = pub.get_current();
            uint64_t s = snap->seq;
            if (s < last) regressed.store(true, std::memory_order_relaxed);
            last = s;
            observed.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread r1(reader), r2(reader);
    for (int i = 0; i < 20000; ++i) {
        pub.update([i](ouroboros::model::Snapshot& s) { s.player.playback_position_ms = i; });
    }
    stop.store(true, std::memory_order_relaxed);
    r1.join();
    r2.join();

    ASSERT_TRUE(!regressed.load());
    ASSERT_TRUE(observed.load() > 0);
}

int main() {
    return ouroboros::test::TestRunner::instance().run_all();
}
