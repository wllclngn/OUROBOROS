#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <future>

namespace ouroboros::util {

// Parallel image decoding pool using fixed thread pool across CPU cores
// Similar to library scanner parallel metadata parsing pattern
class ImageDecoderPool {
public:
    // Job type: function that returns void
    using DecodeJob = std::function<void()>;

    // Singleton pattern (like ImageRenderer)
    static ImageDecoderPool& instance();

    // Submit a job to the pool (non-blocking)
    // Returns false if queue is full (prevents memory explosion)
    [[nodiscard]] bool submit_job(DecodeJob job);

    // Get queue size for monitoring
    [[nodiscard]] size_t get_queue_size() const;

    // Destructor waits for all jobs to complete
    ~ImageDecoderPool();

private:
    ImageDecoderPool();
    ImageDecoderPool(const ImageDecoderPool&) = delete;
    ImageDecoderPool& operator=(const ImageDecoderPool&) = delete;

    // Worker thread function
    void worker_thread();

    // Thread pool
    std::vector<std::thread> workers_;

    // Job queue with mutex
    std::queue<DecodeJob> job_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;

    // Control flags
    std::atomic<bool> stop_{false};

    // Maximum queue size to prevent memory explosion during fast scrolling
    static constexpr size_t MAX_QUEUE_SIZE = 50;
};

} // namespace ouroboros::util
