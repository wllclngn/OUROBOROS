#pragma once

#include "model/Snapshot.hpp"
#include <atomic>
#include <memory>

namespace ouroboros::backend {

// Double-buffered snapshot store: two Snapshot slots (a_, b_), an atomic front
// pointer for lock-free readers, and a back pointer the producer writes.
//
// NOT internally synchronized against concurrent writers. SnapshotPublisher owns
// the only writer mutex and is the sole caller of back()/publish(); see the
// invariant documented there. back()/publish() must be called only under that
// lock. front() is the lock-free read path (atomic-pointer load) and is safe to
// call concurrently with a publish() in progress -- it returns either the old or
// the new snapshot whole, never a mix.
class SnapshotBuffers {
public:
    SnapshotBuffers();
    
    // Non-copyable
    SnapshotBuffers(const SnapshotBuffers&) = delete;
    SnapshotBuffers& operator=(const SnapshotBuffers&) = delete;

    // Get the back buffer for writing (producer)
    [[nodiscard]] model::Snapshot& back();

    // Publish: swap front/back buffers and increment sequence
    void publish();

    // Get the front buffer for reading (consumer)
    [[nodiscard]] const model::Snapshot& front() const;

    // Helper to get current sequence
    [[nodiscard]] uint64_t seq() const;

private:
    model::Snapshot a_;
    model::Snapshot b_;
    
    std::atomic<model::Snapshot*> front_;
    model::Snapshot* back_;
};

}  // namespace ouroboros::backend
