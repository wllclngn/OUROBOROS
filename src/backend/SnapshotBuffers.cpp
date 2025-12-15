#include "backend/SnapshotBuffers.hpp"
#include "util/Logger.hpp"
#include <mutex>

namespace ouroboros::backend {

SnapshotBuffers::SnapshotBuffers() {
    // Initialize default states
    auto empty_lib = std::make_shared<model::LibraryState>();
    auto empty_queue = std::make_shared<model::QueueState>();
    
    a_.library = empty_lib;
    a_.queue = empty_queue;
    a_.seq = 0;
    
    b_.library = empty_lib;
    b_.queue = empty_queue;
    b_.seq = 0;
    
    front_.store(&a_);
    back_ = &b_;
}

model::Snapshot& SnapshotBuffers::back() {
    // Only used by the producer holding the lock in SnapshotPublisher::update
    return *back_;
}

void SnapshotBuffers::publish() {
    // NOTE: This method is called by SnapshotPublisher::update which HOLDs a lock.
    // However, to be absolutely safe against other potential callers,
    // the caller (Publisher) MUST ensure mutual exclusion on the write side.
    //
    // The previous implementation did a dangerous copy: *back_ = *front_
    // This overwrote any partial state if multiple threads were racing.
    //
    // The CORRECT fix here depends on SnapshotPublisher ensuring serialization.
    // Assuming SnapshotPublisher::update is locked (it is), then 'publish'
    // is safe from other producers.
    //
    // BUT, the copy back (*back_ = *front_) is still conceptually wrong for a
    // true triple-buffer or history-based system, but for this "ping-pong"
    // state accumulation, we MUST copy the valid state forward to keep the
    // 'back' buffer up to date with what was just published.
    //
    // To fix the "race" mentioned in CONFLICTS.md:
    // The issue was multiple producers. SnapshotPublisher::update uses a mutex,
    // so `back()` and `publish()` are serialized.
    //
    // We just need to ensure the atomic swap is correct.

    // LOGGING DISABLED: Called many times/second, creates I/O overhead
    // util::Logger::debug("SnapshotBuffers::publish - START");

    // 1. Update sequence
    back_->seq = front_.load(std::memory_order_acquire)->seq + 1;
    // util::Logger::debug("SnapshotBuffers::publish - Updated seq to " + std::to_string(back_->seq));

    // 2. Swap pointers
    auto* old_front = front_.load(std::memory_order_relaxed);
    front_.store(back_, std::memory_order_release);
    back_ = old_front;
    // util::Logger::debug("SnapshotBuffers::publish - Swapped front/back buffers");

    // 3. CRITICAL: Re-sync the NEW back buffer with the NEW front buffer
    // This ensures the next producer starts with the latest state.
    *back_ = *front_.load(std::memory_order_acquire);
    // util::Logger::debug("SnapshotBuffers::publish - COMPLETE (seq=" + std::to_string(back_->seq) + ")");
}

const model::Snapshot& SnapshotBuffers::front() const {
    return *front_.load(std::memory_order_acquire);
}

uint64_t SnapshotBuffers::seq() const {
    return front().seq;
}

}  // namespace ouroboros::backend
