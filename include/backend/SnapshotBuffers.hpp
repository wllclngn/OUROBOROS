#pragma once

#include "model/Snapshot.hpp"
#include <atomic>
#include <memory>

namespace ouroboros::backend {

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
