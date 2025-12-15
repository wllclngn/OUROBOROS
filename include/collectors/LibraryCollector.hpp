#pragma once

#include "backend/SnapshotPublisher.hpp"
#include "model/Snapshot.hpp"
#include <memory>
#include <stop_token>

namespace ouroboros::collectors {

class LibraryCollector {
public:
    LibraryCollector(std::shared_ptr<backend::SnapshotPublisher> publisher);

    void run(std::stop_token stop_token);

private:
    std::shared_ptr<backend::SnapshotPublisher> publisher_;
};

}  // namespace ouroboros::collectors
