#pragma once

#include "model/Snapshot.hpp"
#include <atomic>
#include <memory>
#include <functional>
#include <mutex>

namespace ouroboros::backend {

// SYNCHRONIZATION (load-bearing -- read before touching this):
//
// PUBLISHED SNAPSHOTS ARE IMMUTABLE. Every publish builds a fresh Snapshot and
// installs it; nothing ever writes into a snapshot a reader can see. That is the
// whole invariant, and it is what makes the read path safe rather than merely
// atomic. A pointer swap alone is not enough: the pointer has to lead somewhere
// that never changes underneath.
//
// WRITERS are serialized on mutex_. All mutation goes through publish()/update(),
// which apply the change to a staging copy the readers cannot reach, then install
// a new immutable snapshot. Every queue and library change funnels through
// update(), so two writers never interleave. Callers MUST mutate shared state only
// inside an update() lambda; mutating a snapshot from get_current() is a bug --
// that handle is const and owns a retired copy.
//
// READERS are race-free and never block on a writer. get_current() is one atomic
// load returning an owning handle; the snapshot it names stays alive for as long
// as the caller holds it, however many publishes land meanwhile. Note that this
// costs the reader NOTHING to allocate -- the allocation moved to the publish
// side, which runs far less often than the render loop reads.
class SnapshotPublisher {
public:
    SnapshotPublisher();
    ~SnapshotPublisher();

    void publish(model::Snapshot snap);
    void update(std::function<void(model::Snapshot&)> updater);
    std::shared_ptr<const model::Snapshot> get_current() const;

private:
    // The staging copy writers accumulate into. Reachable only under mutex_.
    model::Snapshot staging_;
    std::atomic<std::shared_ptr<const model::Snapshot>> current_;
    mutable std::mutex mutex_;
};

}  // namespace ouroboros::backend
