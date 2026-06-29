#pragma once

#include "model/Snapshot.hpp"
#include "backend/SnapshotBuffers.hpp"
#include <memory>
#include <functional>
#include <mutex>

namespace ouroboros::backend {

// SYNCHRONIZATION (load-bearing -- read before touching this):
//
// WRITERS are serialized. All state mutation goes through publish()/update(),
// which hold mutex_. Every queue and library change in the app (enqueue, next,
// prev, JumpToQueueIndex, clear, library scan publish) funnels through update(),
// so two writers never interleave -- there is exactly one writer critical section.
// Callers MUST mutate shared state only inside an update() lambda; mutating a
// snapshot obtained from get_current() is a bug (that copy is not published state).
//
// READERS are NOT race-free against writers (KNOWN DEFECT). get_current() reads
// *front_ -- front_ is an atomic POINTER, but SnapshotBuffers alternates between
// just two Snapshot buffers and publish() re-syncs (mutates) them in place. So a
// reader dereferencing front_ can race a concurrent publish writing that same
// buffer: it may observe torn/stale fields, seq is not monotonic across concurrent
// reads, and copying a member shared_ptr mid-reassignment is itself UB. In practice
// the window is small (reads ~30/s, writes on playback events) so it has not been
// seen to crash, but it is a real data race, not a safe lock-free read.
// test_concurrency.cpp documents this; the fix (seqlock versioned read / triple
// buffer / brief shared lock on the read path) is an architectural decision tracked
// in ROADMAP.md. Do not document this as "safe" again -- it was, and the test
// proved otherwise.
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
