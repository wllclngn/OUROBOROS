#include "backend/SnapshotBuffers.hpp"

namespace ouroboros::backend {

SnapshotBuffers::SnapshotBuffers() {
    // Initialize default states. make_shared results move directly into the
    // members (no named locals): locals dying at scope end emit dead
    // refcount-dispose paths at -O3, which GCC 16's -Warray-bounds cross-wires
    // into a false positive. Both buffers share the same empty states.
    a_.library = std::make_shared<model::LibraryState>();
    a_.queue = std::make_shared<model::QueueState>();
    a_.seq = 0;

    b_.library = a_.library;
    b_.queue = a_.queue;
    b_.seq = 0;

    front_.store(&a_);
    back_ = &b_;
}

model::Snapshot& SnapshotBuffers::back() {
    // Only used by the producer holding the lock in SnapshotPublisher::update
    return *back_;
}

void SnapshotBuffers::publish() {
    // Caller (SnapshotPublisher::update/publish) holds the writer mutex, so this
    // runs single-writer; readers race only on the atomic front_ load in front().
    // The three steps below bump seq, atomically swap front, then re-sync the new
    // back buffer to the just-published state so the next producer accumulates
    // from the latest. seq is incremented before the swap, keeping it monotonic
    // for readers. See the invariant in SnapshotPublisher.hpp.

    // 1. Update sequence
    back_->seq = front_.load(std::memory_order_acquire)->seq + 1;

    // 2. Swap pointers
    auto* old_front = front_.load(std::memory_order_relaxed);
    front_.store(back_, std::memory_order_release);
    back_ = old_front;

    // 3. Re-sync the NEW back buffer with the NEW front buffer so the next
    // producer starts from the latest published state.
    *back_ = *front_.load(std::memory_order_acquire);
}

const model::Snapshot& SnapshotBuffers::front() const {
    return *front_.load(std::memory_order_acquire);
}

uint64_t SnapshotBuffers::seq() const {
    return front().seq;
}

}  // namespace ouroboros::backend
