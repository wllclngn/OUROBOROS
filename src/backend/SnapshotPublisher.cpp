#include "backend/SnapshotPublisher.hpp"
#include "util/Logger.hpp"

namespace ouroboros::backend {

SnapshotPublisher::SnapshotPublisher() {
}

SnapshotPublisher::~SnapshotPublisher() = default;

void SnapshotPublisher::publish(model::Snapshot snap) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Copy the provided snapshot into the back buffer
    // Note: This copies the lightweight structure (shared_ptrs), not the heavy data
    buffers_.back() = snap;
    buffers_.publish();
}

void SnapshotPublisher::update(std::function<void(model::Snapshot&)> updater) {
    // LOGGING DISABLED: Called hundreds of times/second in playback loop, creates I/O storm
    // util::Logger::debug("SnapshotPublisher::update - acquiring lock...");
    std::lock_guard<std::mutex> lock(mutex_);
    // util::Logger::debug("SnapshotPublisher::update - lock acquired, calling updater");
    // Modify the back buffer directly
    updater(buffers_.back());
    // util::Logger::debug("SnapshotPublisher::update - updater done, publishing");
    buffers_.publish();
    // util::Logger::debug("SnapshotPublisher::update - done, releasing lock");
}

// LOCK-FREE READ PATH
// This method is called frequently (every 33ms by main loop).
// It does NOT acquire the mutex because SnapshotBuffers::front()
// uses an atomic pointer (std::atomic<Snapshot*> front_).
// Multiple readers can call this concurrently. Writers in update()
// modify the back buffer only, never the front buffer that we read.
std::shared_ptr<const model::Snapshot> SnapshotPublisher::get_current() const {
    // LOGGING DISABLED: Called 100+ times/second, creates I/O storm
    // util::Logger::debug("SnapshotPublisher::get_current - reading snapshot (lock-free)");
    // NO MUTEX: buffers_.front() uses atomic pointer internally
    // Safe for concurrent reads while update() may be modifying back buffer
    auto result = std::make_shared<model::Snapshot>(buffers_.front());
    // util::Logger::debug("SnapshotPublisher::get_current - snapshot read complete");
    return result;
}

}  // namespace ouroboros::backend