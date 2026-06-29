#include "backend/SnapshotPublisher.hpp"

namespace ouroboros::backend {

// Hot-path methods below run hundreds of times/second; they are intentionally
// logging-free to avoid an I/O storm.

SnapshotPublisher::SnapshotPublisher() {
}

SnapshotPublisher::~SnapshotPublisher() = default;

void SnapshotPublisher::publish(model::Snapshot snap) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Copy the lightweight structure (shared_ptrs), not the heavy data
    buffers_.back() = snap;
    buffers_.publish();
}

void SnapshotPublisher::update(std::function<void(model::Snapshot&)> updater) {
    std::lock_guard<std::mutex> lock(mutex_);
    updater(buffers_.back());
    buffers_.publish();
}

// LOCK-FREE READ PATH
// Called frequently (every 33ms by the main loop). It does NOT acquire the
// mutex because SnapshotBuffers::front() uses an atomic pointer
// (std::atomic<Snapshot*> front_). Multiple readers can call this concurrently.
// Writers in update() modify the back buffer only, never the front buffer read here.
std::shared_ptr<const model::Snapshot> SnapshotPublisher::get_current() const {
    auto result = std::make_shared<model::Snapshot>(buffers_.front());
    return result;
}

}  // namespace ouroboros::backend
