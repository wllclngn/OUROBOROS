#include "backend/SnapshotPublisher.hpp"

namespace ouroboros::backend {

// Hot-path methods below run hundreds of times/second; they are intentionally
// logging-free to avoid an I/O storm.

SnapshotPublisher::SnapshotPublisher() {
    staging_.library = std::make_shared<model::LibraryState>();
    staging_.queue = std::make_shared<model::QueueState>();
    staging_.seq = 0;
    current_.store(std::make_shared<const model::Snapshot>(staging_),
                   std::memory_order_release);
}

SnapshotPublisher::~SnapshotPublisher() = default;

void SnapshotPublisher::publish(model::Snapshot snap) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap.seq = staging_.seq + 1;
    staging_ = std::move(snap);
    current_.store(std::make_shared<const model::Snapshot>(staging_),
                   std::memory_order_release);
}

void SnapshotPublisher::update(std::function<void(model::Snapshot&)> updater) {
    std::lock_guard<std::mutex> lock(mutex_);
    updater(staging_);
    ++staging_.seq;
    // Install a fresh immutable copy. The staging buffer keeps accumulating for
    // the next writer; what readers see is never touched again.
    current_.store(std::make_shared<const model::Snapshot>(staging_),
                   std::memory_order_release);
}

// READ PATH. One atomic load. The returned handle owns the snapshot, so it stays
// valid for as long as the caller holds it no matter how many publishes land in
// the meantime, and seq is monotonic across reads because a published snapshot is
// never rewritten.
std::shared_ptr<const model::Snapshot> SnapshotPublisher::get_current() const {
    return current_.load(std::memory_order_acquire);
}

}  // namespace ouroboros::backend
