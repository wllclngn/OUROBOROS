#include "util/ImageDecoderPool.hpp"
#include "util/Logger.hpp"
#include <thread>

namespace ouroboros::util {

ImageDecoderPool& ImageDecoderPool::instance() {
    static ImageDecoderPool pool;
    return pool;
}

ImageDecoderPool::ImageDecoderPool() {
    // Use ALL CPU cores for true parallel processing (like library scanner)
    size_t num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4; // Fallback

    Logger::info("ImageDecoderPool: Initializing with " + std::to_string(num_threads) + " worker threads");

    // Create worker threads
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this]() {
            worker_thread();
        });
    }
}

ImageDecoderPool::~ImageDecoderPool() {
    Logger::info("ImageDecoderPool: Shutting down");

    // Signal all threads to stop
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    cv_.notify_all();

    // Wait for all threads to complete
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    Logger::info("ImageDecoderPool: Shutdown complete");
}

bool ImageDecoderPool::submit_job(DecodeJob job) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        // Limit queue size to prevent memory explosion during fast scrolling
        if (job_queue_.size() >= MAX_QUEUE_SIZE) {
            Logger::debug("ImageDecoderPool: Queue full (" + std::to_string(job_queue_.size()) +
                         " jobs), dropping job");
            return false;
        }

        job_queue_.push(std::move(job));
    }

    // Notify one worker thread
    cv_.notify_one();
    return true;
}

size_t ImageDecoderPool::get_queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return job_queue_.size();
}

void ImageDecoderPool::worker_thread() {
    Logger::debug("ImageDecoderPool: Worker thread " +
                  std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                  " started");

    while (!stop_) {
        DecodeJob job;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);

            // Wait for job or stop signal
            cv_.wait(lock, [this]() {
                return stop_ || !job_queue_.empty();
            });

            // Check stop condition after wake up
            if (stop_ && job_queue_.empty()) {
                break;
            }

            // Get next job
            if (!job_queue_.empty()) {
                job = std::move(job_queue_.front());
                job_queue_.pop();
            }
        }

        // Execute job outside the lock (true parallel processing)
        if (job) {
            job();
        }
    }

    Logger::debug("ImageDecoderPool: Worker thread " +
                  std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                  " stopped");
}

} // namespace ouroboros::util
