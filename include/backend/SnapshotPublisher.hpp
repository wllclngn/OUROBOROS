#pragma once

#include "model/Snapshot.hpp"
#include "backend/SnapshotBuffers.hpp"
#include <memory>
#include <functional>
#include <mutex>

namespace ouroboros::backend {

class SnapshotPublisher {
public:
    SnapshotPublisher();
    ~SnapshotPublisher();

    void publish(model::Snapshot snap);
    void update(std::function<void(model::Snapshot&)> updater);
    std::shared_ptr<const model::Snapshot> get_current() const;

private:
    mutable SnapshotBuffers buffers_;
    std::mutex mutex_;
};

}  // namespace ouroboros::backend
